/*
 * vendor_proto.c - Fun60 Ultra vendor feature-report dispatcher.
 *
 * The vendor dispatcher provides the SET/GET handlers needed to fully configure
 * the board: LED parameters in the per-mode table layout, per-key
 * Static colours, keymatrix remap, and per-key magnetism. Pure / hardware-
 * independent so it is fully host-testable (test/test_functional.c); persistence
 * and RGB output are wired at the platform layer (persist.c, rgb.c, main.c).
 * SET handlers set st->cfg_dirty.
 *
 * The dispatcher operates on the 64-byte USB report directly.
 */
#include "vendor_proto.h"
#include <string.h>

/* ---- clean factory defaults ---------------------------------------------- */

void monsgeek_state_init(monsgeek_state_t *st)
{
  unsigned i;
  memset(st, 0, sizeof(*st));

  /* --- core config header --- */
  st->cfg[0x00] = 0x00;   /* active profile 0                                   */
  st->cfg[0x02] = 0x00;   /* polling code 0 => 8000 Hz                          */
  st->cfg[0x04] = 0x10;   /* options: LED on (bit4)                             */
  st->cfg[0x08] = LED_MODE_WAVE; /* default effect = Wave (lively, "like new")  */
  st->cfg[0x09] = 0x05;   /* debounce 5 ms                                      */
  /* sleep timers (GET_SLEEPTIME 0x91) */
  st->cfg[0x14] = 0x05; st->cfg[0x15] = 0x00;
  st->cfg[0x16] = 0x3c; st->cfg[0x17] = 0x00;   /* 60 s                         */
  st->cfg[0x18] = 0x2c; st->cfg[0x19] = 0x01;   /* 300 s                        */
  st->cfg[0x1a] = 0x84; st->cfg[0x1b] = 0x03;   /* 900 s                        */
  st->cfg[0x1c] = 0x10; st->cfg[0x1d] = 0x0e;

  /* --- LED per-mode table: speed/brightness/options/RGB for every mode --- */
  for (i = 0; i <= MG_LED_MODE_MAX; i++) {
    st->rgb[MG_LED_TBL_SPEED  + i] = 0x02;       /* mid speed                   */
    st->rgb[MG_LED_TBL_BRIGHT + i] = 0x04;       /* full brightness             */
    st->rgb[MG_LED_TBL_OPTS   + i] = LED_OPT_FLAG_NORMAL; /* dir 0, single colour*/
    st->rgb[MG_LED_TBL_RGB + i*3 + 0] = 0xFF;    /* default colour = white      */
    st->rgb[MG_LED_TBL_RGB + i*3 + 1] = 0xFF;
    st->rgb[MG_LED_TBL_RGB + i*3 + 2] = 0xFF;
  }
  /* Wave defaults to DAZZLE (rainbow) so the out-of-box effect is colourful. */
  st->rgb[MG_LED_TBL_OPTS + LED_MODE_WAVE] = LED_OPT_FLAG_DAZZLE;

  /* --- per-key Static colours: default white (only used in UserPicture mode) */
  for (i = 0; i < MG_NUM_LEDS; i++) {
    st->userpic[i][0] = st->userpic[i][1] = st->userpic[i][2] = 0xFF;
  }

  /* --- keymatrix: 0 = "use default ANSI map" (engine falls back to it) --- */
  /* (left zeroed by memset; the engine uses keymap_default until remapped)   */

  /* --- magnetism defaults ---
   * raw == centi-mm (raw 200 = 2.00 mm actuation), Normal mode. */
  for (i = 0; i < MG_NUM_MAG_KEYS; i++) {
    st->mag_press[i]    = MG_DEF_MAG_PRESS;     /* actuation        (sub 0x00)   */
    st->mag_lift[i]     = MG_DEF_MAG_LIFT;      /* release ref      (sub 0x01)   */
    st->mag_rt_press[i] = MG_DEF_MAG_RT_PRESS;  /* RT press delta   (sub 0x02)   */
    st->mag_rt_lift[i]  = MG_DEF_MAG_RT_LIFT;   /* RT release delta (sub 0x03)   */
    st->mag_mode[i]     = 0;     /* Normal                                     */
  }
  st->mag_global_mode = 0;       /* Normal                                     */

  st->cal_state = 0;
  st->mag_report_on = 0;
  st->cfg_dirty = 0;
  st->enter_bootloader = 0;
}

