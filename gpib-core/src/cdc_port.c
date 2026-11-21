#include "port.h"
#include "prologix.h"
#include "gpib_bus.h"
#include "tusb.h"

static uint32_t cdc_read(void *ctx, uint8_t *buf, uint32_t max)
{
	(void)ctx;
	return tud_cdc_read(buf, max);
}

static uint32_t cdc_write_room(void *ctx)
{
	(void)ctx;
	return tud_cdc_write_available();
}

static void cdc_write(void *ctx, const void *buf, uint32_t n)
{
	(void)ctx;
	tud_cdc_write(buf, n);
}

static void cdc_flush(void *ctx)
{
	(void)ctx;
	tud_cdc_write_flush();
}

const port_t cdc_port = { cdc_read, cdc_write_room, cdc_write, cdc_flush, NULL, NULL };

/* The Prologix instance on the CDC port */
void prologix_task(void)
{
	static prologix_t s_px;
	static bool s_ready;

	if (!s_ready) {
		prologix_init(&s_px, &cdc_port, GPIB_OWNER_CDC);
		s_ready = true;
	}
	prologix_run(&s_px);
}
