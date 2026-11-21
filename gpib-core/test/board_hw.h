#ifndef BOARD_HW_H_
#define BOARD_HW_H_

/* board_hw.h for the host tests and server: 1 KB flash pages, no watchdog */

#define BOARD_FLASH_PAGE_SIZE 1024u

static inline void board_watchdog_feed(void) {}

#endif
