/* Firmware update over the network: gpib-core http.c's POST /update hooks over the bootloader's stage writer
 * (bootloader/stage.c). Once the reply is out, a reset with STAGE_REQUEST has the bootloader install it. */

#include "update.h"
#include "board.h"
#include "http.h"
#include "stage.h"

_Static_assert(((int)STAGE_OK == (int)HTTP_UPD_OK) && ((int)STAGE_TOO_LARGE == (int)HTTP_UPD_TOO_LARGE) &&
               ((int)STAGE_BAD_IMAGE == (int)HTTP_UPD_BAD_IMAGE) && ((int)STAGE_FLASH == (int)HTTP_UPD_FLASH),
               "same result codes");

#define INSTALL_DELAY_MS 500u /* lwIP sends the reply and the close meanwhile */

extern void _start(void);

static stage_writer_t s_w;
static bool           s_install;
static uint32_t       s_install_at;

/* only behind the bootloader: a standalone build (APP_ORIGIN=0x08000000) has nothing to install it */
bool board_update_supported(void)
{
	return (uintptr_t)_start == APP_START;
}

int board_update_begin(uint32_t len)
{
	board_log("update: receiving %lu bytes\n", (unsigned long)len);
	return stage_begin(&s_w, len);
}

int board_update_write(const uint8_t *buf, uint32_t n)
{
	return stage_write(&s_w, buf, n);
}

int board_update_end(void)
{
	int r = stage_end(&s_w);
	board_log("update: %s\n", r ? "refused" : "staged");
	return r;
}

void board_update_done(void)
{
	s_install = true;
	s_install_at = board_millis() + INSTALL_DELAY_MS;
}

void update_task(void)
{
	if (s_install && ((int32_t)(board_millis() - s_install_at) >= 0))
		board_install_update();
}
