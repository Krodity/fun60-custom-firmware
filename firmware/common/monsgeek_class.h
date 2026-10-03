/*
 * monsgeek_class.h - usbd_class_handler for the Fun60 Ultra firmware.
 *
 * Implements the three HID interfaces:
 *   IF0 boot keyboard  (EP 0x81 IN)
 *   IF1 extended HID   (EP 0x82 IN)
 *   IF2 vendor config  (64-byte FEATURE reports over EP0)
 * Vendor feature reports are handed to monsgeek_vendor_dispatch().
 */
#ifndef MONSGEEK_CLASS_H
#define MONSGEEK_CLASS_H

#include "usb_std.h"
#include "usbd_core.h"
#include "vendor_proto.h"

#define MONSGEEK_HID_IFACE_COUNT   3

typedef struct
{
  uint32_t alt_setting[MONSGEEK_HID_IFACE_COUNT];
  uint8_t  protocol;
  uint8_t  idle_state;
  uint8_t  hid_state;                 /* != 0 while a SET_REPORT data stage is pending */
  uint8_t  set_report_type;           /* report type of the pending SET_REPORT (2=Out,3=Feature) */
  uint8_t  vendor_report[MONSGEEK_REPORT_SIZE];  /* current 64-byte feature report */
  uint8_t  scratch[MONSGEEK_REPORT_SIZE];        /* SET_REPORT receive buffer       */
  uint8_t  ext_in_buf[MONSGEEK_REPORT_SIZE];     /* EP2 IN (events) buffer          */
  uint8_t  hid_desc[9];               /* scratch for HID functional descriptor      */
  monsgeek_state_t state;             /* live vendor settings                        */
}monsgeek_class_t;

extern usbd_class_handler monsgeek_class_handler;

/* send a 32-byte unsolicited event (VenderMsg, report ID 5) over EP2 IN */
usb_sts_type monsgeek_send_event(void *udev, uint8_t *report32);
extern volatile uint8_t g_ep_busy[8];   /* IN endpoint in flight (cleared on completion) */

#endif /* MONSGEEK_CLASS_H */
