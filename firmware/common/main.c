/*
 * main.c - MonsGeek Fun60 Ultra firmware (wired + optional wireless).
 *
 * Builds on the USB device stack + the analog key-scan / Hall engine so the
 * board boots, types, lights up and is fully configurable by the MonsGeek app:
 *
 *   keyscan_frame()  -> raw[84] ADC samples
 *   hall_process()   -> pressed[84]   (per-key actuation from the magnetism cfg)
 *   hid_build_boot_report() + usbd_ept_send(0x81)
 *   led_effects_render() + rgb_show()  -> WS2812 RGB (SPI2 + DMA1, see rgb.c)
 *   persist_load/save()                -> config survives reboot (EFC, see persist.c)
 *
 * The vendor feature-report path (vendor_proto.c) round-trips every exposed
 * setting; SET handlers mark the config dirty and the main loop flushes it to
 * internal flash. Clean factory defaults: ANSI keymap, 2.0 mm actuation, Normal
 * mode, Wave lighting at full brightness.
 *
 * Links into the application slot @0x08005200 behind the factory bootloader.
 */
#include "at32f402_405.h"
#include "at32f402_405_clock.h"
#include "board.h"
#include "usb_conf.h"
#include "usb_core.h"
#include "usbd_int.h"
#include "usbd_core.h"
#include "monsgeek_class.h"
#include "monsgeek_desc.h"
#include "descriptors.h"

#include "keyscan.h"
#include "hall.h"
#include "hid_report.h"
#include "board_keymap.h"

#include "vendor_proto.h"
#include "led_effects.h"
#include "rgb.h"
#include "persist.h"
#include "wireless.h"

otg_core_type otg_core_struct;

/* Link mode latched at boot from PC13: 0 = wired/USB, 1 = wireless 2.4G/BLE. */
static int g_wireless = 0;

static hall_engine_t  hall;
static uint16_t       raw[KS_NUM_KEYS];
static uint8_t        pressed[KS_NUM_KEYS];
/* 4-sample moving average per site, fed to the hall engine. At ~1150 scans/s
 * this adds ~1.7 ms mean latency and roughly halves sensor noise, which made
 * fine rapid-trigger deltas (0.04 mm ~ 11 counts vs up to 8 counts of noise)
 * double-fire. raw[] stays unfiltered for the diag/mapping paths. */
#define FILT_N 4u
static uint16_t       filt_hist[FILT_N][KS_NUM_KEYS];
static uint16_t       filt[KS_NUM_KEYS];
static uint8_t        filt_pos;
static uint8_t        active_map[KS_NUM_KEYS];   /* base/Fn layer resolved per frame */
static uint8_t        profile_map[KS_NUM_KEYS];  /* base + active profile's layer */
static uint8_t        profile_map_for = 0xFF;
static uint8_t        report[HID_BOOT_REPORT_LEN];
static uint8_t        last_report[HID_BOOT_REPORT_LEN];
static led_frame_t    frame;

#define LED_TICK_MS        16u     /* ~60 Hz RGB refresh                        */
#define SAVE_DEBOUNCE_MS   400u    /* batch rapid SETs, save when settled       */

/* monotonic millisecond clock from the DWT cycle counter (handles 32-bit wrap
 * as long as it is sampled far more often than ~19.9 s, which the loop does). */
static uint32_t now_ms(void)
{
  static uint32_t last_cyc = 0, acc_ms = 0, rem = 0;
  uint32_t per_ms = system_core_clock / 1000u;
  uint32_t cyc = DWT->CYCCNT;
  uint32_t d = cyc - last_cyc;
  last_cyc = cyc;
  rem += d;
  acc_ms += rem / per_ms;
  rem %= per_ms;
  return acc_ms;
}

/* Map the per-key magnetism config (app-programmable) onto the Hall engine.
 * raw units are centi-mm (raw 200 = 2.00 mm). The magnetism arrays cover 64
 * keys; physical scan sites beyond that reuse the last entry (best-effort - the
 * exact site<->magnetism-key index map is a hardware bring-up item). */
static void apply_magnetism(const monsgeek_state_t *st, hall_engine_t *e)
{
  unsigned i;
  for (i = 0; i < KS_NUM_KEYS; i++) {
    unsigned k = (i < MG_NUM_MAG_KEYS) ? i : (MG_NUM_MAG_KEYS - 1u);
    uint16_t press = st->mag_press[k];
    uint16_t lift  = st->mag_lift[k];
    /* Hall release point must sit below the actuation point for hysteresis; the
     * app "lift travel" is used when it does, otherwise a sane margin is kept. */
    uint16_t rel = (lift < press) ? lift : (press >= 50u ? (uint16_t)(press - 50u)
                                                          : (uint16_t)(press / 2u));
    e->cfg[i].mode           = st->mag_mode[k];
    e->cfg[i].press_cmm      = press;
    e->cfg[i].release_cmm    = rel;
    e->cfg[i].rt_press_cmm   = st->mag_rt_press[k];
    e->cfg[i].rt_release_cmm = st->mag_rt_lift[k];
  }
}

