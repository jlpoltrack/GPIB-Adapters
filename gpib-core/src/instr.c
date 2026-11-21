/* Instrument core, split out of xyphro UsbGpib's TestAndMeasurement.c (MIT License, Copyright (c) 2019 Kai Gossner)
 * via the USBGPIB-STM32 port: start-up, auto-ID and the '!' commands, now independent of USB. */

#include <string.h>
#include "instr.h"
#include "netcfg.h"
#include "gpib_bus.h"
#include "miniparser.h"
#include "settings.h"
#include "board.h"

_Static_assert(INSTR_ID_MAX < SETTINGS_ID_LEN, "settings.id holds the ID string and its NUL");

uint8_t gpib_addr = 1;

static char    s_id[INSTR_ID_MAX + 1];
static char    s_term;
static uint8_t s_idbuf[100];

static uint8_t *s_resp;
static uint8_t  s_resp_max, s_resp_len;

char instr_get_readterm(void)
{
	return s_term;
}

void instr_set_readterm(char term)
{
	s_term = term;
}

const char *instr_id(void)
{
	return s_id;
}

/* The AVR used its 80-bit signature row here; the STM32 uses its 96-bit unique ID */
static void set_internal_id(bool add_gpib_addr)
{
	char uid[BOARD_SERIAL_LEN + 1];
	uint8_t len = 0;

	if (add_gpib_addr) {
		memcpy(s_id, "GPIB_", 5);
		len = 5;
		s_id[len++] = (char)('0' + (gpib_addr / 10));
		s_id[len++] = (char)('0' + (gpib_addr % 10));
		s_id[len++] = '_';
	}
	board_get_uid_hex(uid);
	memcpy(&s_id[len], uid, BOARD_SERIAL_LEN);
	s_id[len + BOARD_SERIAL_LEN] = '\0';
}

void instr_init(void)
{
	s_term = (char)settings.read_term;
	set_internal_id(false);
}

/* returns true only if a GPIB device responds to any GPIB address */
static bool find_device(void)
{
	uint8_t addr = gpib_find_device();

	gpib_addr = (addr != GPIB_NO_ADDR) ? addr : 1; /* fallback to GPIB address 1, if no device was found */
	return addr != GPIB_NO_ADDR;
}

/* Sends query, reads the answer into s_idbuf. Returns -1 if the query could not be sent, else the answer length. */
static int query_id(const char *query, uint16_t len, bool stop_at_lf)
{
	gpib_job_t j = { .op = GPIB_OP_WRITE, .addr = gpib_addr, .flags = GPIB_EOI | GPIB_END,
	                 .buf = (uint8_t *)(uintptr_t)query, .len = len, .timeout_ms = 1000 };
	if (gpib_run(&j, GPIB_OWNER_TMC) != GPIB_OK)
		return -1;

	j = (gpib_job_t){ .op = GPIB_OP_READ, .addr = gpib_addr, .flags = stop_at_lf ? GPIB_TERM : 0, .term = '\n',
	                  .buf = s_idbuf, .len = sizeof(s_idbuf), .timeout_ms = 1000 };
	gpib_status_t st = gpib_run(&j, GPIB_OWNER_TMC);
	if ((st == GPIB_OK) && !j.end) { /* longer than s_idbuf */
		gpib_job_t u = { .op = GPIB_OP_UNADDRESS, .timeout_ms = 1000 };
		gpib_run(&u, GPIB_OWNER_TMC);
	}
	return (st == GPIB_OK) ? j.done : 0;
}

/* Turns an ID answer into the ID string: USBTMC-unsafe characters become '_' */
static uint8_t id_to_string(uint16_t n, bool idn, bool *phascomma)
{
	uint8_t len = 0;
	char prev_c = '\0';
	bool suppress_character = false;

	for (uint16_t i = 0; (i < n) && (len < INSTR_ID_MAX); i++) {
		char c = (char)s_idbuf[i];
		if (!idn && ((c == '\r') || (c == '\n')))
			break;
		*phascomma = *phascomma || (c == ',');
		/* a comma and ampersand are allowed by USBTMC, but R&S software does not like them */
		if ((c == '\"') || (c == '*') || (c == '/') || (c == '\\') || (c == ':') || (c == '?') || (c == ' ') ||
		    (c == ',') || (c == '&') || (idn && (c == '_'))) {
			suppress_character = idn && (prev_c == '_'); /* Keysight VISA rejects double underscores */
			c = '_';
		}
		if ((c >= 32) && (c <= 126) && !suppress_character)
			s_id[len++] = c;
		suppress_character = false;
		prev_c = c;
	}
	/* strip trailing underscores (spaces) */
	while ((len > 1) && (s_id[len - 1] == '_'))
		len--;
	s_id[len] = '\0';
	return len;
}