/* ---- checksum (Bit7 default; Bit8 for LED 0x07/0x08) --------------------- */

int monsgeek_checksum_ok(const uint8_t *report)
{
  uint8_t opcode = report[0];
  uint8_t sum8 = (uint8_t)(report[0] + report[1] + report[2] + report[3] +
                           report[4] + report[5] + report[6] + report[7]);
  if (opcode == FEA_SET_LEDPARAM || opcode == FEA_SET_SLEDPARAM)
    return (uint8_t)(sum8 + report[8]) == 0xFF;
  return sum8 == 0xFF;
}

void monsgeek_emit_bit7(uint8_t *report)
{
  uint8_t s = 0; int i;
  for (i = 0; i < 7; i++) s = (uint8_t)(s + report[i]);
  report[7] = (uint8_t)(0xFF - s);
}

void monsgeek_emit_bit8(uint8_t *report)
{
  uint8_t s = 0; int i;
  for (i = 0; i < 8; i++) s = (uint8_t)(s + report[i]);
  report[8] = (uint8_t)(0xFF - s);
}

static void clear_payload(uint8_t *report)
{
  unsigned i;
  for (i = 1; i < MONSGEEK_REPORT_SIZE; i++) report[i] = 0;
}

/* ---- LED parameter table helpers ---------------------------------------- */

int monsgeek_get_led_params(const monsgeek_state_t *st, led_params_t *out)
{
  uint8_t mode = st->cfg[0x08];
  uint8_t m = mode & MG_LED_MODE_MAX;
  uint8_t opts = st->rgb[MG_LED_TBL_OPTS + m];
  out->mode       = mode;
  out->speed      = st->rgb[MG_LED_TBL_SPEED  + m] & 0x07u;
  out->brightness = st->rgb[MG_LED_TBL_BRIGHT + m] & 0x07u;
  out->direction  = (uint8_t)(opts >> 4);
  out->flag       = (uint8_t)(opts & 0x0F);
  out->r = st->rgb[MG_LED_TBL_RGB + m*3 + 0];
  out->g = st->rgb[MG_LED_TBL_RGB + m*3 + 1];
  out->b = st->rgb[MG_LED_TBL_RGB + m*3 + 2];
  return (st->cfg[0x04] >> 4) & 1u;     /* LED-on flag */
}

/* ---- magnetism helpers --------------------------------------------------- */

static uint16_t *mag_u16_array(monsgeek_state_t *st, uint8_t sub)
{
  switch (sub) {
    case MAG_SUB_PRESS_TRAVEL: return st->mag_press;
    case MAG_SUB_LIFT_TRAVEL:  return st->mag_lift;
    case MAG_SUB_RT_PRESS:     return st->mag_rt_press;
    case MAG_SUB_RT_LIFT:      return st->mag_rt_lift;
    default:                   return 0;
  }
}

/* SET_MULTI_MAGNETISM 0x65. report[1]=sub, [2]=bulk(0=single), [3]=key|page,
 * payload at report[8..]. */
