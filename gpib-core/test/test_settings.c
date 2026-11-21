/* Host tests for the settings flash log (make test); test/board_hw.h stands in for the board */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "settings.h"

#define FLASH_BYTES (2u * BOARD_FLASH_PAGE_SIZE)
#define PER_PAGE    (BOARD_FLASH_PAGE_SIZE / SETTINGS_RECORD_SIZE)

uint8_t _settings_start[FLASH_BYTES];

static uint32_t s_now;
static unsigned s_erases, s_writes;
static int s_write_budget = -1; /* bytes left before a simulated power loss; -1 = unlimited */

uint32_t board_millis(void) { return s_now; }

static void fill(uint8_t *p, uint32_t word, size_t len)
{
	for (size_t i = 0; i < len; i += 4)
		memcpy(p + i, &word, 4);
}

void board_flash_erase(const void *page)
{
	size_t off = (const uint8_t *)page - _settings_start;
	assert(off % BOARD_FLASH_PAGE_SIZE == 0 && off < FLASH_BYTES);
	fill(_settings_start + off, BOARD_FLASH_ERASED, BOARD_FLASH_PAGE_SIZE);
	s_erases++;
}

void board_flash_write(const void *dst, const void *src, unsigned len)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = src;
	assert(d >= _settings_start && d + len <= _settings_start + FLASH_BYTES && len % 4 == 0);
	for (unsigned i = 0; i < len; i++) {
		if (s_write_budget == 0)
			return;
		if (s_write_budget > 0)
			s_write_budget--;
		uint32_t erased = BOARD_FLASH_ERASED;
		assert(d[i] == ((const uint8_t *)&erased)[(d + i - _settings_start) % 4]); /* only erased flash */
		d[i] = s[i];
	}
	s_writes++;
}

static void reset_flash(uint32_t word)
{
	fill(_settings_start, word, sizeof(_settings_start));
	s_erases = s_writes = 0;
	s_write_budget = -1;
}

static void set_and_flush(uint8_t autoid)
{
	settings.autoid = autoid;
	settings_changed();
	settings_flush();
}

static void test_defaults_on_empty_flash(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	assert(settings.autoid == AUTOID_ON && settings.read_term == 0);
	assert(settings.gpib_addr == SETTINGS_NO_ADDR);
}

static void test_roundtrip(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	settings.read_term = '\n';
	settings.gpib_addr = 7;
	memcpy(settings.id, "serial", 6);
	settings_changed();
	settings_flush();
	memset(&settings, 0, sizeof(settings));
	settings_init();
	assert(settings.read_term == '\n' && settings.gpib_addr == 7);
	assert(memcmp(settings.id, "serial", 6) == 0);
}

static void test_settle_delay(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	s_now = 1000;
	settings.autoid = AUTOID_SLOW;
	settings_changed();
	s_now += 199;
	settings_task();
	assert(s_writes == 0);
	s_now += 1;
	settings_task();
	assert(s_writes == 1);

	/* the next automatic write waits out the gap; an explicit flush doesn't */
	settings.autoid = AUTOID_OFF;
	settings_changed();
	s_now += 1000;
	settings_task();
	assert(s_writes == 1);
	s_now = 1200 + 60000;
	settings_task();
	assert(s_writes == 2);
	set_and_flush(AUTOID_SLOWER);
	assert(s_writes == 3);
}

static void test_failed_write_is_retried(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	s_now = 5000;
	s_write_budget = 10; /* power dip mid-write: the read-back fails */
	set_and_flush(AUTOID_OFF);
	s_write_budget = -1;
	s_now += 60000;
	settings_task();
	settings_init();
	assert(settings.autoid == AUTOID_OFF);
}

static void test_unchanged_costs_nothing(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	set_and_flush(AUTOID_OFF);
	unsigned writes = s_writes;
	set_and_flush(AUTOID_OFF);
	assert(s_writes == writes);
}

static void test_garbage_is_ignored_and_replaced(void)
{
	reset_flash(0x5A5A5A5Au); /* old v2 bytes or stale app code */
	settings_init();
	assert(settings.autoid == AUTOID_ON);
	set_and_flush(AUTOID_SLOWER);
	settings_init();
	assert(settings.autoid == AUTOID_SLOWER);
}

static void test_wear_cycling(void)
{
	reset_flash(BOARD_FLASH_ERASED);
	settings_init();
	for (int i = 0; i < 100; i++) {
		set_and_flush((uint8_t)((i + 1) % 5)); /* never the default first */
		settings_init();
		assert(settings.autoid == (uint8_t)((i + 1) % 5));
	}
	assert(s_writes == 100);
	assert(s_erases <= 100 / PER_PAGE + 1); /* one erase per page of records */
}

static void test_torn_write_keeps_previous(void)
{
	/* one page of records: the torn one goes to fresh page 1; two pages: it first erases page 0 */
	for (unsigned fill = PER_PAGE; fill <= 2 * PER_PAGE; fill += PER_PAGE)
	for (int cut = 0; cut < SETTINGS_RECORD_SIZE; cut += 7) {
		reset_flash(BOARD_FLASH_ERASED);
		settings_init();
		for (unsigned i = 0; i < fill; i++) {
			settings.read_term = (uint8_t)i; /* make every record differ */
			set_and_flush(AUTOID_SLOWEST);
		}
		s_write_budget = cut;
		set_and_flush(AUTOID_OFF);
		s_write_budget = -1;
		settings_init();
		assert(settings.autoid == AUTOID_SLOWEST);
		set_and_flush(AUTOID_SLOW); /* and the log recovers */
		settings_init();
		assert(settings.autoid == AUTOID_SLOW);
	}
}

/* a record as USBGPIB-STM32 3.x (settings v1) wrote it: the ID slot holds a USB string descriptor */
static void test_v1_id_migrates(void)
{
	struct {
		uint16_t magic;
		uint8_t  version, size;
		uint32_t seq;
		uint8_t  data[SETTINGS_RECORD_SIZE - 12];
		uint32_t crc;
	} r = { .magic = 0x5347u, .version = 1, .size = sizeof(settings_t), .seq = 7 };
	settings_t v1 = { .autoid = AUTOID_SLOWER, .gpib_addr = 9, .px_eos = 2, .usb_mode = USB_MODE_CDC };
	const char *id = "HP3478A";
	uint8_t *d = (uint8_t *)v1.id;

	d[0] = (uint8_t)(2 + 2 * strlen(id));
	d[1] = 3;
	for (unsigned i = 0; i < strlen(id); i++)
		d[2 + 2 * i] = (uint8_t)id[i];
	memcpy(r.data, &v1, sizeof(v1));
	uint32_t c = 0xFFFFFFFFu;
	for (unsigned i = 0; i < offsetof(__typeof__(r), crc); i++) {
		c ^= ((const uint8_t *)&r)[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
	}
	r.crc = ~c;

	reset_flash(BOARD_FLASH_ERASED);
	memcpy(_settings_start, &r, sizeof(r));
	settings_init();
	assert(!strcmp(settings.id, "HP3478A"));
	assert(settings.autoid == AUTOID_SLOWER && settings.gpib_addr == 9 && settings.px_eos == 2);
	assert(settings.usb_mode == USB_MODE_CDC);
}

int main(void)
{
	test_v1_id_migrates();
	test_defaults_on_empty_flash();
	test_roundtrip();
	test_settle_delay();
	test_failed_write_is_retried();
	test_unchanged_costs_nothing();
	test_garbage_is_ignored_and_replaced();
	test_wear_cycling();
	test_torn_write_keeps_previous();
	puts("settings: all tests passed");
	return 0;
}
