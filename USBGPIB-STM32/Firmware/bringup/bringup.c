/* Bring-up firmware for a bare STM32F103 on SWD only: no crystal, USB, LED or GPIB transceivers needed.
 * The Prologix front end runs on an RTT console; tools/bringup.py drives the tests through `bringup`. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "gpib_bus.h"
#include "gpib_pins.h"
#include "prologix.h"
#include "rtt.h"
#include "settings.h"
#include "trace.h"

#define STR_(x)  #x
#define XSTR_(x) STR_(x)
#define HSE_STR  XSTR_(HSE_MHZ)

enum { CMD_NONE, CMD_HANG, CMD_BOOTLOADER, CMD_PINTEST, CMD_TIMING, CMD_RESET, CMD_PINS };

/* Read and written by the host over SWD */
struct {
	uint32_t magic;       /* 0xB007B007 once running */
	uint32_t sysclk, hse_ok;
	uint32_t reset_csr;   /* RCC->CSR at boot (IWDGRSTF bit 29, SFTRSTF bit 28, PINRSTF bit 26, PORRSTF bit 27) */
	uint32_t bkp_dr1;     /* BKP->DR1 at boot: 0x4F42 after board_enter_bootloader() */
	uint32_t loops;
	uint32_t cmd;         /* CMD_*: the host sets it, cleared when done */
	uint32_t pins_fail;   /* pin test: lines that didn't read low when driven, bit per line_t entry */
	uint32_t data_fail;   /* pin test: DIO bits whose DATA_WRITE didn't land on the mapped pin */
	uint32_t delay_100us_cycles, task_idle_cycles, data_rw_cycles;
	uint32_t pins_on;     /* LED, D+ hold, GPIB and trace pins driven (CMD_PINS) */
} volatile bringup;

uint8_t gpib_addr = 1; /* no autoid here: the Prologix default address */

/* vsnprintf links malloc but never calls it for a string buffer: no heap */
void *_sbrk(int incr)
{
	(void)incr;
	return (void *)-1;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
	char b[160];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0)
		rtt_write(b, ((unsigned)n < sizeof(b)) ? (uint32_t)n : sizeof(b) - 1);
}

typedef struct {
	const char   *name;
	GPIO_TypeDef *port;
	uint8_t       pin;
} line_t;

static const line_t s_lines[] = {
	{ "EOI", EOI_PORT, EOI_PIN },   { "DAV", DAV_PORT, DAV_PIN },   { "NRFD", NRFD_PORT, NRFD_PIN },
	{ "NDAC", NDAC_PORT, NDAC_PIN }, { "IFC", IFC_PORT, IFC_PIN },   { "ATN", ATN_PORT, ATN_PIN },
	{ "REN", REN_PORT, REN_PIN },
};

/* Drive each open-drain line low and read it back. Released lines float without the transceivers, so only the
 * driven-low level is checked. Also proves PA15/PB3/PB4 are GPIOs with JTAG off and SWD still up. */
static void pin_test(void)
{
	uint32_t fail = 0, dfail = 0;

	for (unsigned i = 0; i < sizeof(s_lines) / sizeof(s_lines[0]); i++) {
		const line_t *l = &s_lines[i];
		bool was_high = LINE_OUT(l->port, l->pin) != 0;
		LINE_LOW(l->port, l->pin);
		board_delay_us(2);
		bool low = LINE_STATE(l->port, l->pin) == 0;
		LINE_HIGH(l->port, l->pin);
		board_delay_us(5);
		bool rel = LINE_STATE(l->port, l->pin) != 0;
		if (!was_high)
			LINE_LOW(l->port, l->pin);
		if (!low)
			fail |= 1u << i;
		say("  %-4s P%c%-2u driven low: %s, released reads %s\r\n", l->name, (l->port == GPIOA) ? 'A' : 'B', l->pin,
		    low ? "ok" : "FAIL", rel ? "high" : "low (floating)");
	}
	say("  SRQ  PA9  input reads %s\r\n", SRQ_STATE ? "high" : "low");

	for (unsigned i = 0; i < 8; i++) {
		uint8_t d = (uint8_t)(1u << i);
		DATA_WRITE(d);
		board_delay_us(2);
		uint32_t odr = DATA_PORT->ODR & DATA_MASK, idr = DATA_PORT->IDR;
		uint32_t pin = data_spread(d);
		bool ok = (odr == (DATA_MASK & ~pin)) && !(idr & pin) && (DATA_READ & d);
		if (!ok)
			dfail |= d;
		say("  DIO%u -> PB%-2u %s\r\n", i + 1, (unsigned)__builtin_ctz(pin), ok ? "ok" : "FAIL");
	}
	DATA_RELEASE;
	bringup.pins_fail = fail;
	bringup.data_fail = dfail;
	say("pin test: %s\r\n", (fail || dfail) ? "FAIL" : "pass");
}

