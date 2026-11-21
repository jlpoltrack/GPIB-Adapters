#include "board.h"
#include "settings.h"

#define DAPBOOT_CMD_BOOT 0x4F42u /* dapboot checks BKP->DR1 for this at reset */

static volatile uint32_t s_millis;

static const struct { uint16_t on_ms, period_ms; } s_led_patterns[] = {
	[LED_OFF]       = { 0, 0 },
	[LED_ON]        = { 1, 0 },
	[LED_NO_BUS]    = { 250, 500 },
	[LED_NO_DEVICE] = { 1000, 1100 },
	[LED_NO_CLOCK]  = { 50, 100 },
};
static volatile led_mode_t s_led_mode;
static volatile bool       s_led_busy;
/* Remaining ms: only SysTick_Handler decrements them and the main thread only stores, so no locking */
static volatile uint32_t   s_led_pulse_ms;
static volatile uint32_t   s_led_error_ms;

#define LED_ERROR_MS 1000u

static bool led_state(void)
{
	uint16_t on = s_led_patterns[s_led_mode].on_ms, period = s_led_patterns[s_led_mode].period_ms;

	if (s_led_busy || s_led_pulse_ms)
		return false;
	if (s_led_error_ms)
		return (s_millis % 100u) < 50u;
	return period ? ((s_millis % period) < on) : (on != 0);
}

void SysTick_Handler(void)
{
	s_millis++;
	if (s_led_pulse_ms)
		s_led_pulse_ms--;
	if (s_led_error_ms)
		s_led_error_ms--;
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
	s_led_pulse_ms = ms;
	LED(led_state());
}

void board_led_error(void)
{
	s_led_error_ms = LED_ERROR_MS;
}

uint32_t board_millis(void)
{
	return s_millis;
}

void board_delay_us(uint32_t us)
{
	/* SysTick, not DWT: a debugger leaving clears TRCENA, and a delay on CYCCNT would then never end */
	uint32_t left = us * (SystemCoreClock / 1000000u);
	uint32_t reload = SysTick->LOAD + 1u, last = SysTick->VAL;

	for (;;) {
		uint32_t now = SysTick->VAL;
		uint32_t gone = (now <= last) ? (last - now) : (last + reload - now);
		if (gone >= left)
			return;
		left -= gone;
		last = now;
	}
}

void board_delay_ms(uint32_t ms)
{
	while (ms--)
		board_delay_us(1000);
}

static bool s_hse_ok;

#ifndef HSE_MHZ
#error "HSE_MHZ must be defined (Makefile)"
#endif
#if HSE_MHZ == 16
#define PLL_HSE_DIV RCC_CFGR_PLLXTPRE
#elif HSE_MHZ == 8
#define PLL_HSE_DIV 0u
#else
#error "HSE_MHZ must be 8 or 16"
#endif

/* 16 MHz HSE / 2 (8 MHz / 1) -> PLL x9 -> 72 MHz SYSCLK, USB = PLL / 1.5 = 48 MHz. dapboot leaves the same setup;
 * any other PLL setup is redone. Without a working crystal: HSI / 2 -> PLL x16 -> 64 MHz, too inaccurate for USB. */
#define PLL_CFG_MASK (RCC_CFGR_PLLSRC | RCC_CFGR_PLLXTPRE | RCC_CFGR_PLLMULL)
static void clock_init(void)
{
	bool pll = (RCC->CFGR & RCC_CFGR_SWS) == RCC_CFGR_SWS_PLL;
	if (pll && ((RCC->CFGR & PLL_CFG_MASK) == (RCC_CFGR_PLLSRC | PLL_HSE_DIV | RCC_CFGR_PLLMULL9))) {
		s_hse_ok = true;
	} else {
		if (pll) { /* back to HSI to change the PLL */
			RCC->CR |= RCC_CR_HSION;
			while (!(RCC->CR & RCC_CR_HSIRDY)) {
			}
			RCC->CFGR &= ~RCC_CFGR_SW;
			while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) {
			}
			RCC->CR &= ~RCC_CR_PLLON;
			while (RCC->CR & RCC_CR_PLLRDY) {
			}
		}
		RCC->CR |= RCC_CR_HSEON;
		for (uint32_t i = 0; (i < 200000u) && !(RCC->CR & RCC_CR_HSERDY); i++) {
		}
		s_hse_ok = (RCC->CR & RCC_CR_HSERDY) != 0;
		FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
		if (s_hse_ok) {
			RCC->CFGR = RCC_CFGR_PLLMULL9 | PLL_HSE_DIV | RCC_CFGR_PLLSRC | RCC_CFGR_PPRE1_DIV2;
		} else {
			RCC->CR &= ~RCC_CR_HSEON;
			RCC->CFGR = RCC_CFGR_PLLMULL16 | RCC_CFGR_PPRE1_DIV2;
		}
		RCC->CR |= RCC_CR_PLLON;
		while (!(RCC->CR & RCC_CR_PLLRDY)) {
		}
		RCC->CFGR |= RCC_CFGR_SW_PLL;
		while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
		}
	}
	RCC->CFGR &= ~RCC_CFGR_USBPRE;
	SystemCoreClock = s_hse_ok ? 72000000u : 64000000u;
}