static int mag_set(monsgeek_state_t *st, uint8_t *report)
{
  uint8_t sub  = report[1];
  uint8_t bulk = report[2];
  uint8_t kp   = report[3];
  uint16_t *arr = mag_u16_array(st, sub);

  if (arr) {
    if (!bulk) {                        /* single key */
      if (kp < MG_NUM_MAG_KEYS)
        arr[kp] = (uint16_t)(report[8] | (report[9] << 8));
    } else {                            /* bulk: up to 28 u16 per chunk (56 B) */
      unsigned base = (unsigned)kp * MG_MAG_KEYS_PER_PAGE;
      unsigned i;
      for (i = 0; i < 28u && (base + i) < MG_NUM_MAG_KEYS; i++)
        arr[base + i] = (uint16_t)(report[8 + i*2] | (report[9 + i*2] << 8));
    }
    st->cfg_dirty = 1;
    return 1;
  }
  if (sub == MAG_SUB_KEY_MODE) {        /* u8 per key */
    if (!bulk) {
      if (kp < MG_NUM_MAG_KEYS) st->mag_mode[kp] = report[8];
    } else {
      unsigned base = (unsigned)kp * MG_MAG_KEYS_PER_PAGE;
      unsigned i;
      for (i = 0; i < 56u && (base + i) < MG_NUM_MAG_KEYS; i++)
        st->mag_mode[base + i] = report[8 + i];
    }
    st->cfg_dirty = 1;
    return 1;
  }
  /* unknown sub: accept silently so the app's probe sequence still succeeds */
  return 1;
}

/* GET_MULTI_MAGNETISM 0xE5 -> RAW response (no opcode echo), 32 u16 / page or
 * 64 u8 / page. request: report[1]=sub, [4]=page. */
static int mag_get(monsgeek_state_t *st, uint8_t *report)
{
  uint8_t sub  = report[1];
  uint8_t page = report[4];
  uint16_t *arr = mag_u16_array(st, sub);
  unsigned base = (unsigned)page * MG_MAG_KEYS_PER_PAGE;
  unsigned i;

  if (arr) {
    for (i = 0; i < MG_MAG_KEYS_PER_PAGE; i++) {
      uint16_t v = (base + i < MG_NUM_MAG_KEYS) ? arr[base + i] : 0;
      report[i*2 + 0] = (uint8_t)(v & 0xFF);
      report[i*2 + 1] = (uint8_t)(v >> 8);
    }
    return 1;
  }
  if (sub == MAG_SUB_KEY_MODE) {
    for (i = 0; i < MG_MAG_KEYS_PER_PAGE * 2u; i++)
      report[i] = (base + i < MG_NUM_MAG_KEYS) ? st->mag_mode[base + i] : 0;
    return 1;
  }
  clear_payload(report);
  return 1;
}

/* ---- dispatcher ---------------------------------------------------------- */

volatile uint8_t  g_boot_stage;
volatile uint32_t g_loop_count;
volatile const uint16_t *g_diag_raw;
volatile uint16_t g_diag_n;
volatile uint32_t g_adc_timeouts;
volatile uint8_t  g_presslog[128];
volatile uint8_t  g_presslog_n;
volatile const uint8_t *g_diag_pressed;
__attribute__((weak)) int board_diag_cmd(uint8_t *report) { (void)report; return 0; }

