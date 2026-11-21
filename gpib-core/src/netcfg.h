#ifndef NETCFG_H_
#define NETCFG_H_

/* The network settings command, shared by '!net' (instr.c: USBTMC) and '++net' (Prologix: CDC, TCP 1234):
 *   net                                      query: "dhcp|static <ip> <mask> <gw>; ip <now>; name <host>; update on|off"
 *   net dhcp                                 DHCP, then AutoIP (the default)
 *   net static <ip> <mask> [<gateway>]       fixed address
 *   net name <host> | net name default       hostname (DHCP option 12, mDNS <host>.local)
 *   net password <pw> | net password off    firmware update password (http.c POST /update); from USB only
 * The web page (http.c) sets the same through this command, except the password.
 * Settings go to flash at once and take effect at the next reset (++rst, !reset, power). Errors are ignored,
 * as other setters', and show in the query. */

#include <stdbool.h>
#include <stdint.h>

#define NETCFG_NAME_MAX 23
#define NETCFG_PW_MAX   32 /* printable ASCII, no spaces */

/* argv[0] is "net"; reply length. usb: the command came over USB, the only place the password can be set. */
unsigned netcfg_command(int argc, char **argv, char *resp, unsigned max, bool usb);
bool     netcfg_name_valid(const char *s);  /* letters, digits, '-' inside, 1..NETCFG_NAME_MAX */
bool     netcfg_parse_ip(const char *s, uint8_t ip[4]);
bool     netcfg_password_set(void);         /* network updates are on */
bool     netcfg_password_ok(const char *s); /* matches the set password (never when none is set) */

typedef struct {
	uint8_t     ip[4], mask[4], gw[4]; /* all 0 until it has an address */
	uint8_t     mac[6];
	const char *hostname;
	bool        dhcp; /* the address comes from DHCP or AutoIP (the mode it started with) */
} netcfg_status_t;

/* From the board's network code; the weak default says there is no network */
bool board_net_status(netcfg_status_t *st);

#endif /* NETCFG_H_ */
