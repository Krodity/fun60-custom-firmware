/*
 * test_engine.c - host-side self-test for the pure-logic engine (hall + hid_report).
 * Builds natively (no AT32/USB). Run via `make test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hall.h"
#include "hid_report.h"
#include "keyscan.h"
#include "board_keymap.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
  else         { printf("  ok  : %s\n", msg); } } while (0)

/* feed one frame where every key reads `def` except key `idx` which reads `val`. */
static void frame(hall_engine_t *e, uint8_t *pressed, unsigned idx, uint16_t val, uint16_t def)
{
  uint16_t raw[KS_NUM_KEYS];
  for (unsigned i = 0; i < KS_NUM_KEYS; i++) raw[i] = def;
  raw[idx] = val;
  hall_process(e, raw, pressed);
}

/* site index of the first keymap entry holding `code`, or -1 if absent */
static int site_find(uint8_t code)
{
  for (unsigned i = 0; i < KS_NUM_KEYS; i++) if (keymap_default[i] == code) return (int)i;
  return -1;
}

/* site index of the first keymap entry holding `code` (profile-independent tests) */
static unsigned site_of(uint8_t code)
{
  for (unsigned i = 0; i < KS_NUM_KEYS; i++) if (keymap_default[i] == code) return i;
  fprintf(stderr, "keycode 0x%02X not in keymap\n", code); exit(2);
}

