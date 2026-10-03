/*
 * led_layout.h - Fun60 Ultra RGB geometry + LED-protocol constants.
 *
 * The board carries 61 per-key WS2812 LEDs on a single SPI2-driven chain (see
 * rgb.c). The framebuffer is 1464 bytes = 61 LEDs x 3 colour bytes x 8 SPI bytes-
 * per-bit (one SPI byte encodes one WS2812 data bit). This header fixes the counts
 * and the LED parameter model shared by the pure effect engine (led_effects.c) and
 * the SPI driver (rgb.c).
 */
#ifndef LED_LAYOUT_H
#define LED_LAYOUT_H

#include <stdint.h>
#include "board_config.h"

/* MG_NUM_LEDS, MG_LED_MAX_COL and MG_LED_MAX_ROW are per-board (board_config.h). */
#define MG_WS2812_BYTES    (MG_NUM_LEDS * 24u) /* framebuffer size on wire         */

/* Physical geometry: chain index (0..60, WS2812 order) -> physical (col,row),
 * following the board's serpentine LED matrix (6 rows x 16 cols, 0xFF = no LED).
 * Used so Wave / per-key effects map to the real board layout instead of raw
 * chain order. See led_layout.c. */
extern const uint8_t led_phys_col[MG_NUM_LEDS];   /* 0..14 */
extern const uint8_t led_phys_row[MG_NUM_LEDS];   /* 1..5  */

/* 8-entry colour palette (R,G,B): red, green, blue, orange, magenta, yellow,
 * gray, black. */
extern const uint8_t led_palette[8][3];

/* LED effect identifiers, matching the MonsGeek app mode table (0..25). Only the
 * ones the engine renders distinctly are named; the rest fall back to Static so
 * the board still lights with the configured colour. */
enum {
  LED_MODE_OFF        = 0,   /* all LEDs dark                                    */
  LED_MODE_STATIC     = 1,   /* constant single colour ("Constant")              */
  LED_MODE_BREATHING  = 2,   /* single colour, brightness breathes              */
  LED_MODE_NEON       = 3,
  LED_MODE_WAVE       = 4,   /* hue gradient scrolling across the chain          */
  LED_MODE_RIPPLE     = 5,
  LED_MODE_RAINBOW    = 16,  /* full-board rainbow cycle                         */
  LED_MODE_USERPIC    = 13,  /* per-key colours from SET_USERPIC 0x0C            */
  LED_MODE_PERKEY     = 25,  /* per-key colours / GIF (also userpic-backed)      */
  LED_MODE_RANDOM_RAINBOW = 0x40, /* firmware-only: every key cycles hue from its own random start */
  LED_MODE_LIVE       = 0x41 /* firmware-only: show host-streamed frames (FEA_LIVE_LEDS); main.c */
};

/* options byte: (direction << 4) | flag.
 *   flag 7 = NORMAL  (single configured colour)
 *   flag 8 = DAZZLE  (rainbow / multi-colour, ignores R/G/B)                    */
#define LED_OPT_FLAG_NORMAL  7u
#define LED_OPT_FLAG_DAZZLE  8u

/* Decoded LED parameters fed to the effect engine. These mirror the per-mode
 * table the vendor protocol stores (SET_LEDPARAM 0x07 / GET 0x87):
 *   wire: [mode][speed][brightness][options][R][G][B]
 * speed/brightness are the raw wire values (0..4). The engine treats higher
 * speed as faster and higher brightness as brighter; the app's own
 * "wire = 4 - actual" inversion is cosmetic and handled host-side, so the wire
 * byte is stored/returned verbatim for an exact round-trip. */
typedef struct {
  uint8_t mode;        /* LED_MODE_*                                            */
  uint8_t speed;       /* 0..4 (0 slowest .. 4 fastest as rendered)            */
  uint8_t brightness;  /* 0..4                                                 */
  uint8_t direction;   /* options >> 4                                          */
  uint8_t flag;        /* options & 0x0F : 7 NORMAL, 8 DAZZLE                  */
  uint8_t r, g, b;     /* configured colour (NORMAL modes)                     */
} led_params_t;

#endif /* LED_LAYOUT_H */
