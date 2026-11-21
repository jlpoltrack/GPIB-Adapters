#ifndef RTT_H_
#define RTT_H_

#include <stdint.h>

/* SEGGER RTT compatible console over SWD: one up and one down channel, non-blocking (drops what doesn't fit) */

void     rtt_init(void);
uint32_t rtt_write(const void *buf, uint32_t n); /* bytes taken */
uint32_t rtt_write_room(void);
uint32_t rtt_read(uint8_t *buf, uint32_t max);

#endif /* RTT_H_ */
