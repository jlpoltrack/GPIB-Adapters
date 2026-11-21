/* Host tests for the network settings command (netcfg.c), direct and through '!net' (instr.c) (make test) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netcfg.h"
#include "instr.h"
#include "settings.h"
#include "board.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t _settings_start[2 * BOARD_FLASH_PAGE_SIZE];
void board_flash_erase(const void *page) { memset((void *)page, 0xFF, BOARD_FLASH_PAGE_SIZE); }
void board_flash_write(const void *dst, const void *src, unsigned len) { memcpy((void *)dst, src, len); }
void board_get_uid_hex(char *buf) { strcpy(buf, "0123456789ABCDEF01234567"); }
void board_led_mode(led_mode_t mode) { (void)mode; }
void board_enter_bootloader(void) { abort(); } /* not reached */

static bool s_has_net = true;
bool board_net_status(netcfg_status_t *st)
{
	static const uint8_t now[4] = { 192, 168, 1, 23 };
	memcpy(st->ip, now, 4);
	st->hostname = settings.net_name[0] ? settings.net_name : "langpib-1a2b";
	return s_has_net;
}

/* --- helpers ------------------------------------------------------------------------------------------ */

static void setup(void)
{
	memset(_settings_start, 0xFF, sizeof(_settings_start));
	settings_init();
	s_has_net = true;
}

static unsigned cmd_from(const char *line, char *resp, unsigned max, bool usb)
{
	char buf[128], *argv[8];
	int argc = 0;
	strcpy(buf, line);
	for (char *p = strtok(buf, " "); p && (argc < 8); p = strtok(NULL, " "))
		argv[argc++] = p;
	return netcfg_command(argc, argv, resp, max, usb);
}

static unsigned cmd(const char *line, char *resp, unsigned max)
{
	return cmd_from(line, resp, max, true);
}

static bool query_is(const char *want)
{
	char r[96];
	unsigned n = cmd("net", r, sizeof(r));
	bool ok = (n == strlen(want)) && !strcmp(r, want);
	if (!ok)
		fprintf(stderr, "net: \"%s\", expected \"%s\"\n", r, want);
	return ok;
}

/* --- tests -------------------------------------------------------------------------------------------- */

static void test_parse(void)
{
	uint8_t ip[4];
	assert(netcfg_parse_ip("192.168.1.50", ip) && ip[0] == 192 && ip[3] == 50);
	assert(netcfg_parse_ip("0.0.0.0", ip));
	assert(!netcfg_parse_ip("192.168.1", ip) && !netcfg_parse_ip("192.168.1.256", ip));
	assert(!netcfg_parse_ip("192.168.1.5x", ip) && !netcfg_parse_ip("1..2.3", ip) && !netcfg_parse_ip("", ip));
	assert(!netcfg_parse_ip("0192.168.1.1", ip));
	assert(netcfg_name_valid("bench-dmm") && netcfg_name_valid("A1"));
	assert(!netcfg_name_valid("-x") && !netcfg_name_valid("x-") && !netcfg_name_valid("a.b") && !netcfg_name_valid(""));
	assert(!netcfg_name_valid("abcdefghijklmnopqrstuvwx")); /* 24 */
}

static void test_default_dhcp(void)
{
	setup();
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update off"));
}

static void test_static_and_back(void)
{
	char r[96];
	setup();
	assert(cmd("net static 10.0.0.5 255.255.255.0 10.0.0.1", r, sizeof(r)) == 0);
	assert(query_is("static 10.0.0.5 255.255.255.0 10.0.0.1; ip 192.168.1.23; name langpib-1a2b; update off"));
	settings_init(); /* flushed at once: survives a reset */
	assert(settings.net_mode == NET_MODE_STATIC && settings.net_ip[3] == 5 && settings.net_gw[3] == 1);
	cmd("net static 10.0.0.6 255.255.0.0", r, sizeof(r)); /* no gateway */
	assert(query_is("static 10.0.0.6 255.255.0.0 0.0.0.0; ip 192.168.1.23; name langpib-1a2b; update off"));
	cmd("net dhcp", r, sizeof(r));
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update off"));
}

