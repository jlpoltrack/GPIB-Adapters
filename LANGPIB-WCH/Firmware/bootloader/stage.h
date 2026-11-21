#ifndef STAGE_H_
#define STAGE_H_

/* Network update: the app streams a .dfu into the stage (header page last) and resets with STAGE_REQUEST; the
 * bootloader copies it to the app area (header last), then erases the stage. Pure C over flash.h, host-tested. */

#include <stdbool.h>
#include <stdint.h>
#include "image.h"

#define DFU_SUFFIX_LEN 16u

enum { STAGE_OK, STAGE_TOO_LARGE, STAGE_BAD_IMAGE, STAGE_FLASH };

typedef struct {
	app_header_t hdr;                 /* the .dfu's first page, written to the stage once all is checked */
	uint32_t     page[FLASH_PAGE / 4]; /* the image page being filled */
	uint8_t      suffix[DFU_SUFFIX_LEN];
	uint32_t     total, pos;          /* .dfu bytes expected, and taken so far */
	uint32_t     crc;                 /* CRC-32 of all but the suffix's dwCRC */
	uint32_t     erased_to;           /* the stage is erased up to here */
	int          err;                 /* sticky STAGE_* */
} stage_writer_t;

/* The app's side: STAGE_OK or why not. Nothing outside [STAGE_ADDR, APP_END) is written. */
int stage_begin(stage_writer_t *w, uint32_t total); /* total: the .dfu's length (Content-Length) */
int stage_write(stage_writer_t *w, const uint8_t *buf, uint32_t n);
int stage_end(stage_writer_t *w);                   /* suffix, CRC, then the stage's header page */

bool stage_valid(void);     /* a complete image with a good header and CRC is staged */
bool stage_app_valid(void); /* the app area holds a good image */
void stage_clear(void);     /* invalidate the stage (its header page's sector), if it isn't already */

/* The bootloader's side, before anything else: installs the stage if asked (BKP flag) or if the app is invalid (a
 * copy that a power cut stopped). true: a valid app to start; false: stay in DFU. */
bool stage_boot(bool requested);

#endif /* STAGE_H_ */
