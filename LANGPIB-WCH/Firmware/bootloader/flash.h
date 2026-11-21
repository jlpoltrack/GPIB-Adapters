#ifndef FLASH_H_
#define FLASH_H_

#include <stdbool.h>
#include <stdint.h>

#define FLASH_ERASED 0xE339E339u /* what an erased word reads on WCH flash */

/* Both refuse anything outside [APP_HDR_ADDR, APP_END): the bootloader and the settings can't be touched. Both read
 * back and return false on a mismatch or a flash error. */
void flash_unlock(void); /* normal + fast unlock */
void flash_lock(void);
bool flash_erase_sector(uint32_t addr);                      /* 4 KB aligned */
bool flash_program_page(uint32_t addr, const uint32_t *words); /* 256 B aligned, 64 words, to erased flash */
const uint8_t *flash_ptr(uint32_t addr);                     /* where to read addr (the host tests map it) */

#endif /* FLASH_H_ */
