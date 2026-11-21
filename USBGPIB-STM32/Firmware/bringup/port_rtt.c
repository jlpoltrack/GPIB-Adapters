#include "port.h"
#include "prologix.h"
#include "gpib_bus.h"
#include "rtt.h"

/* The Prologix port over RTT instead of USB CDC (bring-up build): stands in for gpib-core's cdc_port.c */

static uint32_t rtt_port_read(void *ctx, uint8_t *buf, uint32_t max)
{
	(void)ctx;
	return rtt_read(buf, max);
}

static uint32_t rtt_port_write_room(void *ctx)
{
	(void)ctx;
	return rtt_write_room();
}

static void rtt_port_write(void *ctx, const void *buf, uint32_t n)
{
	(void)ctx;
	rtt_write(buf, n);
}

static void rtt_port_flush(void *ctx)
{
	(void)ctx;
}

static const port_t rtt_port = {
	.read = rtt_port_read, .write_room = rtt_port_write_room, .write = rtt_port_write, .flush = rtt_port_flush,
};

void prologix_task(void)
{
	static prologix_t s_px;
	static bool s_ready;

	if (!s_ready) {
		prologix_init(&s_px, &rtt_port, GPIB_OWNER_CDC);
		s_ready = true;
	}
	prologix_run(&s_px);
}
