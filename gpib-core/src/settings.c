#include <stddef.h>
#include <string.h>
#include "settings.h"
#include "board.h"

/* Two flash pages (sectors) from _settings_start (linker script). Records are appended across both pages; the newest
 * valid one (highest sequence number, good CRC) wins. A page is erased only when the newest record is in the other. */
extern const uint8_t _settings_start[];

#define PAGE_SIZE      BOARD_FLASH_PAGE_SIZE
#define RECORD_SIZE    SETTINGS_RECORD_SIZE
#define SLOTS_PER_PAGE (PAGE_SIZE / RECORD_SIZE)
#define SLOTS          (2u * SLOTS_PER_PAGE)
#define MAGIC          0x5347u /* "GS" */
#define VERSION        2u      /* bump only for incompatible layouts; appended fields need no bump */
#define SETTLE_MS      200u
#define WRITE_GAP_MS   60000u /* between automatic writes: a script changing settings can't wear the pages out */

typedef struct {
	uint16_t magic;
	uint8_t  version;
	uint8_t  size; /* sizeof(settings_t) when written: newer fields missing from old records keep defaults */
	uint32_t seq;
	uint8_t  data[RECORD_SIZE - 12];
	uint32_t crc;
} record_t;

_Static_assert(sizeof(record_t) == RECORD_SIZE, "record layout");
_Static_assert(sizeof(settings_t) <= sizeof(((record_t *)0)->data), "settings outgrew a record");

static const settings_t s_defaults = {
	.autoid    = AUTOID_ON,
	.read_term = 0,
	.gpib_addr = SETTINGS_NO_ADDR,
	.px_addr   = SETTINGS_NO_ADDR,
	.px_eoi    = 1,
	.px_eos    = 0, /* CR+LF */
	.px_read_tmo_ms = 500,
};

settings_t settings;

static settings_t s_saved; /* what flash holds, so unchanged settings cost no write */
static int       s_slot;
static uint32_t  s_seq;
static bool      s_dirty;
static uint32_t  s_last_change;
static bool      s_written;     /* this boot */
static uint32_t  s_last_write;
static record_t  s_rec;

static const record_t *slot_ptr(unsigned slot)
{
	return (const record_t *)(const void *)(_settings_start + slot * RECORD_SIZE);
}

static uint32_t crc32(const void *buf, unsigned len)
{
	const uint8_t *p = buf;
	uint32_t c = 0xFFFFFFFFu;

	while (len--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
	}
	return ~c;
}

static bool record_valid(const record_t *r)
{
	return (r->magic == MAGIC) && (r->crc == crc32(r, offsetof(record_t, crc)));
}

static bool slot_erased(unsigned slot)
{
	const uint32_t *p = (const uint32_t *)slot_ptr(slot);

	for (unsigned i = 0; i < RECORD_SIZE / 4; i++)
		if (p[i] != BOARD_FLASH_ERASED)
			return false;
	return true;
}

/* v1 (USBGPIB-STM32 3.x) cached the USB serial string descriptor (length, type 3, UTF-16LE) where id is now */
static void id_from_v1(void)
{
	uint8_t d[SETTINGS_ID_LEN];
	unsigned n = 0;

	memcpy(d, settings.id, sizeof(d));
	if ((d[0] >= 2u) && (d[0] <= sizeof(d)) && (d[1] == 3u))
		n = (d[0] - 2u) / 2u;
	for (unsigned i = 0; i < n; i++)
		settings.id[i] = (char)d[2u + 2u * i];
	settings.id[n] = '\0';
}

void settings_init(void)
{
	s_slot = -1;
	for (unsigned i = 0; i < SLOTS; i++) {
		const record_t *r = slot_ptr(i);
		if (record_valid(r) && ((s_slot < 0) || ((int32_t)(r->seq - s_seq) > 0))) {
			s_slot = (int)i;
			s_seq = r->seq;
		}
	}

	settings = s_defaults;
	if (s_slot >= 0) {
		const record_t *r = slot_ptr((unsigned)s_slot);
		if ((r->version == VERSION) || (r->version == 1)) {
			memcpy(&settings, r->data, (r->size < sizeof(settings)) ? r->size : sizeof(settings));
			if (r->version == 1)
				id_from_v1();
		}
	}
	s_saved = settings;
	s_dirty = false;
	s_written = false;
}

void settings_changed(void)
{
	s_dirty = true;
	s_last_change = board_millis();
}

void settings_flush(void)
{
	if (!s_dirty)
		return;
	s_dirty = false;
	if (memcmp(&settings, &s_saved, sizeof(settings)) == 0)
		return;

	unsigned next = (s_slot < 0) ? 0 : ((unsigned)s_slot + 1) % SLOTS;
	bool erase = false;
	if (!slot_erased(next)) {
		/* never erase the page holding the newest record: start over in the other one */
		unsigned page = next / SLOTS_PER_PAGE;
		if ((s_slot >= 0) && (page == (unsigned)s_slot / SLOTS_PER_PAGE))
			page ^= 1u;
		next = page * SLOTS_PER_PAGE;
		erase = true;
	}

	memset(&s_rec, 0, sizeof(s_rec));
	s_rec.magic = MAGIC;
	s_rec.version = VERSION;
	s_rec.size = sizeof(settings);
	s_rec.seq = s_seq + 1;
	memcpy(s_rec.data, &settings, sizeof(settings));
	s_rec.crc = crc32(&s_rec, offsetof(record_t, crc));

	if (erase)
		board_flash_erase(slot_ptr(next));
	board_flash_write(slot_ptr(next), &s_rec, sizeof(s_rec));
	s_written = true;
	s_last_write = board_millis();

	if (record_valid(slot_ptr(next))) {
		s_slot = (int)next;
		s_seq = s_rec.seq;
		s_saved = settings;
	} else {
		s_dirty = true; /* retried after the write gap */
	}
}

void settings_task(void)
{
	uint32_t now = board_millis();

	if (s_dirty && ((now - s_last_change) >= SETTLE_MS) && (!s_written || ((now - s_last_write) >= WRITE_GAP_MS)))
		settings_flush();
}
