/*
 * vendor_proto.h - Fun60 Ultra vendor feature-report protocol.
 *
 * Presents identically to the MonsGeek app and provides the SET/GET handlers and
 * persistent state needed to fully configure the board: LED parameters in the
 * per-mode table layout, per-key Static colours (USERPIC), keymatrix remap, and
 * per-key magnetism (actuation / RT / key-mode). SET handlers mark the config
 * dirty so the platform can flush it to internal flash (persist.c / flash_efc.c).
 *
 * Wire model (HID FEATURE report, ID 0, 64 bytes): report[0]=opcode, the first
 * 8 bytes are the checksummed header, byte 7 (Bit7) or byte 8 (Bit8) is the
 * checksum, bulk payload follows. The dispatcher operates on the report directly.
 */
#ifndef MONSGEEK_VENDOR_PROTO_H
#define MONSGEEK_VENDOR_PROTO_H

#include <stdint.h>
#include "led_layout.h"

#define MONSGEEK_REPORT_SIZE      64

/* Identity (MONSGEEK_INFO_DEVID_LO/HI, _VER_HI/LO) and MG_NUM_SITES are per-board
 * and come from the generated board_config.h (included here via led_layout.h). */

/* counts */
#define MG_NUM_MAG_KEYS     64u                 /* magnetism keys (2 pages x 32)  */
#define MG_MAG_KEYS_PER_PAGE 32u
#define MG_KEYMATRIX_BYTES  (MG_NUM_MAG_KEYS * 4u)  /* 4 bytes per key remap      */

/* LED per-mode table offsets, used by SET/GET_LEDPARAM */
#define MG_LED_TBL_SPEED    0x00u   /* +mode                                      */
#define MG_LED_TBL_BRIGHT   0x20u   /* +mode                                      */
#define MG_LED_TBL_OPTS     0x40u   /* +mode                                      */
#define MG_LED_TBL_RGB      0x82u   /* +mode*3 -> R,G,B                           */
#define MG_LED_MODE_MAX     0x1Fu   /* table index mask (32 modes)                */

/* Vendor opcodes (subset). */
enum {
  FEA_SET_RESET        = 0x01,
  FEA_SET_KBVALUE      = 0x02,
  FEA_SET_REPORT       = 0x03,
  FEA_SET_PROFILE      = 0x04,
  FEA_SET_LEDONOFF     = 0x05,
  FEA_SET_DEBOUNCE     = 0x06,
  FEA_SET_LEDPARAM     = 0x07,
  FEA_SET_SLEDPARAM    = 0x08,
  FEA_SET_KBOPTION     = 0x09,
  FEA_SET_KEYMATRIX    = 0x0A,
  FEA_SET_MACRO        = 0x0B,
  FEA_SET_USERPIC      = 0x0C,
  FEA_SET_FN           = 0x10,
  FEA_SET_SLEEPTIME    = 0x11,
  FEA_SET_MAG_REPORT   = 0x1B,
  FEA_SET_MAG_CAL      = 0x1C,
  FEA_SET_KEY_MAG_MODE = 0x1D,
  FEA_SET_MAG_MAXCAL   = 0x1E,
  FEA_SET_MULTI_MAG    = 0x65,
  FEA_ENTER_BOOTLOADER = 0x7F,
  FEA_GET_REV          = 0x80,
  FEA_GET_KBVALUE      = 0x82,
  FEA_GET_REPORT       = 0x83,
  FEA_GET_PROFILE      = 0x84,
  FEA_GET_LEDONOFF     = 0x85,
  FEA_GET_DEBOUNCE     = 0x86,
  FEA_GET_LEDPARAM     = 0x87,
  FEA_GET_SLEDPARAM    = 0x88,
  FEA_GET_KBOPTION     = 0x89,
  FEA_GET_KEYMATRIX    = 0x8A,
  FEA_GET_MACRO        = 0x8B,
  FEA_GET_USERPIC      = 0x8C,
  FEA_GET_INFOR        = 0x8F,
  FEA_GET_FN           = 0x90,
  FEA_GET_SLEEPTIME    = 0x91,
  FEA_GET_MAG_CAL      = 0x9C,
  FEA_GET_KEY_MAG_MODE = 0x9D,
  FEA_GET_MULTI_MAG    = 0xE5,
  FEA_GET_FEATURE_LIST = 0xE6,
  FEA_DIAG_RAW         = 0xF0,  /* bring-up only (not stock): raw ADC samples, see vendor_proto.c */
  FEA_DIAG_PRESSLOG    = 0xF1,  /* bring-up only: ordered first-press log, see main.c */
  FEA_DIAG_PRESSED     = 0xF2,  /* bring-up only: hall engine pressed[] bitmap */
  FEA_DIAG_GPIO_POKE   = 0xF3,  /* bring-up only: <port 0=A..5=F> <pin> <1=set 0=clr> (board hook) */
  FEA_DIAG_HWSTATUS    = 0xF4,  /* bring-up only: LED/DMA/SPI/GPIO snapshot (board hook) */
  FEA_LIVE_LEDS        = 0xF5,  /* live frames: <start led> <n<=18> .. RGB from [8]; shown in "Live" lighting (board hook) */
  FEA_LEDMAP_CTRL      = 0xF6,  /* LED->key mapping: <1 start | 0 cancel | 2 status> (board hook) */
  FEA_LEDMAP_READ      = 0xF7   /* read the LED->scan-site map: <start led> -> sites from [8] (board hook) */
};

