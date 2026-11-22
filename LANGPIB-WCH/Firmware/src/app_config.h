#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* LANGPIB-WCH identity for gpib-core */

#define APP_PROLOGIX_VERSION "LANGPIB-WCH version 0.2 (Prologix GPIB-USB compatible)"

/* WARNING: 1209:0001 is the pid.codes test PID, for private testing only. It is not unique and must not ship on
 * boards that are distributed: register a PID at pid.codes and put it here. */
#define APP_USB_VID          0x1209
#define APP_USB_PID          0x0001
#define APP_USB_BCD          0x0020 /* firmware v0.2 */
#define APP_USB_MANUFACTURER "LANGPIB-WCH"
#ifdef GPIB_SIM
#define APP_USB_PRODUCT      "LAN GPIB adapter (simulated GPIB)"
#else
#define APP_USB_PRODUCT      "LAN GPIB adapter"
#endif

#endif /* APP_CONFIG_H_ */
