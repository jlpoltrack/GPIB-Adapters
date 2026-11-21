/* Host tests for image.c: the header and CRC as tools/mkdfu.py writes them, and DFU block addressing that can never
 * reach the bootloader or the settings. Argument: a .dfu from mkdfu.py. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"

static uint8_t *load(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	assert(f);
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	rewind(f);
	uint8_t *buf = malloc((size_t)*len);
	assert(fread(buf, 1, (size_t)*len, f) == (size_t)*len);
	fclose(f);
	return buf;
}

static void test_mkdfu_image(const char *path)
{
	long n;
	uint8_t *dfu = load(path, &n);

	/* DFU suffix: "UFD", length 16, dwCRC is CRC-32 without the final inversion */
	assert(n > (long)(FLASH_PAGE + 16));
	assert(memcmp(dfu + n - 8, "UFD", 3) == 0 && dfu[n - 5] == 16);
	uint32_t want;
	memcpy(&want, dfu + n - 4, 4);
	assert((image_crc32(0, dfu, (uint32_t)n - 4) ^ 0xFFFFFFFFu) == want);

	const app_header_t *hdr = (const app_header_t *)dfu;
	const uint8_t *image = dfu + FLASH_PAGE;
	assert(hdr->length == (uint32_t)(n - 16 - FLASH_PAGE));
	assert(image_valid(hdr, image));

	/* one flipped bit anywhere in the image, or a bad header field, and it's refused */
	uint8_t *copy = malloc((size_t)n);
	memcpy(copy, dfu, (size_t)n);
	copy[FLASH_PAGE + hdr->length - 1] ^= 0x01;
	assert(!image_valid((const app_header_t *)copy, copy + FLASH_PAGE));
	memcpy(copy, dfu, (size_t)n);
	((app_header_t *)copy)->magic ^= 1;
	assert(!image_valid((const app_header_t *)copy, copy + FLASH_PAGE));
	memcpy(copy, dfu, (size_t)n);
	((app_header_t *)copy)->length = APP_MAX_LEN + 1;
	assert(!image_valid((const app_header_t *)copy, copy + FLASH_PAGE));

	/* erased flash: the WCH erased pattern is no header */
	app_header_t erased;
	memset(&erased, 0, sizeof(erased));
	for (unsigned i = 0; i < sizeof(erased) / 4; i++)
		((uint32_t *)&erased)[i] = 0xE339E339u;
	assert(!image_valid(&erased, image));

	free(copy);
	free(dfu);
}

static void test_crc_vector(void)
{
	assert(image_crc32(0, "123456789", 9) == 0xCBF43926u); /* the CRC-32 check value */
	assert(image_crc32(image_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

static void test_block_addr(void)
{
	assert(image_block_addr(0, DFU_BLOCK) == APP_HDR_ADDR);
	assert(image_block_addr(1, 100) == APP_HDR_ADDR + DFU_BLOCK);
	assert(image_block_addr(0, 0) == 0);
	assert(image_block_addr(0, DFU_BLOCK + 1) == 0);

	/* every block lands in [APP_HDR_ADDR, APP_END): sectors 0-3 and the settings are out of reach */
	unsigned last = 0;
	for (unsigned b = 0; b <= 0xFFFF; b++) {
		uint32_t a = image_block_addr((uint16_t)b, DFU_BLOCK);
		if (!a)
			continue;
		assert(a >= APP_HDR_ADDR && a + DFU_BLOCK <= APP_END && (a % FLASH_SECTOR) == 0);
		last = b;
	}
	assert(last == (APP_END - APP_HDR_ADDR) / DFU_BLOCK - 1);
	assert(image_block_addr((uint16_t)(last + 1), 1) == 0);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	test_crc_vector();
	test_block_addr();
	test_mkdfu_image(argv[1]);
	puts("bootloader image: all tests passed");
	return 0;
}
