/*
 * rgb.c - WS2812 RGB driver (SPI2 + DMA1 CH1, AT32F405). See rgb.h.
 *
 * Configuration:
 *   SYSCLK 216 MHz, APB1 108 MHz; SPI2 (on APB1) master, half-duplex TX,
 *   MCLK_DIV_16 -> 6.75 MHz bit clock (148 ns/bit). One WS2812 data bit = one
 *   8-bit SPI byte, MSB-first:
 *       logical 0 -> 0xC0 (11000000b): T0H ~ 2*148 = 296 ns
 *       logical 1 -> 0xF0 (11110000b): T1H ~ 4*148 = 593 ns
 *   Wire colour order GRB, 24 bytes/LED, 61 LEDs = 1464-byte frame.
 *   DMA1 channel 1, DMAMUX request SPI2_TX(0x0D), mem->periph byte, single shot
 *   (re-armed per frame); SPI2 CTRL2.DMATEN set once. Data pin PA10, AF mux 5.
 *
 * The >50 us WS2812 latch is produced by the inter-frame idle: both pattern
 * bytes end low, so MOSI idles low after a transfer, and frames are spaced by
 * the render tick (>> 50 us).
 */
#include "at32f402_405.h"
#include "rgb.h"

/* --- WS2812 bit-cell SPI byte patterns (MSB-first) --- */
#ifndef WS2812_BIT0
#define WS2812_BIT0   0xC0u      /* logical 0: 2 high bits */
#endif
#ifndef WS2812_BIT1
#define WS2812_BIT1   0xF0u      /* logical 1: 4 high bits */
#endif

#define WS2812_BYTES_PER_LED   24u
#define WS2812_FRAME_BYTES     MG_WS2812_BYTES   /* 1464 */

/* DMA-visible framebuffer (the bit-stream sent to SPI2). 0xC0 == all bits 0. */
static uint8_t fb[WS2812_FRAME_BYTES];
static volatile int g_inflight;        /* a DMA frame transfer is running */

/* expand one 8-bit colour channel into 8 SPI bytes, MSB-first */
static void encode_channel(uint8_t *dst, uint8_t c)
{
  unsigned bit;
  for (bit = 0; bit < 8; bit++)
    dst[bit] = (c & (uint8_t)(0x80u >> bit)) ? WS2812_BIT1 : WS2812_BIT0;
}

void rgb_init(void)
{
  gpio_init_type gpio_cfg;
  spi_init_type  spi_cfg;
  dma_init_type  dma_cfg;
  unsigned i;

  /* peripheral clocks */
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK,  TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK,  TRUE);

  /* PA10 -> SPI2 single-wire MOSI, AF5, push-pull, pull-up, strong drive */
  gpio_default_para_init(&gpio_cfg);
  gpio_cfg.gpio_pins           = GPIO_PINS_10;
  gpio_cfg.gpio_mode           = GPIO_MODE_MUX;
  gpio_cfg.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio_cfg.gpio_pull           = GPIO_PULL_UP;
  gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOA, &gpio_cfg);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE10, GPIO_MUX_5);

  /* SPI2: master, half-duplex TX, /16, 8-bit, MSB-first, CPOL=0 CPHA=2edge, SW CS */
  spi_default_para_init(&spi_cfg);
  spi_cfg.transmission_mode      = SPI_TRANSMIT_HALF_DUPLEX_TX;
  spi_cfg.master_slave_mode      = SPI_MODE_MASTER;
  spi_cfg.mclk_freq_division     = SPI_MCLK_DIV_16;     /* 108 MHz / 16 = 6.75 MHz */
  spi_cfg.first_bit_transmission = SPI_FIRST_BIT_MSB;
  spi_cfg.frame_bit_num          = SPI_FRAME_8BIT;
  spi_cfg.clock_polarity         = SPI_CLOCK_POLARITY_LOW;
  spi_cfg.clock_phase            = SPI_CLOCK_PHASE_2EDGE;
  spi_cfg.cs_mode_selection      = SPI_CS_SOFTWARE_MODE;
  spi_init(SPI2, &spi_cfg);
  spi_half_duplex_direction_set(SPI2, SPI_HALF_DUPLEX_DIRECTION_TX);
  spi_i2s_dma_transmitter_enable(SPI2, TRUE);          /* CTRL2.DMATEN = 1 */
  spi_enable(SPI2, TRUE);

  /* all-off framebuffer (every bit 0 -> every byte 0xC0) */
  for (i = 0; i < WS2812_FRAME_BYTES; i++) fb[i] = WS2812_BIT0;

  /* DMA1 channel 1: framebuffer -> SPI2->dt, byte, mem-increment, single shot */
  dma_reset(DMA1_CHANNEL1);
  dma_default_para_init(&dma_cfg);
  dma_cfg.buffer_size           = WS2812_FRAME_BYTES;
  dma_cfg.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma_cfg.memory_base_addr      = (uint32_t)fb;
  dma_cfg.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma_cfg.memory_inc_enable     = TRUE;
  dma_cfg.peripheral_base_addr  = (uint32_t)&SPI2->dt;
  dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma_cfg.peripheral_inc_enable = FALSE;
  dma_cfg.priority              = DMA_PRIORITY_HIGH;
  dma_cfg.loop_mode_enable      = FALSE;
  dma_init(DMA1_CHANNEL1, &dma_cfg);
  dmamux_init(DMA1MUX_CHANNEL1, DMAMUX_DMAREQ_ID_SPI2_TX);
  dmamux_enable(DMA1, TRUE);
}

int rgb_busy(void)
{
  if (!g_inflight) return 0;
  /* the full-data-transfer flag for channel 1 marks completion */
  if (dma_flag_get(DMA1_FDT1_FLAG) != RESET) { g_inflight = 0; return 0; }
  return 1;
}

void rgb_show(const led_frame_t frame)
{
  unsigned i;

  /* wait out any in-flight transfer so the framebuffer is not torn */
  {
    uint32_t spin = 2000000;          /* a frame is ~1.7 ms; bound it so a DMA fault cannot wedge the loop */
    while (rgb_busy() && --spin) { }
  }
  dma_channel_enable(DMA1_CHANNEL1, FALSE);
  dma_flag_clear(DMA1_FDT1_FLAG);
  dma_flag_clear(DMA1_HDT1_FLAG);
  dma_flag_clear(DMA1_DTERR1_FLAG);

  /* encode RGB frame -> GRB WS2812 bit-stream */
  for (i = 0; i < MG_NUM_LEDS; i++) {
    uint8_t *led = &fb[i * WS2812_BYTES_PER_LED];
    encode_channel(led + 0,  frame[i][1]);   /* G first on the wire */
    encode_channel(led + 8,  frame[i][0]);   /* R */
    encode_channel(led + 16, frame[i][2]);   /* B */
  }

  /* re-arm single-shot DMA */
  dma_data_number_set(DMA1_CHANNEL1, WS2812_FRAME_BYTES);
  dma_channel_enable(DMA1_CHANNEL1, TRUE);
  g_inflight = 1;
}
