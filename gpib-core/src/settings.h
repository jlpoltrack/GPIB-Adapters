#ifndef SETTINGS_H_
#define SETTINGS_H_

#include <stdint.h>
#include <stdbool.h>

/* Persistent settings, kept as CRC-checked records in a two-page flash log.
 * Edit the fields of `settings`, then call settings_changed(). */

enum { AUTOID_ON, AUTOID_OFF, AUTOID_SLOW, AUTOID_SLOWER, AUTOID_SLOWEST };
enum { USB_MODE_BOTH, USB_MODE_TMC, USB_MODE_CDC }; /* interfaces besides DFU runtime */
enum { NET_MODE_DHCP, NET_MODE_STATIC };              /* DHCP falls back to AutoIP (169.254.x.x) */

#define SETTINGS_ID_LEN      102 /* cached instrument ID string; the size of v1's USB descriptor, so fields stay put */
#define SETTINGS_NO_ADDR     0xFF
#define SETTINGS_RECORD_SIZE 256 /* flash log record: room for 244 bytes of settings */

typedef struct {
	uint8_t autoid;         /* AUTOID_* */
	uint8_t read_term;      /* '\n', '\r', or 0 for EOI only */
	uint8_t idstring_short; /* cut the serial string to 20 characters (for Matlab) */
	uint8_t gpib_addr;      /* address the cached ID string belongs to */
	char    id[SETTINGS_ID_LEN];
	/* Prologix port (++savecfg) */
	uint8_t  px_addr;        /* SETTINGS_NO_ADDR: the instrument autoid found */
	uint8_t  px_auto, px_eoi, px_eos, px_eot_enable, px_eot_char;
	uint16_t px_read_tmo_ms;
	uint8_t  usb_mode;       /* USB_MODE_* */
	/* network, on boards with Ethernet (netcfg.c): read at start-up */
	uint8_t  net_mode;       /* NET_MODE_* */
	uint8_t  net_ip[4], net_mask[4], net_gw[4]; /* NET_MODE_STATIC */
	char     net_name[24];   /* hostname and mDNS name; empty: the board's own (langpib-XXXX) */
	char     update_pw[33];  /* firmware update over the network (http.c); empty: open. Set from USB only */
} settings_t;

extern settings_t settings;

void settings_init(void);
void settings_changed(void); /* schedules a write once changes have settled */
void settings_task(void);
void settings_flush(void);   /* writes pending changes now */

#endif /* SETTINGS_H_ */
