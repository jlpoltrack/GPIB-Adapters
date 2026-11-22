#ifndef BOARD_HW_H_
#define BOARD_HW_H_

/* USBGPIB-STM32 (F103) specifics behind gpib-core's board.h */

#include "stm32f1xx.h"

/* Settings log: the last two 1 KB pages of the app area (stm32f103x8_app.ld) */
#define BOARD_FLASH_PAGE_SIZE 1024u

/* LED on PC13 (active low), bootloader button on PA1 to GND (read only by dapboot, at reset) */
#define LED_PORT GPIOC
#define LED_PIN  13
#define BTN_PORT GPIOA
#define BTN_PIN  1

#define LED(s) do { if (s) LED_PORT->BRR = 1u << LED_PIN; else LED_PORT->BSRR = 1u << LED_PIN; } while (0)

static inline void board_watchdog_feed(void) { IWDG->KR = 0xAAAAu; }

void board_io_init(void); /* LED, button, USB D+ hold; from board_init() unless BOARD_IO_DEFERRED */

#endif /* BOARD_HW_H_ */
