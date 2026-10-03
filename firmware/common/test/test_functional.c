/*
 * test_functional.c - host-side self-test for the firmware feature set:
 *   [A] vendor GET/SET round-trip (LED params, profile, debounce)
 *   [B] LED effect engine (Off / Static / Breathing / Wave / per-key Static)
 *   [C] flash-config persistence over a mock EFC (blank->defaults, save/reload)
 *   [D] magnetism SET/GET round-trip (multi-magnetism + key mode)
 * Pure logic; builds natively (no AT32/USB). Run via `make test`.
 */
#include <stdio.h>
#include <string.h>
#include "vendor_proto.h"
#include "led_effects.h"
#include "persist.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
  if (!(cond)) { printf("  FAIL: %s\n", msg); fails++; } \
  else         { printf("  ok  : %s\n", msg); } } while (0)

/* stamp the correct checksum (Bit8 for LED 0x07/0x08, else Bit7) and dispatch */
static int disp(monsgeek_state_t *st, uint8_t *r)
{
  if (r[0] == FEA_SET_LEDPARAM || r[0] == FEA_SET_SLEDPARAM) monsgeek_emit_bit8(r);
  else                                                       monsgeek_emit_bit7(r);
  return monsgeek_vendor_dispatch(st, r);
}
static void clr(uint8_t *r) { memset(r, 0, MONSGEEK_REPORT_SIZE); }

/* ---- mock EFC backend (NOR semantics: erase->0xFF, program clears bits) ---- */
static uint8_t mock_flash[CONFIG_SECTOR_SIZE];
static int mock_read(uint32_t a, void *d, uint32_t n)
{ memcpy(d, mock_flash + (a - CONFIG_FLASH_ADDR), n); return 0; }
static int mock_erase(uint32_t a)
{ (void)a; memset(mock_flash, 0xFF, sizeof mock_flash); return 0; }
static int mock_program(uint32_t a, const void *s, uint32_t n)
{ const uint8_t *p = s; uint32_t i, off = a - CONFIG_FLASH_ADDR;
  for (i = 0; i < n; i++) mock_flash[off + i] &= p[i];   /* program = AND */
  return 0; }
static const flash_ops_t mock_ops = { mock_read, mock_erase, mock_program };

