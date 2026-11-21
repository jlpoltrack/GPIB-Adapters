#ifndef IMAGE_H_
#define IMAGE_H_

/* Flash layout and the app image header (WCH repo docs/bootloader-design.md §3). Pure C: host-tested in test/. */

#include <stdbool.h>
#include <stdint.h>

#define BOOT_BASE      0x08000000u /* bootloader, sectors 0-3 (WRPR in production) */
#define APP_HDR_ADDR   0x08004000u /* header page; DFU block 0 starts here */
#define APP_START      0x08004100u /* the app's _start */
#define APP_END        0x0807E000u /* settings log from here (two 4 KB sectors): never touched */
#define APP_MAX_LEN    (APP_END - APP_START)
#define FLASH_PAGE     256u
#define FLASH_SECTOR   4096u
#define DFU_BLOCK      FLASH_SECTOR /* wTransferSize: one DFU block is one sector */

/* Network update stage (stage.c): the upper half of the app area, laid out like it (header page, then the image) */
#define STAGE_ADDR     0x08041000u
#define STAGE_MAX_LEN  (APP_END - STAGE_ADDR - FLASH_PAGE)
#define STAGE_REQUEST  0x5354u /* BKP DATAR1 from the app: install the stage */

#define APP_MAGIC      0x4C475042u /* "BPGL" in memory: LANGPIB */
#define APP_HDR_VER    1u

typedef struct {
	uint32_t magic;
	uint32_t hdr_version;
	uint32_t length;      /* bytes of image from APP_START */
	uint32_t crc;         /* CRC-32 (zlib) of those bytes */
	uint32_t app_version; /* bcdDevice of the app, informational */
	uint8_t  reserved[FLASH_PAGE - 20];
} app_header_t;

_Static_assert(sizeof(app_header_t) == FLASH_PAGE, "the header is one flash page");
_Static_assert(STAGE_ADDR - APP_HDR_ADDR == APP_END - STAGE_ADDR, "a staged image copies to below the stage");
_Static_assert((STAGE_ADDR % FLASH_SECTOR) == 0, "the stage starts on a sector");

uint32_t image_crc32(uint32_t crc, const void *buf, uint32_t len); /* zlib convention: start with 0 */

/* magic, version and length sane, and the CRC over the image matches */
bool image_valid(const app_header_t *hdr, const uint8_t *image);

/* Flash address of a DFU download block, or 0 if it would leave [APP_HDR_ADDR, APP_END) */
uint32_t image_block_addr(uint16_t block, uint16_t len);

#endif /* IMAGE_H_ */
