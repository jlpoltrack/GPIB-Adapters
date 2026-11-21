#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* USBGPIB-STM32 identity for gpib-core */

#define APP_PROLOGIX_VERSION "USBGPIB-STM32 version 3.0 (Prologix GPIB-USB compatible)"

/* WARNING: 1209:0001 is the pid.codes test PID, for private testing only. It is not unique and must not ship on
 * boards that are distributed: register a PID at pid.codes and put it here. */
#define APP_USB_VID          0x1209
#define APP_USB_PID          0x0001
#define APP_USB_BCD          0x0300 /* firmware v3.0 */
#define APP_USB_MANUFACTURER "USBGPIB-STM32"
#define APP_USB_PRODUCT      "USB GPIB adapter"

#endif /* APP_CONFIG_H_ */
