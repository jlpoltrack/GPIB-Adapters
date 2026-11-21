#include "tusb.h"
#include "usbtmc.h"
#include "instr.h"
#include "settings.h"
#include "app_config.h"

/* USB identity from the board's app_config.h: APP_USB_VID, APP_USB_PID, APP_USB_BCD (firmware version),
 * APP_USB_MANUFACTURER, APP_USB_PRODUCT. */

enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL, STRID_CDC, STRID_DFU, STRID_NCM, STRID_MAC };

#define CDC_NOTIF_EPADDR 0x84
#define CDC_OUT_EPADDR   0x05
#define CDC_IN_EPADDR    0x85
#define NCM_NOTIF_EPADDR 0x83 /* CDC-NCM, when the board's tusb_config.h has CFG_TUD_NCM (a dev network over USB) */
#define NCM_OUT_EPADDR   0x06
#define NCM_IN_EPADDR    0x86

#define VENDOR_REQUEST_MICROSOFT 1

/* Composite with an IAD (CDC); bcdUSB 2.01 makes Windows ask for the BOS (MS OS 2.0) descriptor.
 * bcdDevice carries the interface set (settings.usb_mode), so Windows doesn't reuse a cached layout. */
static tusb_desc_device_t desc_device = {
	.bLength            = sizeof(tusb_desc_device_t),
	.bDescriptorType    = TUSB_DESC_DEVICE,
	.bcdUSB             = 0x0201,
	.bDeviceClass       = TUSB_CLASS_MISC,
	.bDeviceSubClass    = MISC_SUBCLASS_COMMON,
	.bDeviceProtocol    = MISC_PROTOCOL_IAD,
	.bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
	.idVendor           = APP_USB_VID,
	.idProduct          = APP_USB_PID,
	.bcdDevice          = APP_USB_BCD,
	.iManufacturer      = STRID_MANUFACTURER,
	.iProduct           = STRID_PRODUCT,
	.iSerialNumber      = STRID_SERIAL,
	.bNumConfigurations = 1
};

uint8_t const *tud_descriptor_device_cb(void)
{
	desc_device.bcdDevice = (uint16_t)(APP_USB_BCD | settings.usb_mode);
	return (uint8_t const *)&desc_device;
}

#define TMC_DESC_LEN     (9 + 3 * 7)
#if CFG_TUD_NCM
#define NCM_DESC_LEN     TUD_CDC_NCM_DESC_LEN
#else
#define NCM_DESC_LEN     0
#endif
#define CONFIG_MAX_LEN   (TUD_CONFIG_DESC_LEN + TMC_DESC_LEN + TUD_CDC_DESC_LEN + NCM_DESC_LEN + TUD_DFU_RT_DESC_LEN)

static uint8_t desc_configuration[CONFIG_MAX_LEN];
static uint8_t itf_dfu;

/* MS OS 2.0: WinUSB on the DFU runtime interface, so dfu-util and browsers can detach on Windows without a driver */
#define BOS_TOTAL_LEN     (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2
#define MS_OS_20_ITF_IDX  22 /* function subset: first interface */

static uint8_t desc_ms_os_20[] = {
	/* set header: length, type, Windows version, total length */
	U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR), U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
	/* configuration subset header */
	U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),
	/* function subset header: the DFU runtime interface (patched in) */
	U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), 0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),
	/* compatible ID: WINUSB */
	U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID), 'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
	U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
	U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A),
	'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00,
	'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
	U16_TO_U8S_LE(0x0050),
	'{', 0x00, 'B', 0x00, '2', 0x00, 'C', 0x00, 'A', 0x00, '1', 0x00, 'E', 0x00, 'D', 0x00, 'B', 0x00, '-', 0x00,
	'4', 0x00, '1', 0x00, 'E', 0x00, 'E', 0x00, '-', 0x00, '4', 0x00, '9', 0x00, 'C', 0x00, '2', 0x00, '-', 0x00,
	'B', 0x00, '2', 0x00, '6', 0x00, '3', 0x00, '-', 0x00, '5', 0x00, '3', 0x00, '4', 0x00, '0', 0x00, '5', 0x00,
	'E', 0x00, '5', 0x00, '9', 0x00, 'F', 0x00, 'A', 0x00, 'F', 0x00, 'E', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size");