/* Per-profile lighting so each profile is recognisable at a glance (cfg[0x00]
 * = active profile, cycled by KC_PROFILE_NEXT). Fields: mode, speed, brightness
 * (0..4), direction, flag (7 = single colour, 8 = dazzle), r, g, b. */
#define PROFILE_COUNT 3u
static const led_params_t profile_led[PROFILE_COUNT] = {
  { LED_MODE_WAVE,      3, 3, 0, 8, 0x00, 0x00, 0x00 },   /* 1: rainbow wave     */
  { LED_MODE_STATIC,    0, 3, 0, 7, 0xFF, 0x00, 0x00 },   /* 2: solid red        */
  { LED_MODE_STATIC,    0, 3, 0, 7, 0x00, 0x40, 0xFF },   /* 3: solid blue       */
};

/* KC_LIGHT_NEXT (Fn+\) cycles the active profile's effect through this list,
 * keeping the profile's colour. cfg[0xF0 + profile] holds the choice:
 * 0 = the profile's preset above, k = light_cycle[k-1]. (cfg 0xF0.. is unused by
 * the vendor protocol and is persisted with the rest of cfg[].) */
#define CFG_LIGHT_BASE 0xF0u
static const uint8_t light_cycle[] = {
  LED_MODE_WAVE, LED_MODE_STATIC, LED_MODE_BREATHING, LED_MODE_RAINBOW,
  LED_MODE_RANDOM_RAINBOW,
  LED_MODE_NEON, LED_MODE_RIPPLE, LED_MODE_USERPIC /* "Custom": per-key colours */,
  LED_MODE_LIVE /* host-streamed frames */,
  LED_MODE_OFF,
};
#define LIGHT_CYCLE_N (sizeof light_cycle / sizeof light_cycle[0])

static void light_next(monsgeek_state_t *st)
{
  uint8_t p = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  uint8_t v = st->cfg[CFG_LIGHT_BASE + p], cur = 0;
  if (v >= 1 && v <= LIGHT_CYCLE_N) cur = (uint8_t)(v - 1);
  else for (uint8_t k = 0; k < LIGHT_CYCLE_N; k++)
         if (light_cycle[k] == profile_led[p].mode) { cur = k; break; }
  st->cfg[CFG_LIGHT_BASE + p] = (uint8_t)((cur + 1u) % LIGHT_CYCLE_N + 1u);
  st->cfg_dirty = 1;
}

static volatile uint32_t g_led_frames;   /* rgb_show() calls, for FEA_DIAG_HWSTATUS */

/* Live frames (FEA_LIVE_LEDS): the host streams colours per LED index. They are
 * shown only while the active profile's lighting is "Live" (LED_MODE_LIVE in the
 * Fn+\ cycle); the last frame is held when the stream stops. */
static uint8_t           g_live[MG_NUM_LEDS][3];

/* LED->key mapping (FEA_LEDMAP_CTRL): LED g_map_led is lit white; the next key
 * press records its scan site as that LED's key, then the next LED lights.
 * Presses never reach the host while mapping. 0xFF = unmapped. */
static uint8_t           g_ledmap[MG_NUM_LEDS];
static volatile uint8_t  g_map_state;    /* 0 idle, 1 mapping, 2 done */
static volatile uint8_t  g_map_led;
static volatile uint8_t  g_map_reset;    /* set on start: re-learn the idle baseline */

/* Bring-up hardware hooks behind FEA_DIAG_GPIO_POKE / FEA_DIAG_HWSTATUS. */
static gpio_type *const diag_port[6] = { GPIOA, GPIOB, GPIOC, GPIOD, 0, GPIOF };
int board_diag_cmd(uint8_t *r)
{
  if (r[0] == FEA_LIVE_LEDS) {
    uint8_t start = r[1], n = r[2] > 18 ? 18 : r[2];
    for (uint8_t i = 0; i < n && (unsigned)(start + i) < MG_NUM_LEDS; i++) {
      g_live[start + i][0] = r[8 + 3 * i];
      g_live[start + i][1] = r[9 + 3 * i];
      g_live[start + i][2] = r[10 + 3 * i];
    }
    return 1;
  }
  if (r[0] == FEA_LEDMAP_CTRL) {
    if (r[1] == 1) { for (unsigned i = 0; i < MG_NUM_LEDS; i++) g_ledmap[i] = 0xFF; g_map_led = 0; g_map_reset = 1; g_map_state = 1; }
    else if (r[1] == 0) g_map_state = 0;
    r[1] = g_map_state; r[2] = g_map_led; r[3] = MG_NUM_LEDS;
    return 1;
  }
  if (r[0] == FEA_LEDMAP_READ) {
    uint8_t start = r[1], n = 0;
    while (n < 56 && (unsigned)(start + n) < MG_NUM_LEDS) { r[8 + n] = g_ledmap[start + n]; n++; }
    r[2] = n; r[3] = g_map_state;
    return 1;
  }
  if (r[0] == FEA_DIAG_GPIO_POKE) {
    gpio_type *g = r[1] < 6 ? diag_port[r[1]] : 0;
    if (!g || r[2] > 15) return 0;
    if (r[3]) g->scr = 1u << r[2]; else g->clr = 1u << r[2];
    return 1;
  }
  /* FEA_DIAG_HWSTATUS: [4..7] led frames, [8..11] DMA1 ch1 dtcnt, [12..15] SPI2 sts,
   * [16..39] odt of A,B,C,D,F + idt of A (u32 LE each) */
  uint32_t v[10] = { g_led_frames, DMA1_CHANNEL1->dtcnt, SPI2->sts,
                     GPIOA->odt, GPIOB->odt, GPIOC->odt, GPIOD->odt, GPIOF->odt, GPIOA->idt, 0 };
  for (unsigned i = 0; i < 9; i++)
    for (unsigned b = 0; b < 4; b++) r[4 + 4 * i + b] = (uint8_t)(v[i] >> (8 * b));
  return 1;
}