static void test_bad_static_ignored(void)
{
	char r[96];
	setup();
	cmd("net static 10.0.0.5 255.0.255.0", r, sizeof(r));     /* holes in the mask */
	cmd("net static 10.0.0.0 255.255.255.0", r, sizeof(r));   /* network address */
	cmd("net static 10.0.0.255 255.255.255.0", r, sizeof(r)); /* broadcast */
	cmd("net static 127.0.0.1 255.0.0.0", r, sizeof(r));
	cmd("net static 224.0.0.5 255.255.255.0", r, sizeof(r));
	cmd("net static 10.0.0.5", r, sizeof(r));
	cmd("net static 10.0.0.5 255.255.255.0 10.0.0.1 extra", r, sizeof(r));
	cmd("net bogus", r, sizeof(r));
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update off"));
}

static void test_name(void)
{
	char r[96];
	setup();
	cmd("net name bench-dmm", r, sizeof(r));
	assert(query_is("dhcp; ip 192.168.1.23; name bench-dmm; update off"));
	cmd("net name bad.name", r, sizeof(r));
	assert(query_is("dhcp; ip 192.168.1.23; name bench-dmm; update off"));
	cmd("net name default", r, sizeof(r));
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update off"));
}

static void test_no_network(void)
{
	setup();
	s_has_net = false;
	assert(query_is("no network"));
}

static void test_short_buffer(void)
{
	char r[8];
	setup();
	assert(cmd("net", r, sizeof(r)) == 7 && !strcmp(r, "dhcp; i"));
}

static void test_via_instr_command(void)
{
	uint8_t r[64];
	setup();
	const char *set = "net static 10.1.2.3 255.255.255.0 10.1.2.1\n";
	assert(instr_command((const uint8_t *)set, (uint16_t)strlen(set), r, sizeof(r)) == 0);
	assert(settings.net_mode == NET_MODE_STATIC && settings.net_ip[2] == 2);
	uint8_t n = instr_command((const uint8_t *)"net\r\n", 5, r, sizeof(r));
	assert(n && !memcmp(r, "static 10.1.2.3", 15));
}

static void test_password(void)
{
	char r[96];
	setup();
	assert(!netcfg_password_set() && !netcfg_password_ok("") && !netcfg_password_ok("x"));
	cmd_from("net password s3cret!", r, sizeof(r), false); /* not from the network */
	assert(!netcfg_password_set());
	cmd("net password s3cret!", r, sizeof(r));
	assert(netcfg_password_set() && netcfg_password_ok("s3cret!"));
	assert(!netcfg_password_ok("s3cret") && !netcfg_password_ok("s3cret!!") && !netcfg_password_ok(""));
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update on")); /* never the password itself */
	settings_init();
	assert(netcfg_password_ok("s3cret!")); /* in flash */

	cmd("net password 0123456789abcdef0123456789abcdefX", r, sizeof(r)); /* 33: too long */
	cmd("net password", r, sizeof(r));
	assert(netcfg_password_ok("s3cret!"));
	char ctl[] = "net password a\x01b";
	cmd(ctl, r, sizeof(r));
	assert(netcfg_password_ok("s3cret!"));
	cmd("net password 0123456789abcdef0123456789abcdef", r, sizeof(r)); /* 32 */
	assert(netcfg_password_ok("0123456789abcdef0123456789abcdef"));
	assert(!netcfg_password_ok("0123456789abcdef0123456789abcdef0"));
	cmd_from("net password off", r, sizeof(r), false);
	assert(netcfg_password_set());
	cmd("net password off", r, sizeof(r));
	assert(!netcfg_password_set() && !netcfg_password_ok(""));
	assert(query_is("dhcp; ip 192.168.1.23; name langpib-1a2b; update off"));
}

static void test_password_via_instr_command(void)
{
	uint8_t r[8];
	setup();
	const char *set = "net password hunter2\n";
	instr_command((const uint8_t *)set, (uint16_t)strlen(set), r, sizeof(r));
	assert(netcfg_password_ok("hunter2"));
}

int main(void)
{
	test_parse();
	test_default_dhcp();
	test_static_and_back();
	test_bad_static_ignored();
	test_name();
	test_no_network();
	test_short_buffer();
	test_via_instr_command();
	test_password();
	test_password_via_instr_command();
	puts("netcfg: all tests passed");
	return 0;
}