/* Interfaces are numbered in order: [USBTMC] [CDC control, CDC data] [NCM control, NCM data] DFU runtime */
static void build_configuration(void)
{
	bool tmc = settings.usb_mode != USB_MODE_CDC, cdc = settings.usb_mode != USB_MODE_TMC;
	uint8_t itf = 0;
	uint16_t len = TUD_CONFIG_DESC_LEN;

	if (tmc) {
		uint8_t const block[] = {
			/* Interface: USBTMC (0xFE/0x03), USB488 protocol */
			9, TUSB_DESC_INTERFACE, itf, 0, 3, TUSB_CLASS_APPLICATION_SPECIFIC, 0x03, 0x01, 0,
			/* Bulk IN, bulk OUT, interrupt IN (notifications) */
			7, TUSB_DESC_ENDPOINT, TMC_IN_EPADDR, TUSB_XFER_BULK, U16_TO_U8S_LE(TMC_IO_EPSIZE), 5,
			7, TUSB_DESC_ENDPOINT, TMC_OUT_EPADDR, TUSB_XFER_BULK, U16_TO_U8S_LE(TMC_IO_EPSIZE), 5,
			7, TUSB_DESC_ENDPOINT, TMC_NOTIFICATION_EPADDR, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(TMC_NOTIFICATION_EPSIZE), 5,
		};
		memcpy(&desc_configuration[len], block, sizeof(block));
		len += sizeof(block);
		itf++;
	}
	if (cdc) { /* CDC-ACM: Prologix-style serial port */
		uint8_t const block[] = { TUD_CDC_DESCRIPTOR(itf, STRID_CDC, CDC_NOTIF_EPADDR, 8, CDC_OUT_EPADDR, CDC_IN_EPADDR, 64) };
		memcpy(&desc_configuration[len], block, sizeof(block));
		len += sizeof(block);
		itf += 2;
	}
#if CFG_TUD_NCM
	{
		uint8_t const block[] = { TUD_CDC_NCM_DESCRIPTOR(itf, STRID_NCM, STRID_MAC, NCM_NOTIF_EPADDR, 64, NCM_OUT_EPADDR,
		                                                 NCM_IN_EPADDR, 64, CFG_TUD_NET_MTU, 50, 0) };
		memcpy(&desc_configuration[len], block, sizeof(block));
		len += sizeof(block);
		itf += 2;
	}
#endif
	{ /* DFU runtime: DFU_DETACH reboots into the bootloader (board_enter_bootloader()) */
		uint8_t const block[] = { TUD_DFU_RT_DESCRIPTOR(itf, STRID_DFU, DFU_ATTR_WILL_DETACH | DFU_ATTR_CAN_DOWNLOAD, 1000, 1024) };
		memcpy(&desc_configuration[len], block, sizeof(block));
		len += sizeof(block);
		itf_dfu = itf++;
	}
	uint8_t const header[] = { TUD_CONFIG_DESCRIPTOR(1, itf, 0, len, 0, 100) }; /* bus-powered */
	memcpy(desc_configuration, header, sizeof(header));
	desc_ms_os_20[MS_OS_20_ITF_IDX] = itf_dfu;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
	(void)index;
	build_configuration();
	return desc_configuration;
}

static uint8_t const desc_bos[] = {
	TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
	TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
	return desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
	if (stage != CONTROL_STAGE_SETUP)
		return true;
	if ((request->bRequest == VENDOR_REQUEST_MICROSOFT) && (request->wIndex == 7)) {
		build_configuration();
		return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, sizeof(desc_ms_os_20));
	}
	return false;
}

static char const *const string_desc_arr[] = {
	[STRID_MANUFACTURER] = APP_USB_MANUFACTURER,
	[STRID_PRODUCT]      = APP_USB_PRODUCT,
	[STRID_CDC]          = "GPIB serial (Prologix)",
	[STRID_DFU]          = "Firmware update",
	[STRID_NCM]          = "Network (dev)",
};

static uint16_t desc_str[INSTR_ID_MAX + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
	(void)langid;
	size_t chr_count;

	if (index == STRID_SERIAL) { /* from *IDN? or the chip UID at start-up */
		char const *str = instr_id();
		chr_count = tu_min32(strlen(str), TU_ARRAY_SIZE(desc_str) - 1);
		for (size_t i = 0; i < chr_count; i++)
			desc_str[1 + i] = (uint8_t)str[i];
#if CFG_TUD_NCM
	} else if (index == STRID_MAC) { /* the host's end of the USB network */
		static const char hex[] = "0123456789ABCDEF";
		for (size_t i = 0; i < 6; i++) {
			desc_str[1 + 2 * i] = hex[tud_network_mac_address[i] >> 4];
			desc_str[2 + 2 * i] = hex[tud_network_mac_address[i] & 0xF];
		}
		chr_count = 12;
#endif
	} else if (index == STRID_LANGID) {
		desc_str[1] = 0x0409;
		chr_count = 1;
	} else if (index < TU_ARRAY_SIZE(string_desc_arr) && string_desc_arr[index]) {
		char const *str = string_desc_arr[index];
		chr_count = tu_min32(strlen(str), TU_ARRAY_SIZE(desc_str) - 1);
		for (size_t i = 0; i < chr_count; i++)
			desc_str[1 + i] = (uint8_t)str[i];
	} else {
		return NULL;
	}

	desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
	return desc_str;
}
