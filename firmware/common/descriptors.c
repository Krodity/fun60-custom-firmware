/*
 * descriptors.c - MonsGeek RY5088 USB descriptor byte tables (pure data).
 *
 * No BSP dependency: only <stdint.h>. Verified exactly by the host test.
 */
#include "descriptors.h"

#define LB(x)  ((uint8_t)((x) & 0xFF))
#define HB(x)  ((uint8_t)(((x) >> 8) & 0xFF))

/* ----------------------------------------------------------------------------
 * Device descriptor (18 bytes)
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_device_desc[MG_DEVICE_DESC_SIZE] =
{
  0x12,                 /* bLength                                            */
  0x01,                 /* bDescriptorType = DEVICE                           */
  0x00, 0x02,           /* bcdUSB 2.00                                        */
  0x00,                 /* bDeviceClass    (defined at interface level)       */
  0x00,                 /* bDeviceSubClass                                    */
  0x00,                 /* bDeviceProtocol                                    */
  0x40,                 /* bMaxPacketSize0 = 64                               */
  LB(MONSGEEK_VENDOR_ID),  HB(MONSGEEK_VENDOR_ID),   /* idVendor  0x3151      */
  LB(MONSGEEK_PRODUCT_ID), HB(MONSGEEK_PRODUCT_ID),  /* idProduct (board_config) */
  LB(MONSGEEK_BCD_DEVICE), HB(MONSGEEK_BCD_DEVICE),  /* bcdDevice (board_config) */
  0x01,                 /* iManufacturer  -> "MonsGeek Keyboard"              */
  0x02,                 /* iProduct       -> " FUN60 Ultra-1"                 */
  0x03,                 /* iSerialNumber  -> chip UID                         */
  0x01                  /* bNumConfigurations                                 */
};

/* ----------------------------------------------------------------------------
 * Configuration descriptor (77 bytes): 3 interfaces.
 *   IF0 boot keyboard  (EP 0x81 IN, 8B)
 *   IF1 extended HID   (EP 0x82 IN, 64B) - NKRO/system/consumer/vendor-events
 *   IF2 vendor config  (no EP; 64B feature reports over EP0)  <- vendor IF
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_config_desc[MG_CONFIG_DESC_SIZE] =
{
  /* --- configuration header --- */
  0x09, 0x02,
  LB(MG_CONFIG_DESC_SIZE), HB(MG_CONFIG_DESC_SIZE),  /* wTotalLength = 84      */
  0x03,                 /* bNumInterfaces = 3                                 */
  0x01,                 /* bConfigurationValue                                */
  0x00,                 /* iConfiguration                                     */
  0xA0,                 /* bmAttributes: bus powered + remote wakeup          */
  0xFA,                 /* bMaxPower = 500 mA                                 */

  /* --- IF0: boot keyboard --- */
  0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        /* INTERFACE if=0 alt=0 nEP=1 class=HID sub=Boot proto=Keyboard       */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
        LB(MG_BOOT_KBD_REPORT_SIZE), HB(MG_BOOT_KBD_REPORT_SIZE),  /* HID desc */
  0x07, 0x05, MG_EP_BOOT_KBD_IN, 0x03, 0x08, 0x00, 0x01,
        /* ENDPOINT 0x81 interrupt, 8 byte, bInterval 1 ms                    */

  /* --- IF1: extended (NKRO + system + consumer + vendor events) --- */
  0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        /* INTERFACE if=1 alt=0 nEP=1 class=HID sub=0 proto=0                 */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
        LB(MG_EXT_REPORT_SIZE), HB(MG_EXT_REPORT_SIZE),            /* HID desc */
  0x07, 0x05, MG_EP_EXT_IN, 0x03, 0x40, 0x00, 0x01,
        /* ENDPOINT 0x82 interrupt, 64 byte, bInterval 1 ms                   */

  /* --- IF2: vendor config (feature reports over EP0 + idle interrupt IN) --- */
  0x09, 0x04, 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        /* INTERFACE if=2 alt=0 nEP=1 class=HID sub=0 proto=0                 */
  0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22,
        LB(MG_VENDOR_REPORT_SIZE), HB(MG_VENDOR_REPORT_SIZE),      /* HID desc */
  0x07, 0x05, MG_EP_VENDOR_IN, 0x03, 0x40, 0x00, 0x01,
        /* ENDPOINT 0x83 interrupt, 64 byte, bInterval 1 (never sent on)      */
};