/* Per-profile animation speed (Fn+; slower, Fn+' faster). cfg[0xF4 + profile]:
 * 0 = the profile/preset default, s+1 = speed s (0..4). Persisted with cfg[]. */
#define CFG_SPEED_BASE 0xF4u
static uint8_t current_speed(const monsgeek_state_t *st, uint8_t def)
{
  uint8_t p = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  uint8_t v = st->cfg[CFG_SPEED_BASE + p];
  return (v >= 1 && v <= 5) ? (uint8_t)(v - 1) : (def > 4 ? 4 : def);
}
static void speed_step(monsgeek_state_t *st, int dir)
{
  uint8_t p = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  int s = current_speed(st, profile_led[p].mode == LED_MODE_STATIC ? 3 : profile_led[p].speed) + dir;
  if (s < 0) s = 0;
  if (s > 4) s = 4;
  st->cfg[CFG_SPEED_BASE + p] = (uint8_t)(s + 1);
  st->cfg_dirty = 1;
}

/* Per-profile colour for the single-colour modes (Fn+; / Fn+' while in Static,
 * Breathing, Neon or Ripple). cfg[0xF8 + profile]: 0 = profile default,
 * k = colour_palette[k-1]. Persisted with cfg[]. */
#define CFG_COLOUR_BASE 0xF8u
static const uint8_t colour_palette[][3] = {
  {0xFF,0x00,0x00}, {0xFF,0x50,0x00}, {0xFF,0xC8,0x00}, {0x90,0xFF,0x00}, {0x00,0xFF,0x00},
  {0x00,0xFF,0x80}, {0x00,0xDC,0xFF}, {0x00,0x80,0xFF}, {0x00,0x20,0xFF}, {0x60,0x00,0xFF},
  {0xA0,0x00,0xFF}, {0xFF,0x00,0xC8}, {0xFF,0xFF,0xFF},
};
#define COLOUR_N (sizeof colour_palette / sizeof colour_palette[0])
static int fx_is_single_colour(uint8_t mode)
{
  return mode == LED_MODE_STATIC || mode == LED_MODE_BREATHING ||
         mode == LED_MODE_NEON   || mode == LED_MODE_RIPPLE;
}
static void colour_step(monsgeek_state_t *st, int dir)
{
  uint8_t p = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  uint8_t v = st->cfg[CFG_COLOUR_BASE + p];
  int k = (v >= 1 && v <= COLOUR_N) ? (int)v - 1 : (dir > 0 ? -1 : 0);
  k = (k + dir + (int)COLOUR_N) % (int)COLOUR_N;
  st->cfg[CFG_COLOUR_BASE + p] = (uint8_t)(k + 1);
  st->cfg_dirty = 1;
}

/* ---- status guide on the top two rows -------------------------------------
 * Number row 1..0 = profiles, Q..P = the lighting modes (light_cycle order).
 * On Fn+Tab / Fn+\ the new selection's key flashes purple 3 times (with the
 * row shown as a guide: available = dim white, unused = dark); while Fn is
 * held both rows show the guide with the current selections solid purple. */
#define GUIDE_FLASH_MS   150u                   /* on/off half-period            */
#define GUIDE_FLASHES    3u
static const uint8_t guide_purple[3] = { 0xA0, 0x00, 0xFF };
static int8_t  guide_prof_led[10], guide_fx_led[10];   /* LED index or -1 */
static int8_t  guide_speed_led[10];                    /* A S D F G = speed 0..4 */
static int8_t  rt_flash_led = -1;                      /* Space: rapid-trigger toggle feedback */
static uint32_t g_guide_t0;                     /* start of the current flash    */
static uint8_t  g_guide_row = 0xFF;             /* 0 profiles, 1 modes, 0xFF none */
static uint8_t  g_fn_held;

static int led_of_code(uint8_t code)
{
  for (unsigned s = 0; s < KS_NUM_KEYS; s++) {
    if (keymap_default[s] != code) continue;
    for (unsigned i = 0; i < LED_SITE_N && i < MG_NUM_LEDS; i++)
      if (led_site[i] == s) return (int)i;
  }
  return -1;
}

