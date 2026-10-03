/*
 * hid_report.h - assemble USB HID keyboard reports from the per-key pressed[]
 *                state and a keymap. Pure logic (host-testable).
 *
 * Boot keyboard report (IF0, EP 0x81): 8 bytes
 *   [0]   modifier bitmap (LCtrl..RGui = bit0..bit7, usages 0xE0..0xE7)
 *   [1]   reserved (0)
 *   [2..7] up to 6 pressed key usages (0x01 ErrorRollOver in all six on overflow)
 */
#ifndef HID_REPORT_H
#define HID_REPORT_H

#include <stdint.h>
#include "keyscan.h"

#define HID_BOOT_REPORT_LEN  8u
#define HID_NKRO_BITMAP_LEN  16u   /* 128-bit usage bitmap (EP 0x82 NKRO body)   */

/* Build an 8-byte boot keyboard report from pressed[KS_NUM_KEYS] + keymap usages.
 * Writes exactly HID_BOOT_REPORT_LEN bytes to out. Returns the number of
 * non-modifier keys placed (0..6), or 6 when rollover occurred. */
uint8_t hid_build_boot_report(const uint8_t *pressed, const uint8_t *keymap, uint8_t *out);

/* Resolve the active layer into out[KS_NUM_KEYS]: if any pressed site maps to
 * fn_code in `base`, sites with a nonzero `fn` entry use it; everything else uses
 * `base`. The Fn site itself resolves to 0 (it never reaches the host). */
void keymap_resolve(const uint8_t *pressed, const uint8_t *base, const uint8_t *fn,
                    uint8_t fn_code, uint8_t *out);

/* Build a 128-bit NKRO usage bitmap (set bit = usage pressed). Modifiers are
 * also reflected as their usage bits. Writes HID_NKRO_BITMAP_LEN bytes. */
void hid_build_nkro_bitmap(const uint8_t *pressed, const uint8_t *keymap, uint8_t *out);

#endif /* HID_REPORT_H */
