#ifndef ARCH_CC_H_
#define ARCH_CC_H_

/* lwIP port: newlib-nano, little-endian RV32, no OS */

#include <stdint.h>

#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { for (;;) { } } while (0) /* the watchdog resets */
uint32_t board_random(void);
#define LWIP_RAND() ((u32_t)board_random())
#define LWIP_NO_UNISTD_H 1

#endif /* ARCH_CC_H_ */
