#ifndef COMPAT_H_
#define COMPAT_H_

/* Maps the AVR-libc calls used by the original UsbGpib sources onto the STM32 board layer */

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define _delay_us(us) board_delay_us(us)
#define _delay_ms(ms) board_delay_ms(ms)
#define Delay_MS(ms)  board_delay_ms(ms)

#define PROGMEM
#define PGM_P                const char *
#define pgm_read_byte(p)     (*(const uint8_t *)(p))
#define pgm_read_ptr_far(p)  (*(p))

#endif /* COMPAT_H_ */
