/* Host tests for stage.c on a simulated flash that can lose power in any erase or program: the staged image is only
 * ever valid when complete, and a cut anywhere in the copy ends in the new app or DFU. Arguments: two mkdfu.py .dfu. */
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "flash.h"
#include "stage.h"

/* --- simulated flash: 512 KB from BOOT_BASE ----------------------------------------------------------------- */

#define SIM_SIZE (512u * 1024u)

static uint32_t s_mem[SIM_SIZE / 4];
static bool     s_unlocked;
static long     s_ops, s_cut_at = -1, s_fail_at = -1; /* op count; power cut / flash error at that op */
static uint32_t s_lowest;                             /* lowest address written since the last reset_log() */
static jmp_buf  s_power;

static uint32_t *mem(uint32_t addr)
{
	assert((addr >= BOOT_BASE) && (addr - BOOT_BASE < SIM_SIZE));
	return &s_mem[(addr - BOOT_BASE) / 4];
}

const uint8_t *flash_ptr(uint32_t addr) { return (const uint8_t *)mem(addr); }
void flash_unlock(void) { s_unlocked = true; }
void flash_lock(void) { s_unlocked = false; }

/* as flash.c: only [APP_HDR_ADDR, APP_END); a cut does half the job, then the board is dead until the next boot */
static bool op(uint32_t addr, uint32_t len, uint32_t *words_done)
{
	assert(s_unlocked && (addr >= APP_HDR_ADDR) && (addr + len <= APP_END));
	if (addr < s_lowest)
		s_lowest = addr;
	long n = s_ops++;
	*words_done = len / 4;
	if (n == s_cut_at) {
		*words_done = len / 8;
		return true;
	}
	return n != s_fail_at;
}

bool flash_erase_sector(uint32_t addr)
{
	uint32_t done;
	assert((addr % FLASH_SECTOR) == 0);
	bool ok = op(addr, FLASH_SECTOR, &done);
	for (uint32_t i = 0; ok && (i < done); i++)
		mem(addr)[i] = FLASH_ERASED;
	if (s_ops - 1 == s_cut_at)
		longjmp(s_power, 1);
	return ok;
}

bool flash_program_page(uint32_t addr, const uint32_t *words)
{
	uint32_t done;
	assert((addr % FLASH_PAGE) == 0);
	for (uint32_t i = 0; i < FLASH_PAGE / 4; i++)
		if (mem(addr)[i] != FLASH_ERASED)
			return false; /* only to erased flash */
	bool ok = op(addr, FLASH_PAGE, &done);
	for (uint32_t i = 0; ok && (i < done); i++)
		mem(addr)[i] = words[i];
	if (s_ops - 1 == s_cut_at)
		longjmp(s_power, 1);
	return ok;
}

static void flash_erase_all(void)
{
	for (unsigned i = 0; i < SIM_SIZE / 4; i++)
		s_mem[i] = FLASH_ERASED;
}

static void reset_log(void)
{
	s_ops = 0;
	s_cut_at = s_fail_at = -1;
	s_lowest = 0xFFFFFFFFu;
}

/* --- images ------------------------------------------------------------------------------------------------- */

typedef struct {
	uint8_t *dfu;
	uint32_t len;
} dfu_t;

static dfu_t load(const char *path)
{
	FILE *f = fopen(path, "rb");
	dfu_t d;
	assert(f);
	fseek(f, 0, SEEK_END);
	d.len = (uint32_t)ftell(f);
	rewind(f);
	d.dfu = malloc(d.len);
	assert(fread(d.dfu, 1, d.len, f) == d.len);
	fclose(f);
	return d;
}

static dfu_t copy(dfu_t d)
{
	dfu_t c = { malloc(d.len), d.len };
	memcpy(c.dfu, d.dfu, d.len);
	return c;
}

/* as dfu-util or USB DFU would leave it: header + image at APP_HDR_ADDR */
static void put_app(dfu_t d)
{
	memcpy(mem(APP_HDR_ADDR), d.dfu, d.len - DFU_SUFFIX_LEN);
}

/* the header's fields and the image (a cut in the header's write can leave its reserved bytes unwritten) */
static bool app_is(dfu_t d)
{
	return stage_app_valid() && !memcmp(mem(APP_HDR_ADDR), d.dfu, offsetof(app_header_t, reserved)) &&
	       !memcmp(mem(APP_START), d.dfu + FLASH_PAGE, d.len - FLASH_PAGE - DFU_SUFFIX_LEN);
}

