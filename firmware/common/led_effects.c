/*
 * led_effects.c - pure RGB effect engine. See led_effects.h.
 *
 * Implements Off / Static / Breathing / Wave / Rainbow + per-key Static
 * (UserPicture). All other protocol modes (Neon, Ripple, Snake, ...) fall back
 * to Static so the board still lights with the configured colour rather than
 * going dark - this keeps the device "feature-complete" from the app's point of
 * view (every mode the UI offers produces light) while leaving the exotic
 * animations as a clearly-scoped bring-up item.
 */
#include "led_effects.h"

/* brightness wire value 0..4 -> 0..255 multiplier. Never fully 0 (a configured,
 * non-Off effect at brightness 0 should still glow dimly). */
static const uint8_t k_bright[5] = { 40, 90, 140, 195, 255 };

/* speed wire value 0..4 -> phase increment per millisecond (Q8: 256 = 1 unit/s).
 * Higher index = faster animation. */
static const uint16_t k_speed_q8[5] = { 8, 16, 32, 64, 128 };

uint8_t led_scale8(uint8_t c, uint8_t bright)
{
  return (uint8_t)(((uint16_t)c * (uint16_t)bright + 128u) / 255u);
}

void led_hsv2rgb(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
  /* Integer HSV. region 0..5 over the 0..255 hue circle. */
  uint8_t region = (uint8_t)(h / 43u);
  uint8_t rem    = (uint8_t)((h - region * 43u) * 6u);   /* 0..255 within region */
  uint8_t p = (uint8_t)(((uint16_t)v * (255u - s)) / 255u);
  uint8_t q = (uint8_t)(((uint16_t)v * (255u - (uint16_t)(((uint16_t)s * rem) / 255u))) / 255u);
  uint8_t t = (uint8_t)(((uint16_t)v * (255u - (uint16_t)(((uint16_t)s * (255u - rem)) / 255u))) / 255u);
  switch (region) {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
  }
}

/* Triangle wave 0..255..0 from a phase 0..255 (used by Breathing). */
static uint8_t triangle8(uint8_t phase)
{
  return (phase < 128u) ? (uint8_t)(phase * 2u)
                        : (uint8_t)((255u - phase) * 2u);
}

static uint8_t phase8(uint32_t t_ms, uint8_t speed)
{
  uint8_t s = (speed > 4u) ? 4u : speed;
  /* phase advances k_speed_q8[s]/256 units per ms; wrap at 256 (Q8). */
  return (uint8_t)((t_ms * k_speed_q8[s] / 256u) & 0xFFu);
}

void led_effects_render(const led_params_t *p,
                        const uint8_t (*userpic)[3],
                        uint32_t t_ms,
                        led_frame_t out)
{
  unsigned i;
  uint8_t bri = k_bright[(p->brightness > 4u) ? 4u : p->brightness];

  switch (p->mode) {

  case LED_MODE_OFF:
    for (i = 0; i < MG_NUM_LEDS; i++) { out[i][0] = out[i][1] = out[i][2] = 0; }
    return;

  case LED_MODE_BREATHING: {
    /* single colour, brightness modulated by a triangle of the time phase */
    uint8_t ph  = phase8(t_ms, p->speed);
    uint8_t env = triangle8(ph);
    uint8_t v   = led_scale8(bri, env);
    uint8_t r, g, b;
    if (p->flag == LED_OPT_FLAG_DAZZLE) {
      /* dazzle breathing: hue also drifts so it isn't a fixed colour */
      led_hsv2rgb(ph, 255, v, &r, &g, &b);
    } else {
      r = led_scale8(p->r, v); g = led_scale8(p->g, v); b = led_scale8(p->b, v);
    }
    for (i = 0; i < MG_NUM_LEDS; i++) { out[i][0] = r; out[i][1] = g; out[i][2] = b; }
    return;
  }

  case LED_MODE_WAVE: {
    /* hue gradient across the physical board, scrolling with time. direction
     * picks the sweep axis/sense (bit1: rows vs cols, bit0: reverse), a
     * 4-direction Wave. Uses the serpentine geometry so the front moves
     * left->right / top->bottom on the real layout, not chain order. */
    uint8_t ph = phase8(t_ms, p->speed);
    for (i = 0; i < MG_NUM_LEDS; i++) {
      uint8_t pos = (p->direction & 2u)
                    ? (uint8_t)((led_phys_row[i] * 255u) / MG_LED_MAX_ROW)
                    : (uint8_t)((led_phys_col[i] * 255u) / MG_LED_MAX_COL);
      uint8_t hue = (p->direction & 1u) ? (uint8_t)(ph - pos) : (uint8_t)(ph + pos);
      uint8_t r, g, b;
      led_hsv2rgb(hue, 255, bri, &r, &g, &b);
      out[i][0] = r; out[i][1] = g; out[i][2] = b;
    }
    return;
  }

  case LED_MODE_RAINBOW: {
    /* whole board one hue, cycling over time */
    uint8_t hue = phase8(t_ms, p->speed);
    uint8_t r, g, b;
    led_hsv2rgb(hue, 255, bri, &r, &g, &b);
    for (i = 0; i < MG_NUM_LEDS; i++) { out[i][0] = r; out[i][1] = g; out[i][2] = b; }
    return;
  }

  case LED_MODE_RANDOM_RAINBOW: {
    /* each LED starts at its own pseudo-random hue and drifts at its own rate
     * (75..150% of the base speed), so the board shimmers instead of moving as
     * one wave. Deterministic per LED: no RNG state to keep. */
    for (i = 0; i < MG_NUM_LEDS; i++) {
      uint32_t h = (uint32_t)i * 2654435761u;          /* Knuth multiplicative hash */
      uint8_t start = (uint8_t)(h >> 24);
      uint32_t rate = 3u + ((h >> 8) & 3u);             /* 3..6 quarters */
      uint8_t hue = (uint8_t)(start + phase8(t_ms * rate / 4u, p->speed));
      uint8_t r, g, b;
      led_hsv2rgb(hue, 255, bri, &r, &g, &b);
      out[i][0] = r; out[i][1] = g; out[i][2] = b;
    }
    return;
  }

  case LED_MODE_USERPIC:
  case LED_MODE_PERKEY:
    /* per-key Static: each LED shows its stored colour, scaled by brightness */
    for (i = 0; i < MG_NUM_LEDS; i++) {
      if (userpic) {
        out[i][0] = led_scale8(userpic[i][0], bri);
        out[i][1] = led_scale8(userpic[i][1], bri);
        out[i][2] = led_scale8(userpic[i][2], bri);
      } else {
        out[i][0] = out[i][1] = out[i][2] = 0;
      }
    }
    return;

  case LED_MODE_STATIC:
  default: {
    /* constant single colour (also the fallback for unimplemented animations) */
    uint8_t r = led_scale8(p->r, bri);
    uint8_t g = led_scale8(p->g, bri);
    uint8_t b = led_scale8(p->b, bri);
    for (i = 0; i < MG_NUM_LEDS; i++) { out[i][0] = r; out[i][1] = g; out[i][2] = b; }
    return;
  }
  }
}
