#include <string.h>
#include "stage.h"
#include "flash.h"

static int fail(stage_writer_t *w, int err)
{
	if (w->err == STAGE_OK)
		w->err = err;
	return w->err;
}

int stage_begin(stage_writer_t *w, uint32_t total)
{
	memset(w, 0, sizeof(*w));
	w->total = total;
	w->erased_to = STAGE_ADDR;
	if (total > FLASH_PAGE + STAGE_MAX_LEN + DFU_SUFFIX_LEN)
		return fail(w, STAGE_TOO_LARGE);
	if (total <= FLASH_PAGE + DFU_SUFFIX_LEN)
		return fail(w, STAGE_BAD_IMAGE);
	return STAGE_OK;
}

/* the header page is in: checked before anything is erased */
static int header_check(stage_writer_t *w)
{
	const app_header_t *h = &w->hdr;

	if ((h->magic != APP_MAGIC) || (h->hdr_version != APP_HDR_VER) || (h->length == 0))
		return fail(w, STAGE_BAD_IMAGE);
	if (h->length > STAGE_MAX_LEN)
		return fail(w, STAGE_TOO_LARGE);
	if (w->total != FLASH_PAGE + h->length + DFU_SUFFIX_LEN)
		return fail(w, STAGE_BAD_IMAGE);
	return STAGE_OK;
}

/* stage pages come in order, so each new sector is erased as the first page reaches it */
static int page_out(stage_writer_t *w, uint32_t addr)
{
	bool ok = (addr >= STAGE_ADDR) && (addr < w->erased_to + FLASH_SECTOR);

	flash_unlock();
	if (ok && (addr >= w->erased_to)) {
		ok = flash_erase_sector(w->erased_to);
		w->erased_to += FLASH_SECTOR;
	}
	ok = ok && flash_program_page(addr, w->page);
	flash_lock();
	return ok ? STAGE_OK : fail(w, STAGE_FLASH);
}

int stage_write(stage_writer_t *w, const uint8_t *buf, uint32_t n)
{
	if (w->err)
		return w->err;
	if (n > w->total - w->pos)
		return fail(w, STAGE_BAD_IMAGE);
	uint32_t crc_end = w->total - 4u; /* the suffix's dwCRC isn't in its own CRC */
	if (w->pos < crc_end)
		w->crc = image_crc32(w->crc, buf, (n < crc_end - w->pos) ? n : crc_end - w->pos);

	for (uint32_t i = 0; i < n; i++) {
		uint32_t pos = w->pos++, img_end = FLASH_PAGE + w->hdr.length;
		if (pos < FLASH_PAGE) {
			((uint8_t *)&w->hdr)[pos] = buf[i];
			if ((pos == FLASH_PAGE - 1u) && header_check(w))
				return w->err;
		} else if (pos < img_end) {
			uint8_t *pg = (uint8_t *)w->page;
			pg[pos % FLASH_PAGE] = buf[i];
			if (((pos % FLASH_PAGE) == FLASH_PAGE - 1u) || (pos == img_end - 1u)) {
				memset(&pg[pos % FLASH_PAGE + 1u], 0xFF, FLASH_PAGE - 1u - pos % FLASH_PAGE);
				if (page_out(w, STAGE_ADDR + (pos & ~(FLASH_PAGE - 1u))))
					return w->err;
			}
		} else {
			w->suffix[pos - img_end] = buf[i];
		}
	}
	return STAGE_OK;
}

int stage_end(stage_writer_t *w)
{
	const uint8_t *s = w->suffix;
	uint32_t want = (uint32_t)s[12] | ((uint32_t)s[13] << 8) | ((uint32_t)s[14] << 16) | ((uint32_t)s[15] << 24);

	if (w->err)
		return w->err;
	if ((w->pos != w->total) || memcmp(&s[8], "UFD", 3) || (s[11] != DFU_SUFFIX_LEN) || ((w->crc ^ 0xFFFFFFFFu) != want))
		return fail(w, STAGE_BAD_IMAGE);
	if (!image_valid(&w->hdr, flash_ptr(STAGE_ADDR + FLASH_PAGE))) /* what flash holds, not what came in */
		return fail(w, STAGE_BAD_IMAGE);
	flash_unlock();
	bool ok = flash_program_page(STAGE_ADDR, (const uint32_t *)&w->hdr);
	flash_lock();
	return (ok && stage_valid()) ? STAGE_OK : fail(w, STAGE_FLASH);
}

bool stage_valid(void)
{
	const app_header_t *h = (const app_header_t *)flash_ptr(STAGE_ADDR);
	return (h->magic == APP_MAGIC) && (h->length <= STAGE_MAX_LEN) && image_valid(h, flash_ptr(STAGE_ADDR + FLASH_PAGE));
}

bool stage_app_valid(void)
{
	return image_valid((const app_header_t *)flash_ptr(APP_HDR_ADDR), flash_ptr(APP_START));
}

void stage_clear(void)
{
	const uint32_t *p = (const uint32_t *)flash_ptr(STAGE_ADDR);

	for (unsigned i = 0; i < FLASH_PAGE / 4u; i++) {
		if (p[i] != FLASH_ERASED) {
			flash_unlock();
			flash_erase_sector(STAGE_ADDR);
			flash_lock();
			return;
		}
	}
}

/* The app area's first sector (with the header) is erased first and the header written last, so from the first erase
 * until the copy checks out the app is invalid and the next boot copies again: the stage stays until then. */
static void install(void)
{
	static app_header_t hdr;
	static uint32_t     page[FLASH_PAGE / 4];
	bool ok = true;

	memcpy(&hdr, flash_ptr(STAGE_ADDR), sizeof(hdr));
	flash_unlock();
	for (uint32_t a = APP_HDR_ADDR; ok && (a < APP_START + hdr.length); a += FLASH_PAGE) {
		if ((a % FLASH_SECTOR) == 0)
			ok = flash_erase_sector(a);
		if (ok && (a != APP_HDR_ADDR)) {
			memcpy(page, flash_ptr(STAGE_ADDR + (a - APP_HDR_ADDR)), FLASH_PAGE);
			ok = flash_program_page(a, page);
		}
	}
	ok = ok && image_valid(&hdr, flash_ptr(APP_START)) && flash_program_page(APP_HDR_ADDR, (const uint32_t *)&hdr);
	flash_lock();
	if (ok && stage_app_valid())
		stage_clear();
}

/* after a failed copy the app area decides: still the old app if the first erase failed, else DFU */
bool stage_boot(bool requested)
{
	bool app_ok = stage_app_valid();

	if ((requested || !app_ok) && stage_valid()) {
		install();
		app_ok = stage_app_valid();
	}
	return app_ok;
}
