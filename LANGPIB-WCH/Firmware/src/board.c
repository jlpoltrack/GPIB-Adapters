#include <stdarg.h>
#include <stdio.h>
#include "board.h"
#include "settings.h"
#include "clock.h"
#include "image.h"

/* BKP DATAR1: asks the DFU bootloader to stay in DFU after the reset (WCH repo docs/bootloader-design.md §4) */
#define BOOTLOADER_MAGIC 0xDF11u


static volatile uint32_t s_millis;
static uint32_t s_reset_cause;
static bool s_rng_ok;
static uint32_t s_rand;

static const struct { uint16_t on_ms, period_ms; } s_led_patterns[] = {
	[LED_OFF]       = { 0, 0 },
	[LED_ON]        = { 1, 0 },
	[LED_NO_BUS]    = { 250, 500 },
	[LED_NO_DEVICE] = { 1000, 1100 },
	[LED_NO_CLOCK]  = { 50, 100 },
};
static volatile led_mode_t s_led_mode;
static volatile bool       s_led_busy;
static volatile uint32_t   s_led_pulse_end;
static volatile uint32_t   s_led_error_end;

#define LED_ERROR_MS 1000u

static bool led_state(void)
{
	uint16_t on = s_led_patterns[s_led_mode].on_ms, period = s_led_patterns[s_led_mode].period_ms;

	if (s_led_busy || ((int32_t)(s_led_pulse_end - s_millis) > 0))
		return false;
	if ((int32_t)(s_led_error_end - s_millis) > 0)
		return (s_millis % 100u) < 50u;
	return period ? ((s_millis % period) < on) : (on != 0);
}

void SysTick_Handler(void)
{
	SysTick->SR = 0;
	s_millis++;
	LED(led_state());
}

void board_led_mode(led_mode_t mode)
{
	s_led_mode = mode;
	LED(led_state());
}

void board_led_busy(bool busy)
{
	s_led_busy = busy;
	LED(led_state());
}

void board_led_pulse(uint32_t ms)
{
	s_led_pulse_end = s_millis + ms;
	LED(led_state());
}

void board_led_error(void)
{
	s_led_error_end = s_millis + LED_ERROR_MS;
}

uint32_t board_millis(void)
{
	return s_millis;
}

void board_delay_us(uint32_t us)
{
	uint32_t start = board_cycles(), cycles = us * (SystemCoreClock / 1000000u);

	while ((board_cycles() - start) < cycles) {
	}
}

void board_delay_ms(uint32_t ms)
{
	while (ms--) {
		board_delay_us(1000);
		board_watchdog_feed();
	}
}

static bool s_hse_ok;

bool board_hse_ok(void)
{
	return s_hse_ok;
}

/* LSI (~40 kHz) / 64, 2500 counts: about 4 s. Paused while a debugger halts the core (DBGMCU_CR is CSR 0x7C0).
 * The LSI must run first: PVU/RVU only clear on its clock (on silicon they never did without it). */
static bool s_wdg_ok;

static bool iwdg_idle(void)
{
	for (uint32_t t0 = board_cycles(); (board_cycles() - t0) < SystemCoreClock / 100u;) /* 10 ms, ~400 LSI ticks */
		if (!(IWDG->STATR & 3u))
			return true;
	return false;
}

static void watchdog_start(void)
{
	RCC->RSTSCKR |= RCC_LSION;
	for (uint32_t t0 = board_cycles(); !(RCC->RSTSCKR & RCC_LSIRDY);)
		if ((board_cycles() - t0) > SystemCoreClock / 100u)
			return; /* no LSI: run without the watchdog, board_wdg_ok() says so */
	__asm volatile("csrs 0x7C0, %0" ::"r"(1u << 8)); /* IWDG_STOP */
	IWDG->CTLR = 0x5555u;
	if (!iwdg_idle())
		return;
	IWDG->PSCR = 4u;
	IWDG->RLDR = 2500u - 1u;
	if (!iwdg_idle())
		return;
	IWDG->CTLR = 0xCCCCu;
	board_watchdog_feed();
	s_wdg_ok = true;
}

