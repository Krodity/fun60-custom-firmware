/*
 * hid_report.c - HID keyboard report assembly (boot 8-byte + NKRO bitmap).
 */
#include "hid_report.h"

uint8_t hid_build_boot_report(const uint8_t *pressed, const uint8_t *keymap, uint8_t *out)
{
  uint8_t mod = 0;
  uint8_t keys[6];
  uint8_t nkeys = 0;
  uint8_t overflow = 0;

  for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
    if (!pressed[i]) continue;
    uint8_t usage = keymap[i];
    if (usage == 0x00 || usage >= 0xE8) continue;   /* 0xE8+: internal codes (KC_FN) */
    if (usage >= 0xE0 && usage <= 0xE7) {
      mod |= (uint8_t)(1u << (usage - 0xE0));
      continue;
    }
    if (nkeys < 6) {
      /* de-dupe so two physical keys mapped the same don't fill two slots */
      uint8_t dup = 0;
      for (uint8_t j = 0; j < nkeys; j++) if (keys[j] == usage) { dup = 1; break; }
      if (!dup) keys[nkeys++] = usage;
    } else {
      overflow = 1;
    }
  }

  out[0] = mod;
  out[1] = 0;
  if (overflow) {
    for (unsigned k = 2; k < 8; k++) out[k] = 0x01;  /* ErrorRollOver */
    return 6;
  }
  for (unsigned k = 0; k < 6; k++) out[2 + k] = (k < nkeys) ? keys[k] : 0x00;
  return nkeys;
}

void hid_build_nkro_bitmap(const uint8_t *pressed, const uint8_t *keymap, uint8_t *out)
{
  for (unsigned i = 0; i < HID_NKRO_BITMAP_LEN; i++) out[i] = 0;
  for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
    if (!pressed[i]) continue;
    uint8_t usage = keymap[i];
    if (usage == 0x00 || usage >= 0xE8) continue;   /* 0xE8+: internal codes (KC_FN) */
    out[usage >> 3] |= (uint8_t)(1u << (usage & 7));
  }
}

void keymap_resolve(const uint8_t *pressed, const uint8_t *base, const uint8_t *fn,
                    uint8_t fn_code, uint8_t *out)
{
  uint8_t fn_on = 0;
  for (unsigned i = 0; i < KS_NUM_KEYS; i++)
    if (pressed[i] && base[i] == fn_code) { fn_on = 1; break; }
  for (unsigned i = 0; i < KS_NUM_KEYS; i++) {
    if (base[i] == fn_code)        out[i] = 0;
    else if (fn_on && fn[i] != 0)  out[i] = fn[i];
    else                           out[i] = base[i];
  }
}
