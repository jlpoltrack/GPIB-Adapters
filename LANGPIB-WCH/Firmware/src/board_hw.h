#ifndef BOARD_HW_H_
#define BOARD_HW_H_

/* LANGPIB-WCH specifics behind gpib-core's board.h */

#include "ch32v4x7.h"

/* Settings log: the last two 4 KB erase sectors of the zero-wait area (ch32v407_app.ld). Erased words read
 * 0xE339E339 on WCH flash, not 0xFFFFFFFF (RM 32 errata). */
#define BOARD_FLASH_PAGE_SIZE 4096u
#define BOARD_FLASH_ERASED    0xE339E339u

/* D1 sinks into PA8 through 330R from +3V3 (active low). SW1 (DFU) pulls PD8 to GND, internal pull-up. */
#define LED_PORT GPIOA
#define LED_PIN  8
#define DFU_PORT GPIOD
#define DFU_PIN  8

#define LED(on) (LED_PORT->BSHR = (on) ? (1u << (LED_PIN + 16)) : (1u << LED_PIN))

static inline void board_watchdog_feed(void) { IWDG->CTLR = 0xAAAAu; }

/* mstatus.MIE: the standard bit, so CSR 0x800 (WCH's __enable_irq) needs no CPU_RUN_CTLR.non_usta */
static inline void board_irq_enable(void) { __asm volatile("csrsi mstatus, 8" ::: "memory"); }
static inline void board_irq_disable(void) { __asm volatile("csrci mstatus, 8" ::: "memory"); }

static inline uint32_t board_cycles(void)
{
	uint32_t c;
	__asm volatile("csrr %0, mcycle" : "=r"(c));
	return c;
}

void board_log(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* USART1, blocking: bring-up only */
bool board_dfu_button(void);  /* SW1 held */
uint32_t board_reset_cause(void); /* RCC_RSTSCKR at boot (flags cleared after) */
bool board_wdg_ok(void);          /* the IWDG runs (else the LSI didn't start) */
bool board_rng_ok(void);          /* the RNG seeded board_random() (else the UID and the cycle count did) */
uint32_t board_random(void);
void board_install_update(void) __attribute__((noreturn)); /* reset: the bootloader installs the staged image */

#endif /* BOARD_HW_H_ */