bool board_wdg_ok(void)
{
	return s_wdg_ok;
}

/* push-pull output, 10 MHz */
static void gpio_output(GPIO_TypeDef *port, unsigned pin)
{
	volatile uint32_t *cfg = (pin < 8) ? &port->CFGLR : &port->CFGHR;
	unsigned sh = (pin & 7u) * 4u;
	*cfg = (*cfg & ~(0xFu << sh)) | (0x1u << sh);
}

/* input with the pull-up */
static void gpio_input_pullup(GPIO_TypeDef *port, unsigned pin)
{
	volatile uint32_t *cfg = (pin < 8) ? &port->CFGLR : &port->CFGHR;
	unsigned sh = (pin & 7u) * 4u;
	port->BSHR = 1u << pin;
	*cfg = (*cfg & ~(0xFu << sh)) | (0x8u << sh);
}

/* Pins with nothing on them (PA0-7, PB0/1/6/7, PC0-5, PC13-15): analog mode, so no floating input draws current */
static void unused_pins_analog(void)
{
	GPIOA->CFGLR = 0;
	GPIOB->CFGLR &= ~0xFF0000FFu;
	GPIOC->CFGLR &= ~0x00FFFFFFu;
	GPIOC->CFGHR &= ~0xFFF00000u;
}

/* board_random()'s seed: the RNG if it delivers within ~1 ms, else the UID and the cycle count */
static void random_seed(void)
{
	const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u;
	uint32_t seed = uid[0] ^ (uid[1] * 31u) ^ (uid[2] * 961u);

	RCC->HBPCENR |= RCC_RNGEN;
	RNG->CR = RNG_CR_RNGEN;
	for (uint32_t t0 = board_cycles(); (board_cycles() - t0) < SystemCoreClock / 1000u;) {
		if ((RNG->SR & 0x61u) == 0x01u) { /* DRDY, no seed or clock error */
			s_rng_ok = true;
			seed ^= RNG->DR;
			break;
		}
	}
	s_rand = (seed ^ board_cycles()) | 1u;
}

/* xorshift32 for lwIP (DHCP xid, ports, TCP ISN): newlib-nano's rand() pulls in assert, stdio and malloc */
uint32_t board_random(void)
{
	s_rand ^= s_rand << 13;
	s_rand ^= s_rand >> 17;
	s_rand ^= s_rand << 5;
	return s_rand;
}

void board_init(void)
{
	s_reset_cause = RCC->RSTSCKR;
	RCC->RSTSCKR |= RCC_RMVF;

	s_hse_ok = clock_init();

	RCC->PB2PCENR |= RCC_AFIOEN | RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_IOPDEN;
	unused_pins_analog();
	board_led_mode(LED_OFF);
	gpio_output(LED_PORT, LED_PIN);
	gpio_input_pullup(DFU_PORT, DFU_PIN);

	/* 1 ms tick from HCLK, counting up to CMP and back to 0 */
	SysTick->CTLR = 0;
	SysTick->CMP = SystemCoreClock / 1000u - 1u;
	SysTick->CNT = 0;
	SysTick->SR = 0;
	SysTick->CTLR = (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1) | (1u << 0); /* INIT STRE STCLK STIE STE */
	NVIC_EnableIRQ(SysTick_IRQn);
	board_irq_enable();

	random_seed();
	watchdog_start();
}

bool board_rng_ok(void)
{
	return s_rng_ok;
}

bool board_dfu_button(void)
{
	return !(DFU_PORT->INDR & (1u << DFU_PIN));
}

uint32_t board_reset_cause(void)
{
	return s_reset_cause;
}

void board_get_uid_hex(char *buf)
{
	static const char hex[] = "0123456789ABCDEF";
	const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u; /* ESIG_UNIID1-3 (RM 31) */

	for (int w = 0; w < 3; w++) {
		uint32_t v = uid[w];
		for (int i = 0; i < 4; i++, v >>= 8) {
			buf[8 * w + 2 * i]     = hex[(v >> 4) & 0xFu];
			buf[8 * w + 2 * i + 1] = hex[v & 0xFu];
		}
	}
	buf[BOARD_SERIAL_LEN] = '\0';
}