static uint32_t cycles_of(void (*fn)(void))
{
	uint32_t t0 = DWT->CYCCNT;
	fn();
	return DWT->CYCCNT - t0;
}

static void delay_100us(void) { board_delay_us(100); }
static void task_idle(void) { gpib_task(); }
static void data_rw(void)
{
	DATA_WRITE(0x55);
	(void)DATA_READ;
	DATA_RELEASE;
}

static void timing(void)
{
	bringup.delay_100us_cycles = cycles_of(delay_100us);
	bringup.task_idle_cycles = cycles_of(task_idle);
	bringup.data_rw_cycles = cycles_of(data_rw);
	say("timing @ %lu MHz: board_delay_us(100) = %lu cycles, idle gpib_task() = %lu, DATA write+read+release = %lu\r\n",
	    (unsigned long)(SystemCoreClock / 1000000u), (unsigned long)bringup.delay_100us_cycles,
	    (unsigned long)bringup.task_idle_cycles, (unsigned long)bringup.data_rw_cycles);
}

static void boot_info(void)
{
	bringup.reset_csr = RCC->CSR;
	RCC->CSR |= RCC_CSR_RMVF;
	RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
	PWR->CR |= PWR_CR_DBP;
	bringup.bkp_dr1 = BKP->DR1;
	BKP->DR1 = 0; /* dapboot would clear it; here nothing else does */
	bringup.sysclk = SystemCoreClock;
	bringup.hse_ok = board_hse_ok();

	uint32_t c = bringup.reset_csr;
	char uid[BOARD_SERIAL_LEN + 1];
	board_get_uid_hex(uid);
	say("\r\nUSBGPIB-STM32 bring-up\r\n");
	say("clock %s, SYSCLK %lu MHz; flash %u KB; UID %s\r\n", bringup.hse_ok ? "HSE " HSE_STR " MHz" : "HSI (no crystal)",
	    (unsigned long)(SystemCoreClock / 1000000u), *(const uint16_t *)FLASHSIZE_BASE, uid);
	say("reset:%s%s%s%s%s; BKP DR1 0x%04lX\r\n", (c & RCC_CSR_IWDGRSTF) ? " watchdog" : "",
	    (c & RCC_CSR_SFTRSTF) ? " software" : "", (c & RCC_CSR_PORRSTF) ? " power-on" : "",
	    (c & RCC_CSR_PINRSTF) ? " pin" : "", (c & RCC_CSR_WWDGRSTF) ? " wwdg" : "", (unsigned long)bringup.bkp_dr1);
	say("settings: px_addr %u, autoid %u, usb_mode %u\r\n", settings.px_addr, settings.autoid, settings.usb_mode);
}

/* 1 s on the 8 MHz reset clock before any pin or clock changes, so tools/swd_rescue.py can always halt the core */
static void debug_window(void)
{
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	while (DWT->CYCCNT < 8000000u) {
	}
}

/* Only on request: on a board that isn't ours these pins may be wired to something that drives them */
static void pins_on(void)
{
	if (bringup.pins_on)
		return;
	board_io_init();
	trace_init();
	gpib_init();
	bringup.pins_on = 1;
	say("GPIB pins on; bus %s\r\n", gpib_connected() ? "sensed (ATN reads high)" : "not sensed (ATN reads low)");
}

static void run_cmd(void)
{
	uint32_t cmd = bringup.cmd;

	if ((cmd == CMD_PINTEST || cmd == CMD_BOOTLOADER) && !bringup.pins_on)
		pins_on();

	switch (cmd) {
	case CMD_HANG:
		say("hanging: the watchdog should reset in ~4 s\r\n");
		for (;;) {
		}
	case CMD_BOOTLOADER:
		board_enter_bootloader();
	case CMD_PINTEST:
		pin_test();
		break;
	case CMD_TIMING:
		timing();
		break;
	case CMD_RESET:
		board_reset();
	case CMD_PINS:
		pins_on();
		break;
	default:
		break;
	}
	bringup.cmd = CMD_NONE;
}

int main(void)
{
	debug_window();
	board_init();
	rtt_init();
	settings_init();
	boot_info();
	say("Pins stay off until the host sets cmd = %u. Prologix commands (++ver, ++help) on this console.\r\n",
	    CMD_PINS);
	bringup.magic = 0xB007B007u;

	for (;;) {
		board_watchdog_feed();
		prologix_task();
		gpib_task();
		trace_task();
		settings_task();
		if (bringup.cmd)
			run_cmd();
		bringup.loops++;
	}
}