int monsgeek_vendor_dispatch(monsgeek_state_t *st, uint8_t *report)
{
  uint8_t opcode = report[0];
  uint8_t *cfg = st->cfg;
  uint8_t *rgb = st->rgb;

  if (!monsgeek_checksum_ok(report))
    return 0;

  switch (opcode) {

  /* ---------------- device-info GETs ---------------- */
  case FEA_GET_INFOR:              /* 0x8F */
    report[1] = MONSGEEK_INFO_DEVID_LO; report[2] = MONSGEEK_INFO_DEVID_HI;
    report[3] = g_boot_stage;                       /* bring-up diagnostics */
    report[4] = (uint8_t)g_loop_count; report[5] = (uint8_t)(g_loop_count >> 8);
    report[6] = (uint8_t)(g_loop_count >> 16);
    report[7] = MONSGEEK_INFO_VER_HI;   report[8] = MONSGEEK_INFO_VER_LO;
    return 1;

  case FEA_DIAG_RAW: {             /* 0xF0 <start>: bring-up diagnostics       */
    /* reply: [1]=start [2]=total sites [3]=n returned [4..5]=ADC timeouts (u16 LE)
     *        [8..] = n raw samples, u16 LE (up to 28 per report) */
    uint8_t start = report[1], n = 0;
    uint32_t to = g_adc_timeouts;
    while (n < 28 && g_diag_raw && (uint16_t)(start + n) < g_diag_n) {
      uint16_t v = g_diag_raw[start + n];
      report[8 + 2 * n] = (uint8_t)v; report[9 + 2 * n] = (uint8_t)(v >> 8);
      n++;
    }
    report[2] = (uint8_t)g_diag_n; report[3] = n;
    report[4] = (uint8_t)to; report[5] = (uint8_t)(to >> 8);
    return 1;
  }

  case FEA_DIAG_GPIO_POKE:
  case FEA_DIAG_HWSTATUS:
  case FEA_LIVE_LEDS:
  case FEA_LEDMAP_CTRL:
  case FEA_LEDMAP_READ:
    return board_diag_cmd(report);

  case FEA_DIAG_PRESSED: {         /* 0xF2: [1]=count pressed [8..] bitmap of sites */
    uint8_t cnt = 0;
    for (unsigned i = 8; i < 64; i++) report[i] = 0;
    for (unsigned i = 0; g_diag_pressed && i < g_diag_n && i < 8u * 56u; i++)
      if (g_diag_pressed[i]) { cnt++; report[8 + i / 8] |= (uint8_t)(1u << (i % 8)); }
    report[1] = cnt;
    return 1;
  }

  case FEA_DIAG_PRESSLOG: {        /* 0xF1 <start> [0xC1=clear]: press log     */
    /* reply: [1]=start [2]=total logged [3]=n returned [8..] = site indices */
    uint8_t start = report[1], n = 0;
    if (report[2] == 0xC1) { g_presslog_n = 0; }
    while (n < 56 && (uint8_t)(start + n) < g_presslog_n) { report[8 + n] = g_presslog[start + n]; n++; }
    report[2] = g_presslog_n; report[3] = n;
    return 1;
  }

  case FEA_GET_REV:                /* 0x80 */
  case 0x81:
#ifndef MONSGEEK_STRICT_GETREV
    report[1] = MONSGEEK_INFO_VER_HI; report[2] = MONSGEEK_INFO_VER_LO;
#endif
    return 1;

  case FEA_GET_KBVALUE:            /* 0x82 */
    report[1] = 0x64; report[2] = 0x00; report[3] = cfg[0xe2];
    return 1;

  case FEA_GET_REPORT:             /* 0x83 */
    report[2] = cfg[0x02];
    return 1;

  case FEA_GET_PROFILE:            /* 0x84 */
    report[1] = cfg[0x00];
    return 1;

  case FEA_GET_LEDONOFF:           /* 0x85 */
    report[1] = (uint8_t)((cfg[0x04] >> 4) & 1);
    report[2] = (uint8_t)((cfg[0x04] >> 5) & 1);
    return 1;

  case FEA_GET_DEBOUNCE:           /* 0x86 */
    report[1] = cfg[0x09];
    return 1;

  case FEA_GET_LEDPARAM:           /* 0x87 / 0x88: [mode][speed][bright][opts][R][G][B] */
  case FEA_GET_SLEDPARAM: {
    uint8_t mode = cfg[0x08];
    uint8_t m = mode & MG_LED_MODE_MAX;
    report[1] = mode;
    report[2] = rgb[MG_LED_TBL_SPEED  + m];
    report[3] = rgb[MG_LED_TBL_BRIGHT + m];
    report[4] = rgb[MG_LED_TBL_OPTS   + m];
    report[5] = rgb[MG_LED_TBL_RGB + m*3 + 0];
    report[6] = rgb[MG_LED_TBL_RGB + m*3 + 1];
    report[7] = rgb[MG_LED_TBL_RGB + m*3 + 2];
    return 1;
  }

  case FEA_GET_KBOPTION:           /* 0x89 */
    report[1] = (uint8_t)((cfg[0x04] >> 1) & 3);
    report[2] = (uint8_t)(cfg[0x05] & 1);
    report[3] = cfg[0xe3];
    report[4] = cfg[0xe5];
    return 1;

  case FEA_GET_KEYMATRIX: {        /* 0x8A: return remap chunk [layer][chunk]+payload */
    uint8_t chunk = report[2];
    unsigned off = (unsigned)chunk * 56u;
    unsigned i;
    for (i = 0; i < 56u; i++)
      report[8 + i] = (off + i < MG_KEYMATRIX_BYTES) ? st->keymatrix[off + i] : 0;
    return 1;
  }

  case FEA_GET_USERPIC: {          /* 0x8C: per-key colours, page of report[3] */
    uint8_t page = report[3];
    unsigned base = (unsigned)page * 18u;   /* 18 LEDs/page (54 B of payload)   */
    unsigned i;
    for (i = 0; i < 18u; i++) {
      unsigned led = base + i;
      report[8 + i*3 + 0] = (led < MG_NUM_LEDS) ? st->userpic[led][0] : 0;
      report[8 + i*3 + 1] = (led < MG_NUM_LEDS) ? st->userpic[led][1] : 0;
      report[8 + i*3 + 2] = (led < MG_NUM_LEDS) ? st->userpic[led][2] : 0;
    }
    return 1;
  }

  case FEA_GET_FN:                 /* 0x90: Fn-layer remap (stub, echoes 0) */
    return 1;

  case FEA_GET_SLEEPTIME:          /* 0x91 */
    report[1]  = cfg[0x14]; report[2]  = cfg[0x15];
    report[7]  = cfg[0x16]; report[8]  = cfg[0x17];
    report[9]  = cfg[0x1a]; report[10] = cfg[0x1b];
    report[11] = cfg[0x18]; report[12] = cfg[0x19];
    report[13] = cfg[0x1c]; report[14] = cfg[0x1d];
    return 1;

  case FEA_GET_MAG_CAL:            /* 0x9C */
    report[1] = st->cal_state;
    return 1;

  case FEA_GET_KEY_MAG_MODE:       /* 0x9D */
    report[1] = st->mag_global_mode;
    return 1;

  case FEA_GET_MULTI_MAG:          /* 0xE5 -> RAW page response */
    return mag_get(st, report);

  case FEA_GET_FEATURE_LIST:       /* 0xE6 */
    clear_payload(report);
    report[1] = 0x01; report[2] = 0x01; report[3] = 0x01; report[4] = 0x01;
    return 1;

  /* ---------------- SETs (mark dirty where persistent) ---------------- */
  case FEA_SET_KBVALUE:            /* 0x02 */
    cfg[0xe2] = report[1]; st->cfg_dirty = 1; return 1;

  case FEA_SET_REPORT:             /* 0x03 */
    cfg[0x02] = report[2]; st->cfg_dirty = 1; return 1;

  case FEA_SET_PROFILE:            /* 0x04 */
    cfg[0x00] = report[1]; st->cfg_dirty = 1; return 1;

  case FEA_SET_LEDONOFF:           /* 0x05 */
    cfg[0x04] = (uint8_t)((cfg[0x04] & 0xEF) | ((report[1] & 1) << 4));
    cfg[0x04] = (uint8_t)((cfg[0x04] & 0xDF) | ((report[2] & 1) << 5));
    st->cfg_dirty = 1; return 1;

  case FEA_SET_DEBOUNCE:           /* 0x06 */
    cfg[0x09] = report[1]; st->cfg_dirty = 1; return 1;

  case FEA_SET_LEDPARAM:           /* 0x07 (Bit8): [mode][speed][bright][opts][R][G][B] */
  case FEA_SET_SLEDPARAM: {        /* 0x08 (Bit8) */
    uint8_t mode = report[1];
    uint8_t m = mode & MG_LED_MODE_MAX;
    cfg[0x08] = mode;
    rgb[MG_LED_TBL_SPEED  + m] = report[2];
    rgb[MG_LED_TBL_BRIGHT + m] = report[3];
    rgb[MG_LED_TBL_OPTS   + m] = report[4];
    rgb[MG_LED_TBL_RGB + m*3 + 0] = report[5];
    rgb[MG_LED_TBL_RGB + m*3 + 1] = report[6];
    rgb[MG_LED_TBL_RGB + m*3 + 2] = report[7];
    st->cfg_dirty = 1; return 1;
  }

  case FEA_SET_KBOPTION:           /* 0x09 */
    cfg[0x04] = (uint8_t)((cfg[0x04] & 0xF9) | ((report[1] & 3) << 1));
    cfg[0x05] = (uint8_t)((cfg[0x05] & 0xFE) | (report[2] & 1));
    cfg[0xe3] = report[3];
    cfg[0xe5] = report[4];
    st->cfg_dirty = 1; return 1;

  case FEA_SET_KEYMATRIX: {        /* 0x0A: chunked remap [layer][chunk]+payload */
    uint8_t chunk = report[2];
    unsigned off = (unsigned)chunk * 56u;
    unsigned i;
    for (i = 0; i < 56u && (off + i) < MG_KEYMATRIX_BYTES; i++)
      st->keymatrix[off + i] = report[8 + i];
    st->cfg_dirty = 1; return 1;
  }

  case FEA_SET_USERPIC: {          /* 0x0C: per-key colours. single or page */
    if (report[2] == 0xFF) {       /* bulk page: report[3]=page, triples @8.. */
      uint8_t page = report[3];
      unsigned base = (unsigned)page * 18u;
      unsigned i;
      for (i = 0; i < 18u && (base + i) < MG_NUM_LEDS; i++) {
        st->userpic[base + i][0] = report[8 + i*3 + 0];
        st->userpic[base + i][1] = report[8 + i*3 + 1];
        st->userpic[base + i][2] = report[8 + i*3 + 2];
      }
    } else {                       /* single: report[1]=led, [2..4]=R,G,B */
      uint8_t led = report[1];
      if (led < MG_NUM_LEDS) {
        st->userpic[led][0] = report[2];
        st->userpic[led][1] = report[3];
        st->userpic[led][2] = report[4];
      }
    }
    st->cfg_dirty = 1; return 1;
  }

  case FEA_SET_FN:                 /* 0x10: Fn remap (accepted, stored stub) */
    st->cfg_dirty = 1; return 1;

  case FEA_SET_SLEEPTIME:          /* 0x11 */
    cfg[0x14] = report[1]; cfg[0x15] = report[2];
    cfg[0x16] = report[8]; cfg[0x17] = report[9];
    cfg[0x1a] = report[10]; cfg[0x1b] = report[11];
    cfg[0x18] = report[12]; cfg[0x19] = report[13];
    cfg[0x1c] = report[14]; cfg[0x1d] = report[15];
    st->cfg_dirty = 1; return 1;

  case FEA_SET_MAG_REPORT:         /* 0x1B */
    st->mag_report_on = (uint8_t)(report[1] & 1); return 1;

  case FEA_SET_MAG_CAL:            /* 0x1C */
  case FEA_SET_MAG_MAXCAL:         /* 0x1E */
    st->cal_state = (uint8_t)(report[1] & 1); return 1;

  case FEA_SET_KEY_MAG_MODE: {     /* 0x1D: global key mode (apply to all keys) */
    unsigned i;
    st->mag_global_mode = report[1];
    for (i = 0; i < MG_NUM_MAG_KEYS; i++) st->mag_mode[i] = report[1];
    st->cfg_dirty = 1; return 1;
  }

  case FEA_SET_MULTI_MAG:          /* 0x65 */
    return mag_set(st, report);

  case FEA_SET_MACRO:              /* 0x0B: accept (macro storage stub) */
    st->cfg_dirty = 1; return 1;

  case FEA_SET_RESET:              /* 0x01: factory reset -> clean defaults */
    monsgeek_state_init(st);
    st->cfg_dirty = 1;
    return 1;

  /* ---------------- enter-bootloader handshake ---------------- */
  case FEA_ENTER_BOOTLOADER:       /* 0x7F 55 AA 55 AA */
    if (report[1] == 0x55 && report[2] == 0xAA &&
        report[3] == 0x55 && report[4] == 0xAA) {
      st->enter_bootloader = 1;
      return 1;
    }
    return 0;

  default:
    return 0;
  }
}