static void guide_init(void)
{
  static const uint8_t qp[10] = { 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13 };
  for (unsigned k = 0; k < 10; k++) {
    guide_prof_led[k] = (int8_t)led_of_code((uint8_t)(0x1E + k));   /* 1..9, 0 */
    guide_fx_led[k]   = (int8_t)led_of_code(qp[k]);
  }
  rt_flash_led = (int8_t)led_of_code(0x2C);   /* Space */
  static const uint8_t asdfg[5] = { 0x04, 0x16, 0x07, 0x09, 0x0A };
  for (unsigned k = 0; k < 10; k++) guide_speed_led[k] = k < 5 ? (int8_t)led_of_code(asdfg[k]) : -1;
}

static uint8_t current_fx(const monsgeek_state_t *st)
{
  uint8_t p = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  uint8_t v = st->cfg[CFG_LIGHT_BASE + p];
  if (v >= 1 && v <= LIGHT_CYCLE_N) return (uint8_t)(v - 1);
  for (uint8_t k = 0; k < LIGHT_CYCLE_N; k++) if (light_cycle[k] == profile_led[p].mode) return k;
  return 0;
}

static void guide_row(led_frame_t f, const int8_t *leds, unsigned avail, unsigned sel, int sel_on)
{
  for (unsigned k = 0; k < 10; k++) {
    int i = leds[k];
    if (i < 0) continue;
    if (k == sel) {
      f[i][0] = sel_on ? guide_purple[0] : 0; f[i][1] = sel_on ? guide_purple[1] : 0;
      f[i][2] = sel_on ? guide_purple[2] : 0;
    } else if (k < avail) {
      f[i][0] = f[i][1] = f[i][2] = 0x18;       /* available: dim white */
    } else {
      f[i][0] = f[i][1] = f[i][2] = 0;          /* unused slot: dark */
    }
  }
}

static void guide_overlay(const monsgeek_state_t *st, uint32_t t_ms, led_frame_t f)
{
  uint8_t prof = st->cfg[0x00] < PROFILE_COUNT ? st->cfg[0x00] : 0;
  if (g_guide_row != 0xFF) {
    uint32_t dt = t_ms - g_guide_t0;
    if (dt >= 2u * GUIDE_FLASH_MS * GUIDE_FLASHES) g_guide_row = 0xFF;
    else {
      int on = ((dt / GUIDE_FLASH_MS) & 1u) == 0;
      if (g_guide_row == 0)      guide_row(f, guide_prof_led, PROFILE_COUNT, prof, on);
      else if (g_guide_row == 1) guide_row(f, guide_fx_led, LIGHT_CYCLE_N, current_fx(st), on);
      else guide_row(f, guide_speed_led, 5,
                     current_speed(st, profile_led[prof].mode == LED_MODE_STATIC ? 3 : profile_led[prof].speed), on);
      return;
    }
  }
  if (g_fn_held) {
    guide_row(f, guide_prof_led, PROFILE_COUNT, prof, 1);
    guide_row(f, guide_fx_led, LIGHT_CYCLE_N, current_fx(st), 1);
  }
}

static void guide_flash(uint8_t row, uint32_t now) { g_guide_row = row; g_guide_t0 = now; }

/* Rapid-trigger toggle feedback: Space flashes red x3 (on) or blue x3 (off). */
static uint32_t rt_flash_t0;
static uint8_t  rt_flash_active, rt_flash_on;
static void rt_flash(uint8_t on, uint32_t now) { rt_flash_on = on; rt_flash_t0 = now; rt_flash_active = 1; }
static void rt_flash_overlay(uint32_t t_ms, led_frame_t f)
{
  if (!rt_flash_active || rt_flash_led < 0) return;
  uint32_t dt = t_ms - rt_flash_t0;
  if (dt >= 2u * GUIDE_FLASH_MS * GUIDE_FLASHES) { rt_flash_active = 0; return; }
  int lit = ((dt / GUIDE_FLASH_MS) & 1u) == 0;
  f[rt_flash_led][0] = (lit && rt_flash_on) ? 0xFF : 0;
  f[rt_flash_led][1] = 0;
  f[rt_flash_led][2] = (lit && !rt_flash_on) ? 0xFF : 0;
}