bool board_hse_ok(void)
{
	return s_hse_ok;
}

/* LSI (30-60 kHz) / 64, 2500 counts: 4 s nominal, at least 2.7 s. Paused while a debugger halts the core;
 * a system reset stops it (RM0008 7.1.1, verified on the board), so dapboot runs without it. */
static void watchdog_start(void)
{
	DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;
	IWDG->KR = 0xCCCCu;
	IWDG->KR = 0x5555u;
	IWDG->PR = 4u;
	IWDG->RLR = 2500u - 1u;
	while (IWDG->SR) {
	}
	board_watchdog_feed();
}

/* PA12 as push-pull low overrides the fixed 1K5 D+ pull-up R8 (0x2), floating input releases it (0x4) */
static void usb_dp_hold_low(bool hold)
{
	GPIOA->BRR = 1u << 12;
	GPIOA->CRH = (GPIOA->CRH & ~(0xFu << 16)) | ((hold ? 0x2u : 0x4u) << 16);
}

void board_init(void)
{
	clock_init();
	SysTick_Config(SystemCoreClock / 1000u);

	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPCEN;
#ifndef BOARD_IO_DEFERRED
	board_io_init();
#endif

	watchdog_start();
}

void board_io_init(void)
{
	usb_dp_hold_low(true);

	board_led_mode(LED_OFF);
	GPIOC->CRH = (GPIOC->CRH & ~(0xFu << 20)) | (0x2u << 20); /* PC13 push-pull */
	GPIOA->BSRR = 1u << BTN_PIN;
	GPIOA->CRL = (GPIOA->CRL & ~(0xFu << (BTN_PIN * 4))) | (0x8u << (BTN_PIN * 4)); /* pull-up input */
}

void board_usb_connect(void)
{
	usb_dp_hold_low(false);
	RCC->APB1ENR |= RCC_APB1ENR_USBEN;
}

void board_usb_disconnect(void)
{
	NVIC_DisableIRQ(USB_HP_CAN1_TX_IRQn);
	NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
	if (RCC->APB1ENR & RCC_APB1ENR_USBEN)
		USB->CNTR = USB_CNTR_FRES | USB_CNTR_PDWN;
	RCC->APB1ENR &= ~RCC_APB1ENR_USBEN;
	usb_dp_hold_low(true);
}

void board_get_uid_hex(char *buf)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)UID_BASE;

	for (int i = 0; i < BOARD_SERIAL_LEN / 2; i++) {
		buf[2 * i]     = hex[uid[i] >> 4];
		buf[2 * i + 1] = hex[uid[i] & 0xF];
	}
	buf[BOARD_SERIAL_LEN] = '\0';
}

static void flash_wait(void)
{
	while (FLASH->SR & FLASH_SR_BSY) {
	}
}

static void flash_unlock(void)
{
	FLASH->KEYR = 0x45670123u;
	FLASH->KEYR = 0xCDEF89ABu;
}

void board_flash_erase(const void *page)
{
	flash_unlock();
	flash_wait();
	FLASH->CR |= FLASH_CR_PER;
	FLASH->AR = (uint32_t)(uintptr_t)page;
	FLASH->CR |= FLASH_CR_STRT;
	flash_wait();
	FLASH->CR &= ~FLASH_CR_PER;
	FLASH->CR |= FLASH_CR_LOCK;
}

void board_flash_write(const void *dst, const void *src, unsigned len)
{
	volatile uint16_t *d = (volatile uint16_t *)(uintptr_t)dst;
	const uint16_t *s = src;

	flash_unlock();
	FLASH->CR |= FLASH_CR_PG;
	for (unsigned i = 0; i < len / 2; i++) {
		d[i] = s[i];
		flash_wait();
	}
	FLASH->CR &= ~FLASH_CR_PG;
	FLASH->CR |= FLASH_CR_LOCK;
}

/* Bus trace on USART2 (J3 pin 2, PA2, 115200 8N1) */
void board_trace_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
	RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
	GPIOA->CRL = (GPIOA->CRL & ~(0xFu << 8)) | (0xBu << 8); /* PA2: AF push-pull 50 MHz */
	USART2->BRR = (SystemCoreClock / 2u + 57600u) / 115200u; /* APB1 = SYSCLK / 2 */
	USART2->CR1 = USART_CR1_UE | USART_CR1_TE;
}

bool board_trace_putc(uint8_t c)
{
	if (!(USART2->SR & USART_SR_TXE))
		return false;
	USART2->DR = c;
	return true;
}

void board_reset(void)
{
	settings_flush();
	NVIC_SystemReset();
}

void board_enter_bootloader(void)
{
	settings_flush();
	board_usb_disconnect();
	board_led_mode(LED_OFF);
	board_delay_ms(500); /* let the host register the detach before dapboot enumerates */
	__disable_irq();
	RCC->APB1ENR |= RCC_APB1ENR_PWREN | RCC_APB1ENR_BKPEN;
	PWR->CR |= PWR_CR_DBP;
	BKP->DR1 = DAPBOOT_CMD_BOOT;
	NVIC_SystemReset();
}
