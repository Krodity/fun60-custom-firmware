/*
 * descriptors.h - MonsGeek Fun60 Ultra USB descriptor byte tables.
 *
 * Pure data (only <stdint.h>); shared by the descriptor handler (monsgeek_desc.c)
 * and the USB class handler (monsgeek_class.c) so the exact descriptor bytes are
 * defined in one place.
 *
 * The byte layout matches the Fun60 Ultra so the host driver enumerates and
 * recognises the device identically:
 *   - device:  VID 0x3151, PID 0x502D (configurable below)
 *   - 3 interfaces: IF0 boot keyboard, IF1 extended (NKRO/consumer/system/events),
 *     IF2 vendor config (usage_page 0xFFFF, usage 2) <- the vendor interface
 *   - IF2 report descriptor is the exact 20 bytes the app checks.
 */
#ifndef MONSGEEK_DESCRIPTORS_H
#define MONSGEEK_DESCRIPTORS_H

#include <stdint.h>
#include "board_config.h"

/* USB identity (MONSGEEK_VENDOR_ID / _PRODUCT_ID / _BCD_DEVICE) is per-board and
 * comes from the generated board_config.h. */

/* ---- report-descriptor sizes (verified at runtime in the host test) ---- */
#define MG_BOOT_KBD_REPORT_SIZE   63
#define MG_EXT_REPORT_SIZE        137
#define MG_VENDOR_REPORT_SIZE     20

/* ---- standard descriptor lengths ---- */
#define MG_DEVICE_DESC_SIZE       18
#define MG_CONFIG_DESC_SIZE       84      /* 9 + (9+9+7) + (9+9+7) + (9+9+7)      */
#define MG_QUALIFIER_DESC_SIZE    10

/* ---- endpoints ---- */
#define MG_EP_BOOT_KBD_IN         0x81    /* IF0 interrupt IN, 8-byte boot report */
#define MG_EP_EXT_IN              0x82    /* IF1 interrupt IN, NKRO/consumer/...   */
#define MG_EP_VENDOR_IN           0x83    /* IF2 interrupt IN, 64 B (idle). Feature reports still
                                           * travel over EP0, but Linux usbhid refuses to bind a HID
                                           * interface with no interrupt-IN endpoint (stock has one). */

extern const uint8_t monsgeek_device_desc[MG_DEVICE_DESC_SIZE];
extern const uint8_t monsgeek_config_desc[MG_CONFIG_DESC_SIZE];
extern const uint8_t monsgeek_qualifier_desc[MG_QUALIFIER_DESC_SIZE];
extern const uint8_t monsgeek_boot_kbd_report[MG_BOOT_KBD_REPORT_SIZE];
extern const uint8_t monsgeek_ext_report[MG_EXT_REPORT_SIZE];
extern const uint8_t monsgeek_vendor_report[MG_VENDOR_REPORT_SIZE];
extern const uint8_t monsgeek_langid_desc[4];

#endif /* MONSGEEK_DESCRIPTORS_H */
