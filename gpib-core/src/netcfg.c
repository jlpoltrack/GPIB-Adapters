#include <string.h>
#include "netcfg.h"
#include "settings.h"

__attribute__((weak)) bool board_net_status(netcfg_status_t *st)
{
	(void)st;
	return false;
}

bool netcfg_parse_ip(const char *s, uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		unsigned v = 0, digits = 0;
		for (; (*s >= '0') && (*s <= '9') && (digits < 4); s++, digits++)
			v = v * 10u + (unsigned)(*s - '0');
		if (!digits || (digits > 3) || (v > 255) || (*s != ((i < 3) ? '.' : '\0')))
			return false;
		ip[i] = (uint8_t)v;
		if (i < 3)
			s++;
	}
	return true;
}

bool netcfg_name_valid(const char *s)
{
	size_t n = strlen(s);

	if (!n || (n > NETCFG_NAME_MAX) || (s[0] == '-') || (s[n - 1] == '-'))
		return false;
	for (; *s; s++)
		if (!(((*s >= 'a') && (*s <= 'z')) || ((*s >= 'A') && (*s <= 'Z')) || ((*s >= '0') && (*s <= '9')) ||
		      (*s == '-')))
			return false;
	return true;
}

static bool pw_valid(const char *s)
{
	size_t n = strlen(s);

	if (!n || (n > NETCFG_PW_MAX) || !strcmp(s, "off"))
		return false;
	for (; *s; s++)
		if ((*s < 0x21) || (*s > 0x7E))
			return false;
	return true;
}

bool netcfg_password_set(void)
{
	return settings.update_pw[0] != '\0';
}

/* the whole buffer every time, so the time taken says nothing about how much matched */
bool netcfg_password_ok(const char *s)
{
	char b[sizeof(settings.update_pw)] = { 0 };
	size_t n = strlen(s);
	unsigned d = (n > NETCFG_PW_MAX) || !netcfg_password_set();

	memcpy(b, s, (n > NETCFG_PW_MAX) ? NETCFG_PW_MAX : n);
	for (size_t i = 0; i < sizeof(b); i++)
		d |= (unsigned char)(b[i] ^ settings.update_pw[i]);
	return d == 0;
}

/* contiguous ones from the top, not all zero */
static bool mask_valid(const uint8_t m[4])
{
	uint32_t v = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
	return v && !((~v + 1u) & ~v);
}

static bool host_valid(const uint8_t ip[4], const uint8_t m[4])
{
	uint32_t v = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
	uint32_t mk = ((uint32_t)m[0] << 24) | ((uint32_t)m[1] << 16) | ((uint32_t)m[2] << 8) | m[3];
	return (v & ~mk) && ((v & ~mk) != ~mk) && (ip[0] != 127) && (ip[0] < 224);
}

typedef struct {
	char    *p;
	unsigned left;
} out_t;

static void put(out_t *o, const char *s)
{
	for (; *s && (o->left > 1); o->left--)
		*o->p++ = *s++;
}

static void put_ip(out_t *o, const uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		char b[5], *q = &b[4];
		unsigned v = ip[i];
		*q = '\0';
		do {
			*--q = (char)('0' + v % 10u);
			v /= 10u;
		} while (v);
		if (i)
			put(o, ".");
		put(o, q);
	}
}

unsigned netcfg_command(int argc, char **argv, char *resp, unsigned max, bool usb)
{
	uint8_t ip[4], mask[4], gw[4] = { 0 };

	if (argc == 1) {
		out_t o = { resp, max };
		netcfg_status_t st = { 0 };
		if (!max)
			return 0;
		if (!board_net_status(&st)) {
			put(&o, "no network");
		} else {
			if (settings.net_mode == NET_MODE_STATIC) {
				put(&o, "static ");
				put_ip(&o, settings.net_ip);
				put(&o, " ");
				put_ip(&o, settings.net_mask);
				put(&o, " ");
				put_ip(&o, settings.net_gw);
			} else {
				put(&o, "dhcp");
			}
			put(&o, "; ip ");
			put_ip(&o, st.ip);
			put(&o, "; name ");
			put(&o, st.hostname ? st.hostname : "");
			put(&o, netcfg_password_set() ? "; update on" : "; update off");
		}
		*o.p = '\0';
		return (unsigned)(o.p - resp);
	}

	if (!strcmp(argv[1], "dhcp") && (argc == 2)) {
		settings.net_mode = NET_MODE_DHCP;
	} else if (!strcmp(argv[1], "static") && ((argc == 4) || (argc == 5))) {
		if (!netcfg_parse_ip(argv[2], ip) || !netcfg_parse_ip(argv[3], mask) || ((argc == 5) && !netcfg_parse_ip(argv[4], gw)))
			return 0;
		if (!mask_valid(mask) || !host_valid(ip, mask))
			return 0;
		settings.net_mode = NET_MODE_STATIC;
		memcpy(settings.net_ip, ip, 4);
		memcpy(settings.net_mask, mask, 4);
		memcpy(settings.net_gw, gw, 4);
	} else if (!strcmp(argv[1], "name") && (argc == 3)) {
		if (!strcmp(argv[2], "default"))
			settings.net_name[0] = '\0';
		else if (netcfg_name_valid(argv[2]))
			strcpy(settings.net_name, argv[2]);
		else
			return 0;
	} else if (!strcmp(argv[1], "password") && (argc == 3) && usb) {
		if (!strcmp(argv[2], "off"))
			memset(settings.update_pw, 0, sizeof(settings.update_pw));
		else if (pw_valid(argv[2]))
			strcpy(settings.update_pw, argv[2]);
		else
			return 0;
	} else {
		return 0;
	}
	settings_changed();
	settings_flush(); /* the usual next step is a reset */
	return 0;
}