/* ----------------------------------------------------------------------------
 * Device qualifier (10 bytes) - full speed device, returns 0 other-speed.
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_qualifier_desc[MG_QUALIFIER_DESC_SIZE] =
{
  0x0A, 0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x01, 0x00
};

/* ----------------------------------------------------------------------------
 * IF0 boot keyboard report descriptor (63 bytes) - standard 8-byte boot report.
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_boot_kbd_report[MG_BOOT_KBD_REPORT_SIZE] =
{
  0x05, 0x01,   /* Usage Page (Generic Desktop)        */
  0x09, 0x06,   /* Usage (Keyboard)                    */
  0xA1, 0x01,   /* Collection (Application)            */
  0x05, 0x07,   /*   Usage Page (Keyboard)             */
  0x19, 0xE0,   /*   Usage Minimum (LeftControl)       */
  0x29, 0xE7,   /*   Usage Maximum (Right GUI)         */
  0x15, 0x00,   /*   Logical Minimum (0)               */
  0x25, 0x01,   /*   Logical Maximum (1)               */
  0x75, 0x01,   /*   Report Size (1)                   */
  0x95, 0x08,   /*   Report Count (8)                  */
  0x81, 0x02,   /*   Input (Data,Var,Abs) - modifiers  */
  0x95, 0x01,   /*   Report Count (1)                  */
  0x75, 0x08,   /*   Report Size (8)                   */
  0x81, 0x03,   /*   Input (Cnst,Var,Abs) - reserved   */
  0x95, 0x05,   /*   Report Count (5)                  */
  0x75, 0x01,   /*   Report Size (1)                   */
  0x05, 0x08,   /*   Usage Page (LEDs)                 */
  0x19, 0x01,   /*   Usage Minimum (Num Lock)          */
  0x29, 0x05,   /*   Usage Maximum (Kana)              */
  0x91, 0x02,   /*   Output (Data,Var,Abs) - LEDs      */
  0x95, 0x01,   /*   Report Count (1)                  */
  0x75, 0x03,   /*   Report Size (3)                   */
  0x91, 0x03,   /*   Output (Cnst,Var,Abs) - pad       */
  0x95, 0x06,   /*   Report Count (6)                  */
  0x75, 0x08,   /*   Report Size (8)                   */
  0x15, 0x00,   /*   Logical Minimum (0)               */
  0x25, 0xFF,   /*   Logical Maximum (255)             */
  0x05, 0x07,   /*   Usage Page (Keyboard)             */
  0x19, 0x00,   /*   Usage Minimum (0)                 */
  0x29, 0x65,   /*   Usage Maximum (Application)       */
  0x81, 0x00,   /*   Input (Data,Ary,Abs) - keys       */
  0xC0          /* End Collection                      */
};

/* ----------------------------------------------------------------------------
 * IF1 extended report descriptor (137 bytes).
 * Report IDs (EP2 0x82 multiplex):
 *   ID 1 = NKRO keyboard (16B), ID 2 = system (3B), ID 3 = consumer (3B),
 *   ID 5 = vendor events / VenderMsg (32B), ID 6 = vendor (8B).
 * (IF1's exact layout is not part of the vendor contract; it exists so the
 * vendor interface lands at index 2.)
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_ext_report[MG_EXT_REPORT_SIZE] =
{
  /* -- collection 1: NKRO keyboard, Report ID 1 (41 bytes) -- */
  0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x6F, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x70, 0x81, 0x02,
  0xC0,
  /* -- collection 2: System control, Report ID 2 (25 bytes) -- */
  0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
  0xC0,
  /* -- collection 3: Consumer control, Report ID 3 (25 bytes) -- */
  0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
  0xC0,
  /* -- collection 5: vendor events, Report ID 5, 31 data bytes (23 bytes) -- */
  0x06, 0x00, 0xFF, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x05,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x1F, 0x09, 0x01, 0x81, 0x00,
  0xC0,
  /* -- collection 6: vendor, Report ID 6, 7 data bytes (23 bytes) -- */
  0x06, 0x00, 0xFF, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x06,
    0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x07, 0x09, 0x02, 0x81, 0x00,
  0xC0
};

/* ----------------------------------------------------------------------------
 * IF2 vendor config report descriptor - the EXACT 20 bytes the app validates:
 *   06 ff ff 09 02 a1 01 09 02 15 80 25 7f 95 40 75 08 b1 02 c0
 *   Usage Page 0xFFFF, Usage 2, Collection App, 64x8-bit FEATURE, report ID 0.
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_vendor_report[MG_VENDOR_REPORT_SIZE] =
{
  0x06, 0xFF, 0xFF,  /* Usage Page (0xFFFF, vendor defined)   */
  0x09, 0x02,        /* Usage (0x02)                          */
  0xA1, 0x01,        /* Collection (Application)              */
  0x09, 0x02,        /*   Usage (0x02)                        */
  0x15, 0x80,        /*   Logical Minimum (-128)              */
  0x25, 0x7F,        /*   Logical Maximum (127)               */
  0x95, 0x40,        /*   Report Count (64)                   */
  0x75, 0x08,        /*   Report Size (8)                     */
  0xB1, 0x02,        /*   Feature (Data,Var,Abs)              */
  0xC0               /* End Collection                        */
};

/* ----------------------------------------------------------------------------
 * String 0: LANGID = 0x0409 (English, US)
 * -------------------------------------------------------------------------- */
const uint8_t monsgeek_langid_desc[4] = { 0x04, 0x03, 0x09, 0x04 };
