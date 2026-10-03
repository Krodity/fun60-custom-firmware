/*
 * monsgeek_class.c - USB HID class handler for the Fun60 Ultra firmware.
 *
 * Routes vendor 64-byte FEATURE reports (HID GET_REPORT / SET_REPORT, type
 * FEATURE) to monsgeek_vendor_dispatch() and serves the per-interface HID report
 * descriptors.
 */
#include "usbd_core.h"
#include "monsgeek_class.h"
#include "monsgeek_desc.h"
#include "descriptors.h"

#define HID_REPORT_TYPE_INPUT     1
#define HID_REPORT_TYPE_OUTPUT    2
#define HID_REPORT_TYPE_FEATURE   3

void board_enter_bootloader(void);   /* main.c */

static usb_sts_type class_init_handler(void *udev);
static usb_sts_type class_clear_handler(void *udev);
static usb_sts_type class_setup_handler(void *udev, usb_setup_type *setup);
static usb_sts_type class_ept0_tx_handler(void *udev);
static usb_sts_type class_ept0_rx_handler(void *udev);
static usb_sts_type class_in_handler(void *udev, uint8_t ept_num);
static usb_sts_type class_out_handler(void *udev, uint8_t ept_num);
static usb_sts_type class_sof_handler(void *udev);
static usb_sts_type class_event_handler(void *udev, usbd_event_type event);

static monsgeek_class_t monsgeek_struct;

usbd_class_handler monsgeek_class_handler =
{
  class_init_handler,
  class_clear_handler,
  class_setup_handler,
  class_ept0_tx_handler,
  class_ept0_rx_handler,
  class_in_handler,
  class_out_handler,
  class_sof_handler,
  class_event_handler,
  &monsgeek_struct
};

static usb_sts_type class_init_handler(void *udev)
{
  usbd_core_type *pudev = (usbd_core_type *)udev;
  monsgeek_class_t *p = (monsgeek_class_t *)pudev->class_handler->pdata;

  /* IF0 boot keyboard: interrupt IN, 8 byte */
  for (unsigned e = 0; e < 8; e++) g_ep_busy[e] = 0;
  usbd_ept_open(pudev, MG_EP_BOOT_KBD_IN, EPT_INT_TYPE, 8);
  /* IF1 extended: interrupt IN, 64 byte */
  usbd_ept_open(pudev, MG_EP_EXT_IN, EPT_INT_TYPE, MONSGEEK_REPORT_SIZE);
  /* IF2 vendor: feature reports over EP0; idle interrupt IN so usbhid binds it */
  usbd_ept_open(pudev, MG_EP_VENDOR_IN, EPT_INT_TYPE, MONSGEEK_REPORT_SIZE);

  p->hid_state = 0;
  monsgeek_state_init(&p->state);
  return USB_OK;
}

static usb_sts_type class_clear_handler(void *udev)
{
  usbd_core_type *pudev = (usbd_core_type *)udev;
  usbd_ept_close(pudev, MG_EP_BOOT_KBD_IN);
  usbd_ept_close(pudev, MG_EP_EXT_IN);
  usbd_ept_close(pudev, MG_EP_VENDOR_IN);
  return USB_OK;
}

/* return the HID report descriptor for a given interface number */
static const uint8_t *report_desc_for_iface(uint16_t iface, uint16_t *len)
{
  switch (iface)
  {
    case 0: *len = MG_BOOT_KBD_REPORT_SIZE; return monsgeek_boot_kbd_report;
    case 1: *len = MG_EXT_REPORT_SIZE;      return monsgeek_ext_report;
    case 2: *len = MG_VENDOR_REPORT_SIZE;   return monsgeek_vendor_report;
    default: *len = 0; return 0;
  }
}

/* build the 9-byte HID functional descriptor for an interface */
static void build_hid_desc(monsgeek_class_t *p, uint16_t iface)
{
  uint16_t rlen = 0;
  report_desc_for_iface(iface, &rlen);
  p->hid_desc[0] = 0x09;          /* bLength */
  p->hid_desc[1] = HID_CLASS_DESC_HID;
  p->hid_desc[2] = 0x11;          /* bcdHID 0x0111 */
  p->hid_desc[3] = 0x01;
  p->hid_desc[4] = 0x00;          /* country */
  p->hid_desc[5] = 0x01;          /* numDescriptors */
  p->hid_desc[6] = HID_CLASS_DESC_REPORT;
  p->hid_desc[7] = (uint8_t)(rlen & 0xFF);
  p->hid_desc[8] = (uint8_t)(rlen >> 8);
}

