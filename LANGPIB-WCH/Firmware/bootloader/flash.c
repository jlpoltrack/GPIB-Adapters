/* On-chip flash for the bootloader and the app's update stage (stage.c): 4 KB sector erase (PER) and fast 256-byte
 * page programming (FTPG), WCH's sequences. Runs from the zero-wait area (RM 32.2.1). */

#include "flash.h"
#include "image.h"
#include "ch32v4x7.h"

#define KEY1 0x45670123u
#define KEY2 0xCDEF89ABu

static bool in_app(uint32_t addr, uint32_t len)
{
	return (addr >= APP_HDR_ADDR) && (addr < APP_END) && (len <= APP_END - addr);
}

static bool wait_done(void)
{
	while (FLASH->STATR & FLASH_STATR_BSY) {
	}
	uint32_t st = FLASH->STATR;
	FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;
	return (st & (FLASH_STATR_EOP | FLASH_STATR_WRPRTERR)) == FLASH_STATR_EOP;
}

/* Keys only while locked: a wrong or repeated sequence locks the FPEC until reset (RM 32.4.4) */
void flash_unlock(void)
{
	if (FLASH->CTLR & FLASH_CTLR_LOCK) {
		FLASH->KEYR = KEY1;
		FLASH->KEYR = KEY2;
	}
	if (FLASH->CTLR & FLASH_CTLR_FAST_LOCK) {
		FLASH->MODEKEYR = KEY1;
		FLASH->MODEKEYR = KEY2;
	}
	FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;
}

void flash_lock(void)
{
	FLASH->CTLR |= FLASH_CTLR_FAST_LOCK | FLASH_CTLR_LOCK;
}

const uint8_t *flash_ptr(uint32_t addr)
{
	return (const uint8_t *)(uintptr_t)addr;
}

bool flash_erase_sector(uint32_t addr)
{
	if ((addr & (FLASH_SECTOR - 1u)) || !in_app(addr, FLASH_SECTOR))
		return false;
	while (FLASH->STATR & FLASH_STATR_BSY) {
	}
	FLASH->CTLR |= FLASH_CTLR_PER;
	FLASH->ADDR = addr;
	FLASH->CTLR |= FLASH_CTLR_STRT;
	bool ok = wait_done();
	FLASH->CTLR &= ~FLASH_CTLR_PER;

	for (uint32_t i = 0; ok && (i < FLASH_SECTOR / 4u); i++)
		ok = ((const volatile uint32_t *)addr)[i] == FLASH_ERASED;
	return ok;
}

/* One WRBSY check per word and one BSY/WRBSY check before PGSTRT (the probe's pitfall 3) */
bool flash_program_page(uint32_t addr, const uint32_t *words)
{
	volatile uint32_t *dst = (volatile uint32_t *)addr;

	if ((addr & (FLASH_PAGE - 1u)) || !in_app(addr, FLASH_PAGE))
		return false;
	FLASH->CTLR |= FLASH_CTLR_PAGE_PG;
	while (FLASH->STATR & (FLASH_STATR_BSY | FLASH_STATR_WRBSY)) {
	}
	for (uint32_t i = 0; i < FLASH_PAGE / 4u; i++) {
		dst[i] = words[i];
		while (FLASH->STATR & FLASH_STATR_WRBSY) {
		}
	}
	FLASH->CTLR |= FLASH_CTLR_PG_STRT;
	bool ok = wait_done();
	FLASH->CTLR &= ~FLASH_CTLR_PAGE_PG;

	for (uint32_t i = 0; ok && (i < FLASH_PAGE / 4u); i++)
		ok = dst[i] == words[i];
	return ok;
}