/* Magnetism sub-commands (SET 0x65 / GET 0xE5). */
enum {
  MAG_SUB_PRESS_TRAVEL = 0x00,   /* actuation depth   (u16, centi-mm)            */
  MAG_SUB_LIFT_TRAVEL  = 0x01,   /* release depth     (u16)                      */
  MAG_SUB_RT_PRESS     = 0x02,   /* rapid-trigger press delta (u16)              */
  MAG_SUB_RT_LIFT      = 0x03,   /* rapid-trigger release delta (u16)            */
  MAG_SUB_KEY_MODE     = 0x07    /* per-key actuation mode (u8)                  */
};

/*
 * Live vendor settings. cfg[]/rgb[] keep the protocol's byte offsets so the
 * GET/SET handlers and the persisted image stay consistent; the extra blocks are
 * the per-key data the app can program.
 */
typedef struct {
  uint8_t  cfg[256];                     /* config header layout                 */
  uint8_t  rgb[256];                     /* LED per-mode table                   */
  uint8_t  userpic[MG_NUM_LEDS][3];      /* per-key Static colours (USERPIC 0x0C) */
  uint8_t  keymatrix[MG_KEYMATRIX_BYTES];/* active remap (KEYMATRIX 0x0A), 4B/key */
  uint16_t mag_press[MG_NUM_MAG_KEYS];   /* sub 0x00 actuation, centi-mm          */
  uint16_t mag_lift[MG_NUM_MAG_KEYS];    /* sub 0x01 release                      */
  uint16_t mag_rt_press[MG_NUM_MAG_KEYS];/* sub 0x02                              */
  uint16_t mag_rt_lift[MG_NUM_MAG_KEYS]; /* sub 0x03                              */
  uint8_t  mag_mode[MG_NUM_MAG_KEYS];    /* sub 0x07 per-key key mode             */
  uint8_t  mag_global_mode;              /* SET_KEY_MAG_MODE 0x1D global default   */
  uint8_t  cal_state;                    /* SET_MAG_CAL / MAXCAL latch            */
  uint8_t  mag_report_on;                /* SET_MAG_REPORT streaming toggle       */
  uint8_t  cfg_dirty;                    /* set by SETs; platform flushes to flash*/
  uint8_t  enter_bootloader;             /* set on a valid 0x7F 55 AA 55 AA       */
} monsgeek_state_t;

/* Initialise the settings with clean factory defaults: profile 0, 8 kHz poll,
 * LED on, default LED effect + brightness, ANSI keymap, 2.0 mm actuation,
 * Normal key mode. */
void monsgeek_state_init(monsgeek_state_t *st);

/* Checksum helpers (Bit7 default; Bit8 for LED 0x07/0x08). */
int  monsgeek_checksum_ok(const uint8_t *report);
void monsgeek_emit_bit7(uint8_t *report);
void monsgeek_emit_bit8(uint8_t *report);

/* Process one 64-byte feature report in place. Returns 1 if recognised (and the
 * response is written into report[]), 0 to ignore. Sets st->cfg_dirty when a
 * SET changes persistent settings. */
int monsgeek_vendor_dispatch(monsgeek_state_t *st, uint8_t *report);

/* Decode the currently-selected LED effect (cfg[8] + rgb table) into engine
 * parameters. Returns the "LED enabled" flag (cfg[4] bit4): 0 => render Off. */
int monsgeek_get_led_params(const monsgeek_state_t *st, led_params_t *out);

/* Bring-up diagnostics, echoed in GET_INFOR bytes 3..6 (stock leaves them 0):
 * byte 3 = last boot stage reached (see main.c), bytes 4..6 = main-loop pass count (u24 LE). */
extern volatile uint8_t  g_boot_stage;
extern volatile uint32_t g_loop_count;
/* raw ADC frame for FEA_DIAG_RAW (set by main.c) and the ADC-timeout counter (keyscan.c) */
extern volatile const uint16_t *g_diag_raw;
extern volatile uint16_t g_diag_n;
extern volatile uint32_t g_adc_timeouts;
/* press log (DIAG_NO_HID builds): site indices in first-press order */
extern volatile uint8_t  g_presslog[128];
extern volatile uint8_t  g_presslog_n;
extern volatile const uint8_t *g_diag_pressed;   /* hall engine output (main.c) */
/* hardware diagnostics hook: fills report, returns 1 if handled (main.c; weak no-op default) */
int board_diag_cmd(uint8_t *report);

#endif /* MONSGEEK_VENDOR_PROTO_H */