static usb_sts_type class_setup_handler(void *udev, usb_setup_type *setup)
{
  usbd_core_type *pudev = (usbd_core_type *)udev;
  monsgeek_class_t *p = (monsgeek_class_t *)pudev->class_handler->pdata;
  uint16_t len;
  const uint8_t *buf;

  switch (setup->bmRequestType & USB_REQ_TYPE_RESERVED)
  {
    /* ---- HID class requests ---- */
    case USB_REQ_TYPE_CLASS:
      switch (setup->bRequest)
      {
        case HID_REQ_SET_PROTOCOL:
          p->protocol = (uint8_t)setup->wValue;
          break;
        case HID_REQ_GET_PROTOCOL:
          usbd_ctrl_send(pudev, (uint8_t *)&p->protocol, 1);
          break;
        case HID_REQ_SET_IDLE:
          p->idle_state = (uint8_t)(setup->wValue >> 8);
          break;
        case HID_REQ_GET_IDLE:
          usbd_ctrl_send(pudev, (uint8_t *)&p->idle_state, 1);
          break;
        case HID_REQ_SET_REPORT:
          /* receive the data stage; process it in ept0_rx */
          p->hid_state = HID_REQ_SET_REPORT;
          p->set_report_type = (uint8_t)(setup->wValue >> 8);
          len = setup->wLength;
          if (len > MONSGEEK_REPORT_SIZE) len = MONSGEEK_REPORT_SIZE;
          usbd_ctrl_recv(pudev, p->scratch, len);
          break;
        case HID_REQ_GET_REPORT:
          /* FEATURE read -> return the current vendor report buffer */
          len = setup->wLength;
          if (len > MONSGEEK_REPORT_SIZE) len = MONSGEEK_REPORT_SIZE;
          usbd_ctrl_send(pudev, p->vendor_report, len);
          break;
        default:
          usbd_ctrl_unsupport(pudev);
          break;
      }
      break;

    /* ---- standard requests addressed to an interface ---- */
    case USB_REQ_TYPE_STANDARD:
      switch (setup->bRequest)
      {
        case USB_STD_REQ_GET_DESCRIPTOR:
          if ((setup->wValue >> 8) == HID_REPORT_DESC)
          {
            uint16_t rlen = 0;
            buf = report_desc_for_iface(setup->wIndex, &rlen);
            if (buf == 0) { usbd_ctrl_unsupport(pudev); break; }
            len = MIN(rlen, setup->wLength);
            usbd_ctrl_send(pudev, (uint8_t *)buf, len);
          }
          else if ((setup->wValue >> 8) == HID_DESCRIPTOR_TYPE)
          {
            build_hid_desc(p, setup->wIndex);
            len = MIN(9, setup->wLength);
            usbd_ctrl_send(pudev, p->hid_desc, len);
          }
          else
          {
            usbd_ctrl_unsupport(pudev);
          }
          break;
        case USB_STD_REQ_GET_INTERFACE:
          if (setup->wIndex < MONSGEEK_HID_IFACE_COUNT)
            usbd_ctrl_send(pudev, (uint8_t *)&p->alt_setting[setup->wIndex], 1);
          else
            usbd_ctrl_unsupport(pudev);
          break;
        case USB_STD_REQ_SET_INTERFACE:
          if (setup->wIndex < MONSGEEK_HID_IFACE_COUNT)
            p->alt_setting[setup->wIndex] = (uint8_t)setup->wValue;
          break;
        case USB_STD_REQ_CLEAR_FEATURE:
        case USB_STD_REQ_SET_FEATURE:
          break;
        default:
          usbd_ctrl_unsupport(pudev);
          break;
      }
      break;

    default:
      usbd_ctrl_unsupport(pudev);
      break;
  }
  return USB_OK;
}

static usb_sts_type class_ept0_tx_handler(void *udev)
{
  (void)udev;
  return USB_OK;
}

/* SET_REPORT data stage complete: process the received report */
static usb_sts_type class_ept0_rx_handler(void *udev)
{
  usbd_core_type *pudev = (usbd_core_type *)udev;
  monsgeek_class_t *p = (monsgeek_class_t *)pudev->class_handler->pdata;
  uint32_t recv_len = usbd_get_recv_len(pudev, 0);

  if (p->hid_state == HID_REQ_SET_REPORT)
  {
    p->hid_state = 0;
    /* Only FEATURE reports are vendor commands. Output reports (keyboard LEDs on
     * IF0) are accepted and ignored in this skeleton. */
    if (p->set_report_type == HID_REPORT_TYPE_FEATURE && recv_len >= 1)
    {
      uint32_t i;
      for (i = 0; i < MONSGEEK_REPORT_SIZE; i++)
        p->vendor_report[i] = (i < recv_len) ? p->scratch[i] : 0;
      /* run the dispatcher in place; the response is read back via GET_REPORT */
      monsgeek_vendor_dispatch(&p->state, p->vendor_report);
      /* act on enter-bootloader here (IRQ context) so it never depends on the main loop */
      if (p->state.enter_bootloader) board_enter_bootloader();
    }
  }
  return USB_OK;
}

/* IN-endpoint busy flags: set by the sender, cleared here when the host has
 * taken the data. Lets main.c avoid overwriting an in-flight report (which
 * silently dropped key releases -> "stuck" keys). */
volatile uint8_t g_ep_busy[8];

static usb_sts_type class_in_handler(void *udev, uint8_t ept_num)
{
  (void)udev;
  g_ep_busy[ept_num & 0x07u] = 0;
  return USB_OK;
}

static usb_sts_type class_out_handler(void *udev, uint8_t ept_num)
{
  (void)udev; (void)ept_num;
  return USB_OK;
}

static usb_sts_type class_sof_handler(void *udev)
{
  (void)udev;
  return USB_OK;
}

static usb_sts_type class_event_handler(void *udev, usbd_event_type event)
{
  (void)udev; (void)event;
  return USB_OK;
}

usb_sts_type monsgeek_send_event(void *udev, uint8_t *report32)
{
  usbd_core_type *pudev = (usbd_core_type *)udev;
  if (usbd_connect_state_get(pudev) != USB_CONN_STATE_CONFIGURED)
    return USB_FAIL;
  usbd_ept_send(pudev, MG_EP_EXT_IN, report32, MONSGEEK_REPORT_SIZE);
  return USB_OK;
}