static void fix_suffix_crc(dfu_t d)
{
	uint32_t c = image_crc32(0, d.dfu, d.len - 4) ^ 0xFFFFFFFFu;
	memcpy(d.dfu + d.len - 4, &c, 4);
}

/* the app's side: the .dfu in pieces of chunk bytes (as TCP delivers them); the first error */
static int stage(dfu_t d, uint32_t chunk)
{
	static stage_writer_t w;
	int r = stage_begin(&w, d.len);
	for (uint32_t off = 0; !r && (off < d.len); off += chunk)
		r = stage_write(&w, d.dfu + off, (d.len - off < chunk) ? d.len - off : chunk);
	return r ? r : stage_end(&w);
}

static bool stage_header_erased(void)
{
	for (unsigned i = 0; i < FLASH_PAGE / 4; i++)
		if (mem(STAGE_ADDR)[i] != FLASH_ERASED)
			return false;
	return true;
}

/* --- tests -------------------------------------------------------------------------------------------------- */

static dfu_t s_a, s_b; /* the running app, the update */

static void test_layout(void)
{
	assert(STAGE_ADDR == 0x08041000u && STAGE_MAX_LEN == 249600u);
	assert(APP_START + STAGE_MAX_LEN <= STAGE_ADDR); /* the largest copy stays below the stage */
}

static void test_stage_good(void)
{
	static const uint32_t chunks[] = { 1, 7, 256, 1460, 4096, 100000 };
	for (unsigned i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
		flash_erase_all();
		put_app(s_a);
		reset_log();
		assert(stage(s_b, chunks[i]) == STAGE_OK);
		assert(stage_valid() && app_is(s_a) && (s_lowest == STAGE_ADDR)); /* nothing below the stage */
	}
}

/* every refusal leaves no valid stage and the app untouched; a bad header is refused before any flash op */
static void test_stage_refused(void)
{
	dfu_t d;
	static stage_writer_t w;

	flash_erase_all();
	put_app(s_a);

	reset_log();
	d = copy(s_b);
	((app_header_t *)d.dfu)->magic ^= 1;
	assert((stage(d, 1000) == STAGE_BAD_IMAGE) && !s_ops);
	d = copy(s_b);
	((app_header_t *)d.dfu)->length += 1; /* disagrees with the length sent */
	assert((stage(d, 1000) == STAGE_BAD_IMAGE) && !s_ops);
	d = copy(s_b);
	((app_header_t *)d.dfu)->length = STAGE_MAX_LEN + 1;
	assert((stage(d, 1000) == STAGE_TOO_LARGE) && !s_ops);
	assert(stage_begin(&w, FLASH_PAGE + STAGE_MAX_LEN + DFU_SUFFIX_LEN + 1) == STAGE_TOO_LARGE);
	assert(stage_begin(&w, FLASH_PAGE + DFU_SUFFIX_LEN) == STAGE_BAD_IMAGE);
	assert(!s_ops);

	d = copy(s_b); /* one bit in the image: the suffix CRC catches it */
	d.dfu[FLASH_PAGE + 5000] ^= 0x10;
	assert(stage(d, 1460) == STAGE_BAD_IMAGE);
	fix_suffix_crc(d); /* and with a matching suffix, the header's CRC does */
	assert(stage(d, 1460) == STAGE_BAD_IMAGE);
	d = copy(s_b);
	d.dfu[d.len - 8] = 'X'; /* not a DFU suffix */
	fix_suffix_crc(d);
	assert(stage(d, 1460) == STAGE_BAD_IMAGE);

	/* too few or too many bytes */
	assert(stage_begin(&w, s_b.len) == STAGE_OK);
	assert(stage_write(&w, s_b.dfu, s_b.len - 1) == STAGE_OK && stage_end(&w) == STAGE_BAD_IMAGE);
	assert(stage_begin(&w, s_b.len) == STAGE_OK);
	assert(stage_write(&w, s_b.dfu, s_b.len) == STAGE_OK && stage_write(&w, s_b.dfu, 1) == STAGE_BAD_IMAGE);
	assert(stage_end(&w) == STAGE_BAD_IMAGE); /* sticky */

	reset_log();
	s_fail_at = 3; /* flash error */
	assert(stage(s_b, 1460) == STAGE_FLASH);
	assert(!stage_valid() && app_is(s_a) && (s_lowest >= STAGE_ADDR));
}

/* a new upload invalidates an older stage as soon as it writes (the header sector goes first) */
static void test_restage(void)
{
	flash_erase_all();
	put_app(s_a);
	assert(stage(s_a, 4096) == STAGE_OK && stage_valid());
	static stage_writer_t w;
	assert(stage_begin(&w, s_b.len) == STAGE_OK && stage_write(&w, s_b.dfu, 2 * FLASH_PAGE) == STAGE_OK);
	assert(!stage_valid() && stage_header_erased());
}