/* Render the active profile's effect into the WS2812 chain (Off if LED disabled). */
static void render_leds(const monsgeek_state_t *st, uint32_t t_ms)
{
  led_params_t lp;
  int led_on = monsgeek_get_led_params(st, &lp);
  if (st->cfg[0x00] < PROFILE_COUNT) {
    uint8_t p = st->cfg[0x00], v = st->cfg[CFG_LIGHT_BASE + p];
    lp = profile_led[p];
    if (v >= 1 && v <= LIGHT_CYCLE_N) {
      lp.mode  = light_cycle[v - 1];
      lp.speed = 3;
      /* rainbow-type effects use the dazzle flag; single-colour ones keep the
       * profile colour (profile 1 has none, so give it white) */
      lp.flag  = (lp.mode == LED_MODE_WAVE || lp.mode == LED_MODE_RAINBOW ||
                  lp.mode == LED_MODE_RANDOM_RAINBOW) ? 8 : 7;
      if (lp.flag == 7 && !(lp.r | lp.g | lp.b)) lp.r = lp.g = lp.b = 0xFF;
    }
    lp.speed = current_speed(st, lp.speed);
    {
      uint8_t c = st->cfg[CFG_COLOUR_BASE + p];
      if (c >= 1 && c <= COLOUR_N) {
        lp.r = colour_palette[c - 1][0]; lp.g = colour_palette[c - 1][1]; lp.b = colour_palette[c - 1][2];
      }
    }
  }
  if (!led_on) lp.mode = LED_MODE_OFF;
  if (g_map_state == 1) {                       /* mapping: only the current LED */
    for (unsigned i = 0; i < MG_NUM_LEDS; i++) frame[i][0] = frame[i][1] = frame[i][2] = 0;
    if (g_map_led < MG_NUM_LEDS) frame[g_map_led][0] = frame[g_map_led][1] = frame[g_map_led][2] = 0xFF;
  } else if (lp.mode == LED_MODE_LIVE && led_on) {
    for (unsigned i = 0; i < MG_NUM_LEDS; i++) {
      frame[i][0] = g_live[i][0]; frame[i][1] = g_live[i][1]; frame[i][2] = g_live[i][2];
    }
  } else {
    led_effects_render(&lp, st->userpic, t_ms, frame);
  }
  if (g_map_state != 1) { guide_overlay(st, t_ms, frame); rt_flash_overlay(t_ms, frame); }
  rgb_show(frame);
  g_led_frames++;
}

/* Send on an interrupt IN endpoint only when the previous transfer is done.
 * Returns 1 if queued; 0 means "busy, retry next pass" (the caller keeps the
 * report pending). A flag stuck >20 ms (host stopped polling) is cleared. */
static uint32_t ep_busy_since[8];
static int ep_try_send(uint8_t ept, uint8_t *buf, uint16_t len, uint32_t now)
{
  uint8_t e = ept & 0x07u;
  if (usbd_connect_state_get(&otg_core_struct.dev) != USB_CONN_STATE_CONFIGURED) return 0;
  if (g_ep_busy[e]) {
    if ((uint32_t)(now - ep_busy_since[e]) < 20u) return 0;
    g_ep_busy[e] = 0;                     /* recover from a lost completion */
  }
  g_ep_busy[e] = 1; ep_busy_since[e] = now;
  usbd_ept_send(&otg_core_struct.dev, ept, buf, len);
  return 1;
}

static int send_boot_report(uint8_t *rep, uint32_t now)
{
  /* the USB core reads the buffer after this returns, and main rebuilds `report`
   * every pass - so transmit from a copy that only changes when a send is queued */
  static uint8_t tx[HID_BOOT_REPORT_LEN];
  uint8_t e = MG_EP_BOOT_KBD_IN & 0x07u;
  if (g_ep_busy[e] && (uint32_t)(now - ep_busy_since[e]) < 20u) return 0;
  for (unsigned i = 0; i < HID_BOOT_REPORT_LEN; i++) tx[i] = rep[i];
  return ep_try_send(MG_EP_BOOT_KBD_IN, tx, HID_BOOT_REPORT_LEN, now);
}

static int report_changed(const uint8_t *a, const uint8_t *b)
{
  for (unsigned i = 0; i < HID_BOOT_REPORT_LEN; i++) if (a[i] != b[i]) return 1;
  return 0;
}

/* Arm the resident bootloader's DFU mailbox the way stock does (erase its 2 KB
 * sector, program 0x55AA55AA, verify, retry) and reset -- a bare reset finds an
 * empty mailbox and the bootloader jumps straight back into this app. Called
 * from the EP0 handler too, so it works even if the main loop is wedged. */
void board_enter_bootloader(void)
{
  const uint32_t magic = 0x55AA55AAu;
  for (int i = 0; i < 10; i++) {
    if (flash_efc_ops.erase(0x08004800u) == 0 &&
        flash_efc_ops.program(0x08004800u, &magic, sizeof magic) == 0) break;
  }
  usb_delay_ms(20); NVIC_SystemReset();
}

int main(void)
{
  monsgeek_state_t *st;
  uint32_t led_t = 0, dirty_t = 0;
  int prev_configured = 0;

  nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);

  system_clock_config();          /* 216 MHz; PLLU -> 48 MHz USB clock          */
  g_boot_stage = 1;
  board_delay_init();             /* DWT cycle counter (also the ms time base)  */

  /* ---- link-mode select: read PC13 = VBUS/cable sense. PC13 HIGH selects the
   * wired/USB path, PC13 LOW selects wireless. The wired path below is unchanged;
   * a LOW PC13 additionally brings up SPI3. Wired-only boards (profile
   * wireless = false) skip the detect and stay wired.                             */
#if BOARD_WIRELESS
  {
    gpio_init_type io;
    crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
    gpio_default_para_init(&io);
    io.gpio_pins = GPIO_PINS_13;            /* GPIOC bit 0x2000 = PC13               */
    io.gpio_mode = GPIO_MODE_INPUT;
    /* Pull-DOWN so an unplugged board reads LOW => wireless; VBUS drives it HIGH
     * when a cable is present. The pull direction is a bring-up item. */
    io.gpio_pull = GPIO_PULL_DOWN;
    gpio_init(GPIOC, &io);
    g_wireless = (gpio_input_data_bit_read(GPIOC, GPIO_PINS_13) == RESET) ? 1 : 0;
  }
