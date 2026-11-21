/* System clock, shared by the app and the DFU bootloader */

#include "clock.h"

uint32_t SystemCoreClock = 20000000u; /* HSI until clock_init() */

/* WCH's SYSCLK_400MHz_HCLK_200MHz_HSE: 25 MHz HSE x16 = 400 MHz PLL, HCLK = SYSCLK / 2 = 200 MHz, APB1 = APB2 = HCLK,
 * flash clock HCLK / 2 (100 MHz max). Without the crystal the PLL runs from the 20 MHz HSI x20: no USB, no Ethernet. */
bool clock_init(void)
{
	RCC->CTLR |= RCC_HSEON;
	for (uint32_t i = 0; (i < 200000u) && !(RCC->CTLR & RCC_HSERDY); i++) {
	}
	bool hse_ok = (RCC->CTLR & RCC_HSERDY) != 0;
	if (!hse_ok)
		RCC->CTLR &= ~RCC_HSEON;

	RCC->CFGR0 |= RCC_HPRE_DIV2 | RCC_PPRE2_DIV1 | RCC_PPRE1_DIV1;
	RCC->CFGR0 |= hse_ok ? (RCC_PLLSRC_HSE | RCC_PLLMULL16) : (RCC_PLLSRC_HSI | RCC_PLLMULL20);
	RCC->CTLR |= RCC_PLLON;
	while (!(RCC->CTLR & RCC_PLLRDY)) {
	}
	RCC->CFGR2 |= RCC_SYSPLLSRC_PLL | RCC_SYSPLLDIV1;
	while ((RCC->CFGR2 & RCC_SYSPLLSRC) != RCC_SYSPLLSRC_PLL) {
	}

	/* SCKMOD needs the normal and the fast unlock; the locks go back on after */
	FLASH->KEYR = 0x45670123u;
	FLASH->KEYR = 0xCDEF89ABu;
	FLASH->MODEKEYR = 0x45670123u;
	FLASH->MODEKEYR = 0xCDEF89ABu;
	FLASH->CTLR = (FLASH->CTLR & ~FLASH_CTLR_SCKMOD) | FLASH_CTLR_SCKMOD_Div2;
	FLASH->CTLR |= FLASH_CTLR_FAST_LOCK | FLASH_CTLR_LOCK;

	RCC->CFGR2 |= RCC_SYSPLL_GATE;
	RCC->CFGR0 = (RCC->CFGR0 & ~RCC_SW) | RCC_SW_PLL;
	while ((RCC->CFGR0 & RCC_SWS) != RCC_SWS_PLL) {
	}
	SystemCoreClock = 200000000u;
	return hse_ok;
}