int main(void)
{
  hall_engine_t e;
  uint8_t pressed[KS_NUM_KEYS];
  const unsigned K = 9;            /* key index 9 = 'A' (0x04) in the default map */
  const uint16_t REST = 3000, DEEP = 1000;

  printf("[1] Normal-mode actuation + hysteresis\n");
  hall_init(&e);
  frame(&e, pressed, K, REST, REST);                 /* prime baseline */
  CHECK(pressed[K] == 0, "released after baseline seed");
  frame(&e, pressed, K, DEEP, REST);                 /* full press */
  CHECK(pressed[K] == 1, "press at 2000 counts actuates");
  {
    /* derive the band from the profile's defaults so any tuning is tested */
    int act = (int)HALL_CMM_TO_COUNTS(HALL_DEF_PRESS_CMM);
    int rel = (int)HALL_CMM_TO_COUNTS(HALL_DEF_RELEASE_CMM);
    if (rel >= act) rel = act - (int)HALL_CMM_TO_COUNTS(20);
    frame(&e, pressed, K, (uint16_t)(REST - (act + rel) / 2), REST);
    CHECK(pressed[K] == 1, "stays pressed inside the hysteresis band");
    frame(&e, pressed, K, (uint16_t)(REST - rel / 2), REST);
    CHECK(pressed[K] == 0, "releases once travel drops below release point");
  }

  printf("[2] Rapid Trigger: release/re-press on direction reversal\n");
  hall_init(&e);
  hall_keycfg_t rt = { .mode = HALL_MODE_RAPID_TRIGGER, .press_cmm = 200,
                       .release_cmm = 150, .rt_press_cmm = 50, .rt_release_cmm = 50 };
  hall_set_global(&e, &rt);
  frame(&e, pressed, K, REST, REST);                 /* prime */
  frame(&e, pressed, K, DEEP, REST);                 /* press deep */
  CHECK(pressed[K] == 1, "RT press on downstroke");
  frame(&e, pressed, K, 1500, REST);                 /* up 500 counts (>0.5mm) */
  CHECK(pressed[K] == 0, "RT release on upstroke past rt_release");
  frame(&e, pressed, K, 1200, REST);                 /* down 300 counts (>0.5mm) */
  CHECK(pressed[K] == 1, "RT re-press on downstroke past rt_press");

  printf("[2b] Rapid Trigger honours the configured actuation point on first press\n");
  hall_init(&e);
  hall_set_global(&e, &rt);                          /* act 800, rt_press 200 counts */
  frame(&e, pressed, K, REST, REST);                 /* prime (travel 0) */
  frame(&e, pressed, K, 2600, REST);                 /* travel 400 < actuation 800 */
  CHECK(pressed[K] == 0, "RT does NOT actuate before the actuation point (travel 400)");
  frame(&e, pressed, K, 2100, REST);                 /* travel 900 >= actuation 800 */
  CHECK(pressed[K] == 1, "RT actuates once travel reaches the actuation point (900)");
  frame(&e, pressed, K, REST, REST);                 /* full release (travel 0 <= release) */
  CHECK(pressed[K] == 0, "RT releases and re-arms on full release");
  frame(&e, pressed, K, 2600, REST);                 /* travel 400 < actuation 800 again */
  CHECK(pressed[K] == 0, "re-armed: the next first press again needs the actuation point");

  printf("[2c] Baseline: a single upward spike must not leave a key 'pressed'\n");
  {
    hall_engine_t e2;
    uint8_t pr2[KS_NUM_KEYS];
    hall_init(&e2);
    frame(&e2, pr2, K, REST, REST);                  /* prime */
    for (int i = 0; i < 20; i++) frame(&e2, pr2, K, REST, REST);
    uint16_t before = e2.key[K].baseline;
    frame(&e2, pr2, K, (uint16_t)(REST + 400), REST);/* one-sample glitch upward */
    CHECK(e2.key[K].baseline <= before + 1, "spike raises the baseline by at most 1 count");
    int ever = 0;
    for (int i = 0; i < 50; i++) { frame(&e2, pr2, K, REST, REST); ever |= pr2[K]; }
    CHECK(!ever, "key never reads pressed after the spike");
    CHECK(e2.key[K].baseline <= before + 1, "baseline settles back to rest");
  }

  printf("[2d] Bad-baseline guard: a key stuck 'pressed' at rest is re-seeded\n");
  {
    int act = (int)HALL_CMM_TO_COUNTS(HALL_DEF_PRESS_CMM);
    int lim = (int)HALL_CMM_TO_COUNTS(100);          /* guard only below 1.00 mm */
    if (act + 30 >= lim) {
      printf("  skip: actuation (%d counts) is not below the 1 mm guard\n", act);
    } else {
      hall_engine_t e3; uint8_t p3[KS_NUM_KEYS];
      uint16_t high = (uint16_t)(REST + act + 30);   /* wrong (too high) first reading */
      hall_init(&e3);
      frame(&e3, p3, K, high, REST);                 /* seeds the bad baseline */
      frame(&e3, p3, K, REST, REST);
      CHECK(p3[K] == 1, "bad seed: key reads pressed at rest (the bug)");
      int released_at = -1;
      for (int i = 0; i < 7000 && released_at < 0; i++) {
        frame(&e3, p3, K, REST, REST);
        if (!p3[K]) released_at = i;
      }
      CHECK(released_at > 0, "guard releases the stuck key");
      frame(&e3, p3, K, REST, REST);
      CHECK(p3[K] == 0, "stays released afterwards");
      /* a real bottomed-out hold must survive the same time */
      hall_init(&e3);
      frame(&e3, p3, K, REST, REST);
      int dropped = 0;
      for (int i = 0; i < 7000; i++) { frame(&e3, p3, K, DEEP, REST); if (i > 2 && !p3[K]) dropped = 1; }
      CHECK(!dropped, "a real bottomed-out hold is never released by the guard");
    }
  }

  printf("[3] Boot report: modifiers fold, keycodes fill, de-dupe\n");
  {
    uint8_t pr[KS_NUM_KEYS]; memset(pr, 0, sizeof pr);
    uint8_t rep[HID_BOOT_REPORT_LEN];
    /* look sites up by keycode so the test holds for any profile's layout */
    pr[site_of(0x29)] = 1;   /* Esc    */
    pr[site_of(0xE1)] = 1;   /* LShift -> modifier bit1 */
    pr[site_of(0x04)] = 1;   /* A      */
    uint8_t n = hid_build_boot_report(pr, keymap_default, rep);
    CHECK(n == 2, "two non-modifier keys");
    CHECK(rep[0] == 0x02, "LShift folded into modifier byte (bit1)");
    CHECK(rep[1] == 0x00, "reserved byte zero");
    int has_esc = 0, has_a = 0;
    for (int i = 2; i < 8; i++) { if (rep[i] == 0x29) has_esc = 1; if (rep[i] == 0x04) has_a = 1; }
    CHECK(has_esc && has_a, "Esc + A present in keycode slots");
  }

  printf("[5] Fn layer: KC_FN switches to keymap_fn, 0 falls through\n");
  if (site_find(KC_FN) < 0) {
    printf("  skip: profile has no Fn key\n");
  } else {
    uint8_t pr[KS_NUM_KEYS], act[KS_NUM_KEYS], rep[HID_BOOT_REPORT_LEN];
    unsigned fn = (unsigned)site_find(KC_FN), k = site_of(0x0C) /* 'i' */, a = site_of(0x04) /* 'a' */;
    memset(pr, 0, sizeof pr);
    pr[k] = 1;
    keymap_resolve(pr, keymap_default, keymap_fn, KC_FN, act);
    hid_build_boot_report(pr, act, rep);
    CHECK(rep[2] == 0x0C, "no Fn: 'i' sends i");
    pr[fn] = 1;
    keymap_resolve(pr, keymap_default, keymap_fn, KC_FN, act);
    hid_build_boot_report(pr, act, rep);
    CHECK(act[k] == (keymap_fn[k] ? keymap_fn[k] : 0x0C), "Fn+'i' resolves through keymap_fn");
    CHECK(act[fn] == 0, "the Fn key itself never reaches the host");
    int only_fn_code = 1;
    for (int j = 2; j < 8; j++) if (rep[j] != 0 && rep[j] != act[k]) only_fn_code = 0;
    CHECK(only_fn_code, "Fn+'i' report holds only the layer code (no 0xF0)");
    pr[k] = 0; pr[a] = 1;
    keymap_resolve(pr, keymap_default, keymap_fn, KC_FN, act);
    CHECK(act[a] == (keymap_fn[a] ? keymap_fn[a] : 0x04), "Fn+key with fn=0 falls through to base");
  }

  printf("[4] Boot report: >6 keys -> ErrorRollOver\n");
  {
    uint8_t pr[KS_NUM_KEYS]; memset(pr, 0, sizeof pr);
    uint8_t rep[HID_BOOT_REPORT_LEN];
    /* 7 distinct non-modifier keys: '1'..'7' (0x1E..0x24), located by keycode */
    for (int i = 0; i < 7; i++) pr[site_of((uint8_t)(0x1E + i))] = 1;
    hid_build_boot_report(pr, keymap_default, rep);
    int rollover = 1;
    for (int i = 2; i < 8; i++) if (rep[i] != 0x01) rollover = 0;
    CHECK(rollover, "all six slots = 0x01 ErrorRollOver");
  }

  printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
