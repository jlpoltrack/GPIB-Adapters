/* DFU-mode descriptors: one DFU interface, one alternate ("Application flash"), download only, and MS OS 2.0 so
 * Windows binds WinUSB for dfu-util without a driver. Same VID:PID as the app, its own bcdDevice. */

#include "tusb.h"
#include "image.h"
#include "app_config.h"

#define BOOT_USB_BCD 0xDF10 /* bootloader v1.0: not the app's, so Windows keeps their interface layouts apart */
#define VENDOR_REQUEST_MICROSOFT 1

enum { STRID_LANGID, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_ALT0 };

static const tusb_desc_device_t s_desc_device = {
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = 0x0201, /* 2.01: Windows asks for the BOS */
	.bDeviceClass       = 0,
	.bDeviceSubClass    = 0,
	.bDeviceProtocol    = 0,
	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor           = APP_USB_VID,
	.idProduct          = APP_USB_PID,
	.bcdDevice          = BOOT_USB_BCD,
	.iManufacturer      = STRID_MANUFACTURER,
	.iProduct           = STRID_PRODUCT,
	.iSerialNumber      = STRID_SERIAL,
	.bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void)
{
	return (const uint8_t *)&s_desc_device;
}

#define CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_DFU_DESC_LEN(1))
#define DFU_ATTRS  (DFU_ATTR_CAN_DOWNLOAD | DFU_ATTR_MANIFESTATION_TOLERANT | DFU_ATTR_WILL_DETACH)

static const uint8_t s_desc_config[] = {
	TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_LEN, 0x80, 100),
	TUD_DFU_DESCRIPTOR(0, 1, STRID_ALT0, DFU_ATTRS, 1000, CFG_TUD_DFU_XFER_BUFSIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	return s_desc_config;
}

/* MS OS 2.0 for a single-interface device: no subset headers, WinUSB on the device */
#define MS_OS_20_DESC_LEN 0xA2

static const uint8_t s_desc_ms_os_20[] = {
	U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
	U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY), U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
	'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
	'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
	U16_TO_U8S_LE(0x0050),
	'{', 0x00, 'B', 0x00, '2', 0x00, 'C', 0x00, 'A', 0x00, '1', 0x00, 'E', 0x00, 'D', 0x00, 'B', 0x00, '-', 0x00,
	'4', 0x00, '1', 0x00, 'E', 0x00, 'E', 0x00, '-', 0x00, '4', 0x00, '9', 0x00, 'C', 0x00, '2', 0x00, '-', 0x00,
	'B', 0x00, '2', 0x00, '6', 0x00, '3', 0x00, '-', 0x00, '5', 0x00, '3', 0x00, '4', 0x00, '0', 0x00, '5', 0x00,
	'E', 0x00, '5', 0x00, '9', 0x00, 'F', 0x00, 'A', 0x00, 'F', 0x00, 'E', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size");

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_desc_bos[] = {
	TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
	TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

const uint8_t *tud_descriptor_bos_cb(void)
{
	return s_desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request)
{
	if (stage != CONTROL_STAGE_SETUP)
		return true;
	if ((request->bRequest == VENDOR_REQUEST_MICROSOFT) && (request->wIndex == 7))
		return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_desc_ms_os_20, sizeof(s_desc_ms_os_20));
	return false;
}

static const char *const s_strings[] = {
	[STRID_MANUFACTURER] = APP_USB_MANUFACTURER,
	[STRID_PRODUCT]      = APP_USB_PRODUCT " DFU",
	[STRID_ALT0]         = "Application flash @0x08004000",
};

static uint16_t s_desc_str[32];

/* The serial is the 96-bit UID in hex, as the app's (board_get_uid_hex()) */
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned n = 0;

	(void)langid;
	if (index == STRID_LANGID) {
		s_desc_str[1] = 0x0409;
		n = 1;
	} else if (index == STRID_SERIAL) {
		const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u;
		for (int w = 0; w < 3; w++) {
			uint32_t v = uid[w];
			for (int i = 0; i < 4; i++, v >>= 8) {
				s_desc_str[1 + n++] = (uint16_t)hex[(v >> 4) & 0xFu];
				s_desc_str[1 + n++] = (uint16_t)hex[v & 0xFu];
			}
		}
	} else if ((index < TU_ARRAY_SIZE(s_strings)) && s_strings[index]) {
		for (const char *s = s_strings[index]; *s && (n < TU_ARRAY_SIZE(s_desc_str) - 1); s++)
			s_desc_str[1 + n++] = (uint16_t)*s;
	} else {
		return NULL;
	}
	s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * n + 2u));
	return s_desc_str;
}