/* returns true if an ID string was received over GPIB */
static bool identify_device(void)
{
	static const char idn[] = "*IDN?\n", id[] = "ID?\n";
	bool hascomma = false;
	uint8_t len = 0;

	s_id[0] = '\0';

	int n = query_id(idn, sizeof(idn) - 1, false);
	if (n >= 0) {
		len = id_to_string((uint16_t)n, true, &hascomma);
		if (len == 0) { /* no response to *IDN?, so try ID? */
			n = query_id(id, sizeof(id) - 1, true);
			if (n > 0)
				len = id_to_string((uint16_t)n, false, &hascomma);
		}
	}

	if (!hascomma && (s_id[0] == 'H') && (s_id[1] == 'P') && (s_id[2] >= '0') && (s_id[2] <= '9'))
		hascomma = true;

	bool got_id = (len > 0) && hascomma;
	if (!got_id) /* no answer => build an ID from the GPIB address */
		set_internal_id(true);

	gpib_ren(false);
	board_delay_ms(100);
	gpib_ren(true);
	return got_id;
}

static void wait_ms(uint32_t ms, void (*idle)(void))
{
	for (uint32_t i = 0; i < ms / 10u; i++) {
		board_delay_ms(10);
		idle();
	}
}

bool instr_startup(void (*idle)(void))
{
	board_led_mode(LED_NO_BUS);
	while (!gpib_connected())
		wait_ms(10, idle);
	board_led_mode(LED_ON);

	uint8_t autoid = settings.autoid;
	if ((autoid >= AUTOID_SLOW) && (autoid <= AUTOID_SLOWEST)) /* slow instruments: wait before asking *IDN? */
		wait_ms((autoid == AUTOID_SLOWEST) ? 30000u : (autoid == AUTOID_SLOWER) ? 15000u : 5000u, idle);

	/* the bus is there: find a responsive GPIB address */
	while (!find_device()) {
		board_led_mode(LED_NO_DEVICE);
		wait_ms(1100, idle);
		if (!gpib_connected()) {
			board_led_mode(LED_OFF);
			return false;
		}
	}
	board_led_mode(LED_ON);

	if (autoid != AUTOID_OFF) {
		uint8_t prevaddr = settings.gpib_addr;
		if (identify_device()) { /* got an ID string: cache it with its address */
			settings.gpib_addr = gpib_addr;
			memset(settings.id, 0, sizeof(settings.id));
			memcpy(settings.id, s_id, strlen(s_id));
			settings_changed();
		} else if ((prevaddr == gpib_addr) && settings.id[0]) { /* same address as last time: reuse the cache */
			memcpy(s_id, settings.id, INSTR_ID_MAX);
			s_id[INSTR_ID_MAX] = '\0';
		}
	} else { /* auto-ID off */
		set_internal_id(true);
		gpib_ren(false);
		board_delay_ms(100);
		gpib_ren(true);
	}

	/* Matlab does not allow resource strings longer than this */
	if (settings.idstring_short && (strlen(s_id) > 20))
		s_id[20] = '\0';
	return true;
}

void set_internal_response(uint8_t *pdat, uint8_t len)
{
	if (s_resp && (len <= s_resp_max)) {
		memcpy(s_resp, pdat, len);
		s_resp_len = len;
	}
}

/* "net ...": netcfg.c, words split on spaces */
static bool net_command(const uint8_t *cmd, uint16_t len, uint8_t *resp, uint8_t max)
{
	char line[96], *argv[8];
	int argc = 0;
	uint16_t n = 0;

	for (uint16_t i = 0; (i < len) && (n < sizeof(line) - 1); i++)
		if ((cmd[i] != '\r') && (cmd[i] != '\n'))
			line[n++] = (char)cmd[i];
	line[n] = '\0';
	for (char *p = line; *p && (argc < 8);) {
		while (*p == ' ')
			*p++ = '\0';
		if (*p)
			argv[argc++] = p;
		while (*p && (*p != ' '))
			p++;
	}
	if (!argc || strcmp(argv[0], "net"))
		return false;
	s_resp_len = (uint8_t)netcfg_command(argc, argv, (char *)resp, max, true); /* only USBTMC calls instr_command() */
	return true;
}

/* Syntax: see miniparser.c, plus "net" (netcfg.h); the legacy form !XXYY sets auto-ID (XX = 00) or the read
 * termination (XX = 01) */
uint8_t instr_command(const uint8_t *cmd, uint16_t len, uint8_t *resp, uint8_t max)
{
	bool cmd_executed = false;

	s_resp = resp;
	s_resp_max = max;
	s_resp_len = 0;

	if (net_command(cmd, len, resp, max)) {
		s_resp = NULL;
		return s_resp_len;
	}
	parser_reset();
	for (uint16_t i = 0; (i < len) && !cmd_executed; i++)
		if ((cmd[i] != '\r') && (cmd[i] != '\n')) /* drop termination characters */
			cmd_executed = parser_add(cmd[i]);
	parser_add('_'); /* lets "autoid slow" match without also matching slower/slowest */
	parser_finalize(); /* dispatches the addr command */

	s_resp = NULL;
	return s_resp_len;
}
