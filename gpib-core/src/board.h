#ifndef BOARD_H_
#define BOARD_H_

/* What gpib-core needs from a board. Each board repo implements it and provides board_hw.h: the device header,
 * BOARD_FLASH_PAGE_SIZE, board_watchdog_feed() (inline) and anything board-only. */

#include <stdint.h>
#include <stdbool.h>
#include "board_hw.h"

#ifndef BOARD_FLASH_ERASED
#define BOARD_FLASH_ERASED 0xFFFFFFFFu /* what an erased flash word reads as (WCH: 0xE339E339) */
#endif

#define BOARD_SERIAL_LEN 24 /* 96-bit UID as hex */

typedef enum {
	LED_OFF,
	LED_ON,        /* ready */
	LED_NO_BUS,    /* no GPIB cable / instrument powered off */
	LED_NO_DEVICE, /* bus present, no instrument answers */
	LED_NO_CLOCK,  /* HSE crystal didn't start: running on HSI, no USB */
} led_mode_t;

void board_init(void);           /* clocks, SysTick, LED, watchdog; USB stays detached */
bool board_hse_ok(void);         /* false: the crystal failed, no USB */
void board_usb_connect(void);    /* tusb_init() follows */
void board_usb_disconnect(void); /* the host sees a detach */
uint32_t board_millis(void);
void board_delay_us(uint32_t us);
void board_delay_ms(uint32_t ms);
void board_get_uid_hex(char *buf); /* buf must hold BOARD_SERIAL_LEN + 1 */
void board_flash_erase(const void *page);                               /* one BOARD_FLASH_PAGE_SIZE page */
void board_flash_write(const void *dst, const void *src, unsigned len); /* to erased flash, len a multiple of 4 */
void board_led_mode(led_mode_t mode);
void board_led_busy(bool busy);     /* LED off during a GPIB transfer */
void board_led_pulse(uint32_t ms);  /* LED off for ms: USB488 indicator pulse */
void board_led_error(void);         /* fast blink for a second: a GPIB transfer failed */
void board_trace_init(void);        /* the bus trace's UART (trace.c) */
bool board_trace_putc(uint8_t c);   /* false while the UART is busy */
void board_reset(void) __attribute__((noreturn));
void board_enter_bootloader(void) __attribute__((noreturn));

#endif /* BOARD_H_ */
