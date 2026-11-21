#include <string.h>
#include "rtt.h"
#include "stm32f1xx.h"

/* Control block layout as SEGGER_RTT.c, so pyocd / OpenOCD / J-Link find it by its ID in RAM */

#define UP_SIZE   1024u
#define DOWN_SIZE 256u

typedef struct {
	const char       *name;
	uint8_t          *buf;
	uint32_t          size;
	volatile uint32_t wr;
	volatile uint32_t rd;
	uint32_t          flags;
} rtt_ring_t;

typedef struct {
	char       id[16];
	int32_t    max_up, max_down;
	rtt_ring_t up, down;
} rtt_cb_t;

static rtt_cb_t s_cb __attribute__((aligned(4)));
static uint8_t  s_up[UP_SIZE], s_down[DOWN_SIZE];

void rtt_init(void)
{
	static const char name[] = "Terminal";

	s_cb.max_up = 1;
	s_cb.max_down = 1;
	s_cb.up = (rtt_ring_t){ name, s_up, UP_SIZE, 0, 0, 0 };
	s_cb.down = (rtt_ring_t){ name, s_down, DOWN_SIZE, 0, 0, 0 };
	/* the ID goes in last and is built here, so the host never finds a half-made block or a copy in flash */
	__DMB();
	memcpy(s_cb.id, "SEGGER", 6);
	memcpy(&s_cb.id[6], " RTT", 5);
	__DMB();
}

uint32_t rtt_write_room(void)
{
	uint32_t wr = s_cb.up.wr, rd = s_cb.up.rd;
	return (rd > wr) ? (rd - wr - 1u) : (UP_SIZE - 1u - (wr - rd));
}

uint32_t rtt_write(const void *buf, uint32_t n)
{
	const uint8_t *p = buf;
	uint32_t room = rtt_write_room(), wr = s_cb.up.wr;

	if (n > room)
		n = room;
	for (uint32_t i = 0; i < n; i++) {
		s_up[wr] = p[i];
		wr = (wr + 1u) % UP_SIZE;
	}
	__DMB();
	s_cb.up.wr = wr;
	return n;
}

uint32_t rtt_read(uint8_t *buf, uint32_t max)
{
	uint32_t rd = s_cb.down.rd, wr = s_cb.down.wr, n = 0;

	while ((rd != wr) && (n < max)) {
		buf[n++] = s_down[rd];
		rd = (rd + 1u) % DOWN_SIZE;
	}
	__DMB();
	s_cb.down.rd = rd;
	return n;
}