#else
  g_wireless = 0;                           /* wired-only board                     */
#endif

  /* USB device bring-up */
  usb_gpio_config();
  crm_periph_clock_enable(OTG_CLOCK, TRUE);
  usb_clock48m_select(USB_CLK_HEXT);
  nvic_irq_enable(OTG_IRQ, 0, 0);
  usbd_init(&otg_core_struct,
            USB_SPEED_CORE_ID,          /* from usb_conf.h: HS core for USB_OTG_HS */
            USB_ID,
            &monsgeek_class_handler,
            &monsgeek_desc_handler);

  g_boot_stage = 2;                         /* USB up */
  /* engine + RGB bring-up */
  keyscan_init();
  g_boot_stage = 3;
  hall_init(&hall);
  g_boot_stage = 4;
  rgb_init();
  g_boot_stage = 5;
  guide_init();

  /* wireless 2.4G/BLE bus bring-up (only when PC13 selected wireless). The
   * keyscan/hall/RGB/persist paths below are identical in both modes; only the
   * report-send site differs. The pairing / connection state machine is not
   * ported here - bring-up item. */
  if (g_wireless) wireless_init();

  /* live vendor settings live inside the USB class struct */
  st = &((monsgeek_class_t *)otg_core_struct.dev.class_handler->pdata)->state;

  /* boot config: clean defaults if flash is blank, else the saved image. This
   * runs before enumeration so the LEDs light even with no host attached. */
  persist_load(&flash_efc_ops, st);
  apply_magnetism(st, &hall);
  g_boot_stage = 6;
  g_diag_raw = raw; g_diag_n = KS_NUM_KEYS; g_diag_pressed = pressed;
  if (st->cfg[0x00] >= PROFILE_COUNT) { st->cfg[0x00] = 0; st->cfg_dirty = 1; }  /* was on a removed profile */

  for (unsigned i = 0; i < HID_BOOT_REPORT_LEN; i++) last_report[i] = 0;

  while (1)
  {
    uint32_t now = now_ms();
    int configured =
        (usbd_connect_state_get(&otg_core_struct.dev) == USB_CONN_STATE_CONFIGURED);

    /* host requested the 0x7F 55 AA 55 AA enter-bootloader handshake */
    if (st->enter_bootloader) board_enter_bootloader();

    /* On each (re)enumeration the shared class_init resets the state to defaults;
     * reload the persisted config so flash stays the source of truth. */
    if (configured && !prev_configured) {
      persist_load(&flash_efc_ops, st);
      apply_magnetism(st, &hall);
    }
    prev_configured = configured;

    /* ---- one scan / actuation / report cycle ---- */
    g_loop_count++;
    g_boot_stage = 10;                      /* in keyscan_frame */
    keyscan_frame(raw);
    g_boot_stage = 11;
    {
      static uint8_t primed;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
        if (!primed) for (unsigned h = 0; h < FILT_N; h++) filt_hist[h][i] = raw[i];
        filt_hist[filt_pos][i] = raw[i];
        uint32_t acc = 0;
        for (unsigned h = 0; h < FILT_N; h++) acc += filt_hist[h][i];
        filt[i] = (uint16_t)(acc / FILT_N);
      }
      primed = 1;
      filt_pos = (uint8_t)((filt_pos + 1u) % FILT_N);
    }
    /* calibrate only after ~64 scans: the first readings after power-up
     * (column 0 first) run high and used to seed a wrong rest level */
    if (g_loop_count > 64u) hall_process(&hall, filt, pressed);
    else for (unsigned i = 0; i < KS_NUM_KEYS; i++) pressed[i] = 0;
    /* sites with no key in the base map can never count as pressed (they read
     * near 0 and used to inflate the >10-key fuse) */
    for (unsigned i = 0; i < KS_NUM_KEYS; i++) if (!keymap_default[i]) pressed[i] = 0;
    if (g_map_state == 1) {
      /* LED mapping. Detection is deliberately independent of the actuation
       * engine (which made some presses not register): a press is a raw drop of
       * >300 counts below an idle baseline learnt at mapping start, held for 3
       * frames; release below 100. Same rule as the proven press-log tool. */
      static uint32_t macc[KS_NUM_KEYS];
      static uint16_t mbase[KS_NUM_KEYS];
      static uint8_t  mcnt[KS_NUM_KEYS], mdown[KS_NUM_KEYS], mframes;
      if (g_map_reset) {
        g_map_reset = 0; mframes = 0;
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) { macc[i] = 0; mcnt[i] = 0; mdown[i] = 0; }
      }
      if (mframes < 16) {
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) macc[i] += raw[i];
        if (++mframes == 16) for (unsigned i = 0; i < KS_NUM_KEYS; i++) mbase[i] = (uint16_t)(macc[i] / 16u);
      } else {
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
          if (mbase[i] < 300) continue;                       /* no sensor here */
          int d = (int)mbase[i] - (int)raw[i];
          if (mdown[i]) { if (d < 100) mdown[i] = 0; continue; }
          if (d > 300) { if (++mcnt[i] < 3) continue; } else { mcnt[i] = 0; continue; }
          mdown[i] = 1; mcnt[i] = 0;
          uint8_t used = 0;                     /* a key already mapped can't take another LED */
          for (unsigned j = 0; j < g_map_led; j++) if (g_ledmap[j] == i) { used = 1; break; }
          if (used || g_map_led >= MG_NUM_LEDS) continue;
          g_ledmap[g_map_led++] = (uint8_t)i;
          if (g_map_led >= MG_NUM_LEDS) g_map_state = 2;
          led_t = now - LED_TICK_MS;            /* light the next LED now */
        }
      }
      for (unsigned i = 0; i < KS_NUM_KEYS; i++) pressed[i] = 0;   /* nothing types while mapping */
    }
    /* Uses the default keymap: live keymatrix remap is persisted and round-tripped
     * but not yet applied here (see docs/firmware.md feature status). */
    {
      /* fuse: >10 simultaneous keys means a scan/sensor fault (seen in bring-up as
       * a flood of every key + Ctrl/Shift/Alt/Gui) -- send an empty report instead */
      unsigned n = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++) n += pressed[i] ? 1u : 0u;
      if (n > 10) for (unsigned i = 0; i < KS_NUM_KEYS; i++) pressed[i] = 0;
    }
    if (profile_map_for != st->cfg[0x00]) {     /* rebuild only on a profile change */
      uint8_t p = st->cfg[0x00] < 4u ? st->cfg[0x00] : 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        profile_map[i] = keymap_profile[p][i] ? keymap_profile[p][i] : keymap_default[i];
      profile_map_for = st->cfg[0x00];
    }
    keymap_resolve(pressed, profile_map, keymap_fn, KC_FN, active_map);
    {
      /* media keys -> consumer report (IF1 EP 0x82, report ID 3, 16-bit usage).
       * Busy-safe and retried like the keyboard report; buffer must stay valid. */
      static const uint16_t media_usage[6] = { 0x00B6, 0x00CD, 0x00B5, 0x00E2, 0x00EA, 0x00E9 };
      static uint8_t  cons_buf[3];
      static uint16_t cons_sent;
      uint16_t cu = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        if (pressed[i] && active_map[i] >= KC_MEDIA_PREV && active_map[i] <= KC_MEDIA_VOLU) {
          cu = media_usage[active_map[i] - KC_MEDIA_PREV]; break;
        }
      if (cu != cons_sent && !g_wireless) {
        cons_buf[0] = 0x03; cons_buf[1] = (uint8_t)cu; cons_buf[2] = (uint8_t)(cu >> 8);
        if (ep_try_send(MG_EP_EXT_IN, cons_buf, 3, now)) cons_sent = cu;
      }
    }
    g_fn_held = 0;
    for (unsigned i = 0; i < KS_NUM_KEYS; i++)
      if (pressed[i] && keymap_default[i] == KC_FN) { g_fn_held = 1; break; }
    {
      /* KC_PROFILE_NEXT (Fn+Tab): cycle the profile on the press edge only */
      static uint8_t prof_prev;
      uint8_t prof_now = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        if (pressed[i] && active_map[i] == KC_PROFILE_NEXT) { prof_now = 1; break; }
      if (prof_now && !prof_prev) {
        st->cfg[0x00] = (uint8_t)((st->cfg[0x00] + 1u) % PROFILE_COUNT);
        st->cfg_dirty = 1;                    /* persisted by the debounced save */
        guide_flash(0, now);                  /* number row: flash the new profile */
        led_t = now - LED_TICK_MS;            /* repaint on this pass */
      }
      prof_prev = prof_now;

      /* KC_PROFILE_1..3 (Fn+1/2/3): jump straight to a profile, press edge */
      static uint8_t psel_prev;
      uint8_t psel = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        if (pressed[i] && active_map[i] >= KC_PROFILE_1 && active_map[i] <= KC_PROFILE_3) {
          psel = (uint8_t)(active_map[i] - KC_PROFILE_1 + 1u); break;
        }
      if (psel && psel != psel_prev && (uint8_t)(psel - 1u) < PROFILE_COUNT) {
        st->cfg[0x00] = (uint8_t)(psel - 1u);
        st->cfg_dirty = 1;
        guide_flash(0, now);
        led_t = now - LED_TICK_MS;
      }
      psel_prev = psel;

      /* KC_LIGHT_NEXT (Fn+\): next lighting effect for this profile, press edge */
      static uint8_t light_prev;
      uint8_t light_now = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        if (pressed[i] && active_map[i] == KC_LIGHT_NEXT) { light_now = 1; break; }
      if (light_now && !light_prev) { light_next(st); guide_flash(1, now); led_t = now - LED_TICK_MS; }
      light_prev = light_now;

      /* KC_RT_TOGGLE (Fn+LCtrl): rapid trigger on/off for every key, press edge.
       * Uses the engine's per-key HALL_MODE_RAPID_TRIGGER and the configured
       * RT deltas (default 0.5 mm); persisted like any magnetism change. */
      static uint8_t rt_prev;
      uint8_t rt_now = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++)
        if (pressed[i] && active_map[i] == KC_RT_TOGGLE) { rt_now = 1; break; }
      if (rt_now && !rt_prev) {
        uint8_t on = st->mag_mode[0] != HALL_MODE_RAPID_TRIGGER;
        for (unsigned k = 0; k < MG_NUM_MAG_KEYS; k++)
          st->mag_mode[k] = on ? HALL_MODE_RAPID_TRIGGER : HALL_MODE_NORMAL;
        st->cfg_dirty = 1;                  /* apply_magnetism + debounced save */
        rt_flash(on, now);                  /* Space: red x3 = on, blue x3 = off */
        led_t = now - LED_TICK_MS;
      }
      rt_prev = rt_now;

      /* KC_SPEED_DOWN / KC_SPEED_UP (Fn+; / Fn+'): animation speed, press edge */
      static uint8_t spd_prev;
      uint8_t spd_now = 0;
      for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
        if (!pressed[i]) continue;
        if (active_map[i] == KC_SPEED_DOWN) spd_now = 1;
        else if (active_map[i] == KC_SPEED_UP) spd_now = 2;
      }
      if (spd_now && spd_now != spd_prev) {
        /* single-colour modes: step the colour; animated modes: step the speed */
        if (fx_is_single_colour(light_cycle[current_fx(st)])) {
          colour_step(st, spd_now == 1 ? -1 : +1);
        } else {
          speed_step(st, spd_now == 1 ? -1 : +1);
          guide_flash(2, now);                /* A..G row: flash the new speed */
        }
        led_t = now - LED_TICK_MS;
      }
      spd_prev = spd_now;
    }
    hid_build_boot_report(pressed, active_map, report);
