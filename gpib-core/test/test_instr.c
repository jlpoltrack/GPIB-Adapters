/* Host tests for the instrument core: start-up auto-ID and the '!' commands, on simulated instruments (make test) */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "instr.h"
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "settings.h"
#include "board.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t _settings_start[2 * BOARD_FLASH_PAGE_SIZE];
static int s_bootloader;
static jmp_buf s_noreturn; /* board_enter_bootloader() doesn't return */
static led_mode_t s_led;

void board_flash_erase(const void *page) { memset((void *)page, 0xFF, BOARD_FLASH_PAGE_SIZE); }
void board_flash_write(const void *dst, const void *src, unsigned len) { memcpy((void *)dst, src, len); }
void board_get_uid_hex(char *buf) { strcpy(buf, "0123456789ABCDEF01234567"); }
void board_led_mode(led_mode_t mode) { s_led = mode; }
void board_enter_bootloader(void) { s_bootloader++; longjmp(s_noreturn, 1); }

static int s_idle_calls, s_unplug_after = -1, s_power_on_after = -1;

static void idle(void)
{
	s_idle_calls++;
	if (s_idle_calls == s_unplug_after)
		sim_powered = false;
	if (s_idle_calls == s_power_on_after)
		sim_powered = true;
}

/* --- helpers ------------------------------------------------------------------------------------------ */

static void setup(bool keep_settings)
{
	sim_reset();
	if (!keep_settings)
		memset(_settings_start, 0xFF, sizeof(_settings_start));
	settings_init();
	gpib_init();
	instr_init();
	s_idle_calls = 0;
	s_unplug_after = s_power_on_after = -1;
	s_bootloader = 0;
}

static void answer_idn(sim_dev_t *d)
{
	sim_answer(d, "ACME,M1,42,1.0\n", true);
}

static uint8_t cmd(const char *c, char *resp)
{
	uint8_t r[8];
	uint8_t n = instr_command((const uint8_t *)c, (uint16_t)strlen(c), r, sizeof(r));
	if (resp) {
		memcpy(resp, r, n);
		resp[n] = '\0';
	}
	return n;
}

/* --- tests -------------------------------------------------------------------------------------------- */

static void test_idn(void)
{
	setup(false);
	sim_dev_t *d = sim_add(7, -1);
	d->on_message = answer_idn;
	assert(instr_startup(idle));
	assert(gpib_addr == 7 && s_led == LED_ON);
	assert(!strcmp(instr_id(), "ACME_M1_42_1.0"));
	settings_flush();
	assert(settings.gpib_addr == 7 && !strcmp(settings.id, "ACME_M1_42_1.0"));
}

static void test_no_answer_uses_address_and_uid(void)
{
	setup(false);
	sim_add(7, -1); /* listens, never talks */
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "GPIB_07_0123456789ABCDEF01234567"));
}

static void test_cached_id_reused(void)
{
	setup(false);
	sim_dev_t *d = sim_add(7, -1);
	d->on_message = answer_idn;
	assert(instr_startup(idle));
	settings_flush();

	setup(true);
	sim_add(7, -1); /* same address, now silent */
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "ACME_M1_42_1.0"));

	setup(true);
	sim_add(8, -1); /* another address: the cache doesn't apply */
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "GPIB_08_0123456789ABCDEF01234567"));
}

static void test_autoid_off_and_short(void)
{
	setup(false);
	settings.autoid = AUTOID_OFF;
	sim_dev_t *d = sim_add(3, -1);
	d->on_message = answer_idn;
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "GPIB_03_0123456789ABCDEF01234567") && d->messages == 0);

	setup(false);
	settings.idstring_short = 1;
	d = sim_add(3, -1);
	d->on_message = answer_idn;
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "ACME_M1_42_1.0")); /* 14 characters: not cut */
	setup(false);
	settings.idstring_short = 1;
	sim_add(3, -1);
	assert(instr_startup(idle));
	assert(!strcmp(instr_id(), "GPIB_03_0123456789AB"));
}

static void test_bus_lost_while_searching(void)
{
	setup(false);
	s_unplug_after = 300; /* nothing answers, then the cable goes */
	assert(!instr_startup(idle));
	assert(s_led == LED_OFF);
}

static void test_waits_for_bus(void)
{
	setup(false);
	sim_powered = false;
	s_power_on_after = 50;
	sim_dev_t *d = sim_add(4, -1);
	d->on_message = answer_idn;
	assert(instr_startup(idle));
	assert(s_idle_calls >= 50 && gpib_addr == 4 && !strcmp(instr_id(), "ACME_M1_42_1.0"));
}

static void test_commands(void)
{
	char r[9];

	setup(false);
	assert(cmd("term lf\n", NULL) == 0 && instr_get_readterm() == '\n');
	assert(cmd("term?", r) == 2 && !strcmp(r, "lf"));
	assert(cmd("term cr", NULL) == 0 && instr_get_readterm() == '\r');
	assert(cmd("term store", NULL) == 0 && settings.read_term == '\r');
	assert(cmd("term eoi", NULL) == 0 && instr_get_readterm() == '\0');
	assert(cmd("term?", r) == 3 && !strcmp(r, "eoi"));

	assert(cmd("autoid?", r) == 2 && !strcmp(r, "on"));
	assert(cmd("autoid slow", NULL) == 0 && settings.autoid == AUTOID_SLOW);
	assert(cmd("autoid slowest", NULL) == 0 && settings.autoid == AUTOID_SLOWEST);
	assert(cmd("autoid?", r) == 7 && !strcmp(r, "slowest"));
	assert(cmd("0001", NULL) == 0 && settings.autoid == AUTOID_OFF);
	assert(cmd("0101", NULL) == 0 && settings.read_term == '\n' && instr_get_readterm() == '\n');

	assert(cmd("string short", NULL) == 0 && settings.idstring_short == 1);
	assert(cmd("string?", r) == 5 && !strcmp(r, "short"));
	assert(cmd("ver?", r) == 4 && !strcmp(r, "V2.4"));

	assert(cmd("addr 9", NULL) == 0 && gpib_addr == 9);
	assert(cmd("addr 9 2", NULL) == 0 && gpib_addr == (9 | (3 << 5)));
	assert(cmd("addr 40", NULL) == 0 && gpib_addr == (9 | (3 << 5))); /* out of range: unchanged */

	if (!setjmp(s_noreturn))
		cmd("reset", NULL);
	assert(s_bootloader == 1);
	assert(cmd("bogus", NULL) == 0);
}

int main(void)
{
	test_idn();
	test_no_answer_uses_address_and_uid();
	test_cached_id_reused();
	test_autoid_off_and_short();
	test_bus_lost_while_searching();
	test_waits_for_bus();
	test_commands();
	puts("instr: all tests passed");
	return 0;
}