int main(void)
{
  monsgeek_state_t st;
  uint8_t r[MONSGEEK_REPORT_SIZE];

  printf("[A] vendor GET/SET round-trip\n");
  monsgeek_state_init(&st);
  /* SET_PROFILE then GET_PROFILE */
  clr(r); r[0] = FEA_SET_PROFILE; r[1] = 2; disp(&st, r);
  clr(r); r[0] = FEA_GET_PROFILE; disp(&st, r);
  CHECK(r[1] == 2, "profile set/get round-trips");
  /* SET_DEBOUNCE then GET_DEBOUNCE */
  clr(r); r[0] = FEA_SET_DEBOUNCE; r[1] = 8; disp(&st, r);
  clr(r); r[0] = FEA_GET_DEBOUNCE; disp(&st, r);
  CHECK(r[1] == 8, "debounce set/get round-trips");
  /* SET_LEDPARAM [mode][speed][bright][opts][R][G][B] then GET_LEDPARAM */
  clr(r); r[0] = FEA_SET_LEDPARAM;
  r[1]=2; r[2]=3; r[3]=1; r[4]=0x18; r[5]=10; r[6]=20; r[7]=30;
  CHECK(disp(&st, r) == 1, "SET_LEDPARAM accepted (Bit8 checksum)");
  clr(r); r[0] = FEA_GET_LEDPARAM; disp(&st, r);
  CHECK(r[1]==2 && r[2]==3 && r[3]==1 && r[4]==0x18 && r[5]==10 && r[6]==20 && r[7]==30,
        "LED param [mode/speed/bright/opts/R/G/B] round-trips exactly");
  /* a bad checksum must be ignored */
  clr(r); r[0] = FEA_GET_PROFILE; r[7] = 0x00; /* deliberately wrong */
  CHECK(monsgeek_vendor_dispatch(&st, r) == 0, "bad checksum is rejected");

  printf("[B] LED effect engine\n");
  {
    led_frame_t f;
    led_params_t p;
    unsigned i; int uniform, anydiff;

    /* Off */
    memset(&p, 0, sizeof p); p.mode = LED_MODE_OFF; p.brightness = 4;
    led_effects_render(&p, NULL, 0, f);
    uniform = 1; for (i = 0; i < MG_NUM_LEDS; i++)
      if (f[i][0] || f[i][1] || f[i][2]) uniform = 0;
    CHECK(uniform, "Off => every LED dark");

    /* Static red, full brightness */
    memset(&p, 0, sizeof p);
    p.mode = LED_MODE_STATIC; p.brightness = 4; p.flag = LED_OPT_FLAG_NORMAL;
    p.r = 255; p.g = 0; p.b = 0;
    led_effects_render(&p, NULL, 0, f);
    uniform = 1; for (i = 0; i < MG_NUM_LEDS; i++)
      if (!(f[i][0] == 255 && f[i][1] == 0 && f[i][2] == 0)) uniform = 0;
    CHECK(uniform, "Static red => every LED (255,0,0) at full brightness");

    /* Random rainbow: LEDs differ from each other, and each one moves over time */
    memset(&p, 0, sizeof p);
    p.mode = LED_MODE_RANDOM_RAINBOW; p.brightness = 4; p.speed = 3; p.flag = 8;
    led_effects_render(&p, NULL, 0, f);
    anydiff = 0; for (i = 1; i < MG_NUM_LEDS; i++) if (memcmp(f[i], f[0], 3)) anydiff = 1;
    CHECK(anydiff, "Random rainbow => LEDs start at different colours");
    {
      led_frame_t f2; int moved = 0;
      led_effects_render(&p, NULL, 900, f2);
      for (i = 0; i < MG_NUM_LEDS; i++) if (memcmp(f[i], f2[i], 3)) moved++;
      CHECK(moved > (int)(MG_NUM_LEDS / 2), "Random rainbow => colours cycle over time");
    }

    /* Breathing: uniform across LEDs, but the envelope changes over time */
    memset(&p, 0, sizeof p);
    p.mode = LED_MODE_BREATHING; p.brightness = 4; p.speed = 3;
    p.flag = LED_OPT_FLAG_NORMAL; p.r = 0; p.g = 0; p.b = 255;
    led_effects_render(&p, NULL, 0, f);
    uniform = 1; for (i = 1; i < MG_NUM_LEDS; i++)
      if (memcmp(f[i], f[0], 3)) uniform = 0;
    CHECK(uniform, "Breathing => all LEDs share one colour each frame");
    {
      led_frame_t f2;
      led_effects_render(&p, NULL, 700, f2);   /* later phase */
      CHECK(memcmp(f[0], f2[0], 3) != 0, "Breathing envelope changes over time");
    }

    /* Wave: gradient across the board (not all LEDs equal) */
    memset(&p, 0, sizeof p);
    p.mode = LED_MODE_WAVE; p.brightness = 4; p.speed = 2; p.flag = LED_OPT_FLAG_DAZZLE;
    led_effects_render(&p, NULL, 0, f);
    anydiff = 0; for (i = 1; i < MG_NUM_LEDS; i++)
      if (memcmp(f[i], f[0], 3)) { anydiff = 1; break; }
    CHECK(anydiff, "Wave => spatial gradient (LEDs differ)");

    /* per-key Static from userpic */
    {
      uint8_t up[MG_NUM_LEDS][3];
      for (i = 0; i < MG_NUM_LEDS; i++) { up[i][0]=(uint8_t)i; up[i][1]=0; up[i][2]=99; }
      memset(&p, 0, sizeof p); p.mode = LED_MODE_USERPIC; p.brightness = 4;
      led_effects_render(&p, up, 0, f);
      CHECK(f[5][0]==5 && f[5][2]==99 && f[7][0]==7,
            "UserPicture => each LED shows its own stored colour");
    }
  }

  printf("[C] flash-config persistence (mock EFC)\n");
  {
    monsgeek_state_t a, b;
    int was_valid;

    /* image fits inside one 2 KB sector */
    CHECK(persist_image_size() <= CONFIG_SECTOR_SIZE, "config image fits in a 2 KB sector");

    /* blank flash -> clean factory defaults, reported as "was blank" */
    memset(mock_flash, 0xFF, sizeof mock_flash);
    was_valid = persist_load(&mock_ops, &a);
    CHECK(was_valid == 0, "blank flash reports first-boot");
    CHECK(a.cfg[0x08] == LED_MODE_WAVE && a.mag_press[0] == 200,
          "first-boot yields clean defaults (Wave LED, 2.0 mm actuation)");

    /* mutate, save, reload into a fresh state -> identical */
    clr(r); r[0]=FEA_SET_PROFILE;  r[1]=3; disp(&a, r);
    clr(r); r[0]=FEA_SET_DEBOUNCE; r[1]=9; disp(&a, r);
    clr(r); r[0]=FEA_SET_LEDPARAM; r[1]=2; r[2]=4; r[3]=2; r[4]=0x07;
            r[5]=1; r[6]=2; r[7]=3; disp(&a, r);
    clr(r); r[0]=FEA_SET_MULTI_MAG; r[1]=MAG_SUB_PRESS_TRAVEL; r[2]=0; r[3]=10;
            r[8]=0x2C; r[9]=0x01; disp(&a, r);     /* key10 press = 300 */
    CHECK(a.cfg_dirty == 1, "a SET marks the config dirty");
    CHECK(persist_save(&mock_ops, &a) == 0, "persist_save succeeds");
    CHECK(a.cfg_dirty == 0, "persist_save clears the dirty flag");

    was_valid = persist_load(&mock_ops, &b);
    CHECK(was_valid == 1, "saved image loads as valid");
    CHECK(b.cfg[0x00]==3 && b.cfg[0x09]==9 && b.cfg[0x08]==2,
          "profile/debounce/LED-mode survive save+reload");
    CHECK(b.rgb[MG_LED_TBL_RGB + 2*3 + 0]==1 && b.mag_press[10]==300,
          "LED colour + per-key magnetism survive save+reload");

    /* corrupting a byte inside the CRC-covered image must fall back to defaults */
    mock_flash[64] ^= 0xFF;     /* offset 64 is within the config header */
    CHECK(persist_load(&mock_ops, &b) == 0, "corrupt image falls back to defaults");
  }

  printf("[D] magnetism SET/GET round-trip\n");
  {
    monsgeek_state_init(&st);
    /* single-key actuation: key 5 -> 250 (0x00FA) */
    clr(r); r[0]=FEA_SET_MULTI_MAG; r[1]=MAG_SUB_PRESS_TRAVEL; r[2]=0; r[3]=5;
            r[8]=0xFA; r[9]=0x00; disp(&st, r);
    CHECK(st.mag_press[5] == 250, "SET_MULTI_MAG single key writes actuation");
    /* GET page 0 -> raw 32xu16; key5 at bytes [10],[11] */
    clr(r); r[0]=FEA_GET_MULTI_MAG; r[1]=MAG_SUB_PRESS_TRAVEL; r[2]=1; r[4]=0;
    disp(&st, r);
    CHECK(r[10]==0xFA && r[11]==0x00, "GET_MULTI_MAG returns raw page (key5=250)");
    /* default key (untouched) still reads 200 */
    CHECK(r[0]==0xC8 && r[1]==0x00, "GET_MULTI_MAG key0 = default 200 (2.0 mm)");
    /* global key mode 0x1D -> 0x9D */
    clr(r); r[0]=FEA_SET_KEY_MAG_MODE; r[1]=1; disp(&st, r);
    CHECK(st.mag_global_mode==1 && st.mag_mode[0]==1 && st.mag_mode[40]==1,
          "SET_KEY_MAG_MODE applies the mode to every key");
    clr(r); r[0]=FEA_GET_KEY_MAG_MODE; disp(&st, r);
    CHECK(r[1]==1, "GET_KEY_MAG_MODE round-trips the global mode");
  }

  printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
         fails, fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
