#ifndef CLOCK_H_
#define CLOCK_H_

#include <stdbool.h>
#include "ch32v4x7.h"

/* From the reset clock (HSI) to 200 MHz HCLK; false: no 25 MHz crystal, running from the HSI PLL (no USB, no ETH) */
bool clock_init(void);

#endif /* CLOCK_H_ */