#ifndef DIAG_NO_HID
    if (report_changed(report, last_report)) {
      int sent = 1;
      if (g_wireless) wl_send_keyboard(report);   /* wireless SPI3 path (0x81) */
      else            sent = send_boot_report(report, now);   /* wired USB path */
      /* only a delivered report becomes "last": a busy endpoint retries next pass */
      if (sent) for (unsigned i = 0; i < HID_BOOT_REPORT_LEN; i++) last_report[i] = report[i];
    }
#else
    (void)report_changed; (void)send_boot_report;   /* diag: never send key reports */
    {
      /* bring-up press log: baseline = mean of loops 32..63, a press = drop of
       * >300 counts (full travel is ~700), release at <100; log first presses. */
      static uint32_t base_acc[KS_NUM_KEYS];
      static uint16_t base[KS_NUM_KEYS];
      static uint8_t  down[KS_NUM_KEYS], seen[KS_NUM_KEYS], cnt[KS_NUM_KEYS];
      uint32_t lc = g_loop_count;
      if (lc >= 32 && lc < 64) {
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) base_acc[i] += raw[i];
      } else if (lc == 64) {
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) { base[i] = (uint16_t)(base_acc[i] / 32u); down[i] = seen[i] = 0; }
      } else if (lc > 64) {
        if (g_presslog_n == 0) for (unsigned i = 0; i < KS_NUM_KEYS; i++) seen[i] = 0;  /* host cleared */
        for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
          if (base[i] < 300) continue;                         /* no sensor at this site */
          int d = (int)base[i] - (int)raw[i];
          /* 3-frame debounce: site 49 shows lone 1-sample dips to ~120 at idle */
          if (!down[i] && d > 300) { if (cnt[i] < 255) cnt[i]++; } else if (!down[i]) cnt[i] = 0;
          if (!down[i] && cnt[i] >= 3) {
            down[i] = 1; cnt[i] = 0;
            if (!seen[i] && g_presslog_n < sizeof g_presslog) { seen[i] = 1; g_presslog[g_presslog_n++] = (uint8_t)i; }
          } else if (down[i] && d < 100) down[i] = 0;
        }
      }
    }
#endif

    /* ---- RGB refresh ---- */
    if ((uint32_t)(now - led_t) >= LED_TICK_MS) {
      led_t = now;
      g_boot_stage = 12;                    /* in render_leds */
#ifndef DIAG_NO_HID
      render_leds(st, now);
#endif
      g_boot_stage = 13;
    }

    /* ---- apply config changes immediately; persist to flash debounced ---- */
    if (st->cfg_dirty) {
      apply_magnetism(st, &hall);           /* take effect now, not only after the save */
      if (dirty_t == 0) dirty_t = now ? now : 1u;
      if ((uint32_t)(now - dirty_t) >= SAVE_DEBOUNCE_MS) {
        persist_save(&flash_efc_ops, st);   /* clears cfg_dirty */
        dirty_t = 0;
      }
    } else {
      dirty_t = 0;
    }
  }
}

/* OTG-FS global interrupt -> USB device IRQ handler */
void OTG_IRQ_HANDLER(void)
{
  usbd_irq_handler(&otg_core_struct);
}