/* Standard (compatible) mode, RM 32.2.1: 4 KB sector erase, 16-bit programming. The CPU stalls meanwhile; this code
 * must run from the zero-wait area, which all of it does. Enhanced read mode is never on. */
static void flash_wait(void)
{
	while (FLASH->STATR & FLASH_STATR_BSY) {
	}
}

static void flash_unlock(void)
{
	flash_wait();
	FLASH->KEYR = 0x45670123u;
	FLASH->KEYR = 0xCDEF89ABu;
	FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;
}

static void flash_lock(void)
{
	FLASH->CTLR = (FLASH->CTLR & ~(FLASH_CTLR_PG | FLASH_CTLR_PER)) | FLASH_CTLR_LOCK;
}

void board_flash_erase(const void *page)
{
	flash_unlock();
	FLASH->CTLR |= FLASH_CTLR_PER;
	FLASH->ADDR = (uint32_t)(uintptr_t)page;
	FLASH->CTLR |= FLASH_CTLR_STRT;
	flash_wait();
	flash_lock();
}

void board_flash_write(const void *dst, const void *src, unsigned len)
{
	volatile uint16_t *d = (volatile uint16_t *)(uintptr_t)dst;
	const uint16_t *s = src;

	flash_unlock();
	FLASH->CTLR |= FLASH_CTLR_PG;
	for (unsigned i = 0; i < len / 2; i++) {
		d[i] = s[i];
		flash_wait();
	}
	flash_lock();
}

/* USART1 on J4 (PA9 TX, 115200 8N1): the bus trace and the bring-up log. APB2 = HCLK. */
void board_trace_init(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	RCC->PB2PCENR |= RCC_IOPAEN | RCC_USART1EN;
	GPIOA->CFGHR = (GPIOA->CFGHR & ~(0xFu << 4)) | (0xBu << 4); /* PA9: AF push-pull, 50 MHz */
	USART1->BRR = (uint16_t)((SystemCoreClock + 57600u) / 115200u);
	USART1->CTLR1 = USART_CTLR1_UE | USART_CTLR1_TE;
}

bool board_trace_putc(uint8_t c)
{
	if (!(USART1->STATR & USART_STATR_TXE))
		return false;
	USART1->DATAR = c;
	return true;
}

void board_log(const char *fmt, ...)
{
	char buf[160];
	va_list ap;

	board_trace_init();
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	for (int i = 0; i < n; i++) {
		if (buf[i] == '\n')
			while (!board_trace_putc('\r')) {
			}
		while (!board_trace_putc((uint8_t)buf[i])) {
		}
	}
}

/* PFIC system reset (CFGR, KEY3 | SYSRESET), WCH's NVIC_SystemReset() */
static void __attribute__((noreturn)) system_reset(void)
{
	board_irq_disable();
	NVIC_SystemReset();
	for (;;) {
	}
}

void board_reset(void)
{
	settings_flush();
	system_reset();
}

/* the bootloader reads (and clears) BKP DATAR1 first thing */
static void __attribute__((noreturn)) reset_to_bootloader(uint16_t request)
{
	RCC->PB1PCENR |= RCC_PWREN | RCC_BKPEN;
	PWR->CTLR |= PWR_CTLR_DBP;
	BKP->DATAR1 = request;
	system_reset();
}

void board_enter_bootloader(void)
{
	settings_flush();
	board_usb_disconnect();
	board_led_mode(LED_OFF);
	board_delay_ms(500); /* let the host register the detach before the bootloader enumerates */
	reset_to_bootloader(BOOTLOADER_MAGIC);
}

void board_install_update(void)
{
	settings_flush();
	board_usb_disconnect();
	board_led_mode(LED_OFF);
	board_log("update: resetting to install\n");
	reset_to_bootloader(STAGE_REQUEST);
}