/* power lost at every flash op while the app stages: never a valid stage unless complete, the app untouched */
static void test_stage_power_cut(void)
{
	flash_erase_all();
	put_app(s_a);
	reset_log();
	assert(stage(s_b, 1460) == STAGE_OK);
	long ops = s_ops;
	for (long k = 0; k < ops; k++) {
		flash_erase_all();
		put_app(s_a);
		reset_log();
		s_cut_at = k;
		if (!setjmp(s_power)) {
			stage(s_b, 1460);
			assert(0);
		}
		/* valid only if the header page's fields made it in a cut in its own write: then the image is complete */
		assert(app_is(s_a) && (!stage_valid() || (k == ops - 1)));
		reset_log();
		assert(stage_boot(false) && app_is(s_a) && !s_ops); /* the next boot: the old app, nothing written */
	}
}

static void test_install(void)
{
	flash_erase_all();
	put_app(s_a);
	assert(stage(s_b, 1460) == STAGE_OK);

	reset_log();
	assert(stage_boot(false) && app_is(s_a) && !s_ops); /* no request, valid app: a stale stage is left alone */
	assert(stage_boot(true) && app_is(s_b) && stage_header_erased() && !stage_valid());
	reset_log();
	assert(stage_boot(true) && app_is(s_b) && !s_ops); /* nothing staged now */

	flash_erase_all(); /* no app at all (a USB download cut off), and a stage: installed without a request */
	assert(stage(s_b, 1460) == STAGE_OK);
	assert(stage_boot(false) && app_is(s_b));
}

/* power lost at every flash op of the install: the boot after it, with no flag (BKP is lost), ends in the new app */
static void test_install_power_cut(void)
{
	flash_erase_all();
	put_app(s_a);
	assert(stage(s_b, 1460) == STAGE_OK);
	reset_log();
	assert(stage_boot(true));
	long ops = s_ops;

	for (long k = 0; k < ops; k++) {
		flash_erase_all();
		put_app(s_a);
		assert(stage(s_b, 1460) == STAGE_OK);
		reset_log();
		s_cut_at = k;
		if (!setjmp(s_power)) {
			stage_boot(true);
			assert(0);
		}
		reset_log();
		assert(stage_boot(false) && app_is(s_b));
		reset_log();
		assert(stage_boot(false) && app_is(s_b) && !s_ops); /* and the one after does nothing */
	}
}

/* a flash error in the install: the stage is kept; DFU (false) unless the old app survived (the first erase
 * failed), and a boot without the old app tries again */
static void test_install_fails(void)
{
	for (long k = 0; k < 6; k++) {
		flash_erase_all();
		put_app(s_a);
		assert(stage(s_b, 1460) == STAGE_OK);
		reset_log();
		s_fail_at = k;
		bool started = stage_boot(true);
		assert(stage_valid() && (started == (k == 0)) && (!started || app_is(s_a)));
		reset_log();
		assert(stage_boot(false) && app_is(started ? s_a : s_b));
	}
}

/* a corrupt stage is never installed: with no valid app that's DFU */
static void test_bad_stage(void)
{
	flash_erase_all();
	put_app(s_a);
	assert(stage(s_b, 1460) == STAGE_OK);
	mem(STAGE_ADDR + FLASH_PAGE + 4096)[3] ^= 0x100;
	reset_log();
	assert(stage_boot(true) && app_is(s_a) && !s_ops);
	memset(mem(APP_HDR_ADDR), 0, FLASH_PAGE); /* app header gone */
	reset_log();
	assert(!stage_boot(false) && !s_ops);
}

/* the bootloader's USB DFU download clears a stage, so it can't later overwrite the download */
static void test_clear(void)
{
	flash_erase_all();
	assert(stage(s_b, 1460) == STAGE_OK);
	reset_log();
	stage_clear();
	assert(!stage_valid() && stage_header_erased() && (s_ops == 1));
	stage_clear();
	assert(s_ops == 1); /* already clear: no erase */
}

int main(int argc, char **argv)
{
	assert(argc == 3);
	s_a = load(argv[1]);
	s_b = load(argv[2]);
	test_layout();
	test_stage_good();
	test_stage_refused();
	test_restage();
	test_stage_power_cut();
	test_install();
	test_install_power_cut();
	test_install_fails();
	test_bad_stage();
	test_clear();
	puts("bootloader stage: all tests passed");
	return 0;
}
