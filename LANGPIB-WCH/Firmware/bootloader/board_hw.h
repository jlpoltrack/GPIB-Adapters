#ifndef BOARD_HW_H_
#define BOARD_HW_H_

/* The DFU bootloader's stand-in for the app's board_hw.h: gpib-core's board.h is only on the path for the USB driver
 * and trap.c, and only board_usb_connect() / board_usb_disconnect() from it exist here. */

#include "ch32v4x7.h"

#define BOARD_FLASH_PAGE_SIZE 4096u

static inline void board_watchdog_feed(void) {}

#endif /* BOARD_HW_H_ */
