#include "scpi_raw.h"
#include "instr.h"

#define WRITE_TMO 10000 /* ms: a listener that never takes a byte */
#define READ_TMO  10000 /* ms per chunk: a slow measurement still answers */

enum { RAW_IDLE, RAW_WRITE, RAW_READ_MORE, RAW_READ };

void scpi_raw_init(scpi_raw_t *r, const port_t *port)
{
	*r = (scpi_raw_t){ .port = port };
}

void scpi_raw_reset(scpi_raw_t *r)
{
	if (r->job.status == GPIB_PENDING)
		gpib_abort(&r->job);
	r->state = RAW_IDLE;
	r->len = 0;
	r->query = r->sent_part = false;
}

/* A full buffer goes out as a piece without EOI; the line's last piece carries EOI. A query doesn't unaddress after
 * it, so this client keeps the bus until the reply is read: no other front end's command can come in between. */
static void send_line(scpi_raw_t *r, bool final)
{
	uint8_t flags = final ? (uint8_t)(GPIB_EOI | (r->query ? 0 : GPIB_END)) : 0;
	r->job = (gpib_job_t){ .op = GPIB_OP_WRITE, .addr = gpib_addr, .buf = r->line, .len = r->len,
	                       .timeout_ms = WRITE_TMO, .flags = flags };
	gpib_ren(true);
	gpib_submit(&r->job, GPIB_OWNER_RAW);
	r->state = RAW_WRITE;
	r->sent_part = !final;
}

static void submit_read(scpi_raw_t *r)
{
	char term = instr_get_readterm();
	r->job = (gpib_job_t){ .op = GPIB_OP_READ, .addr = gpib_addr, .buf = r->rbuf, .len = sizeof(r->rbuf),
	                       .timeout_ms = READ_TMO, .flags = term ? GPIB_TERM : 0, .term = (uint8_t)term };
	gpib_ren(true);
	gpib_submit(&r->job, GPIB_OWNER_RAW);
	r->state = RAW_READ;
}

/* Input up to the end of a line; true when that started a write */
static bool input(scpi_raw_t *r)
{
	uint8_t c;

	for (int n = 0; (n < 64) && r->port->read(r->port->ctx, &c, 1); n++) {
		if ((c == '\n') && !r->len && !r->sent_part)
			continue; /* empty line, or the LF of a CR LF that ended a line */
		r->line[r->len++] = c;
		r->query |= (c == '?');
		if (c == '\n') {
			send_line(r, true);
			return true;
		}
		if (r->len == sizeof(r->line)) {
			send_line(r, false);
			return true;
		}
	}
	return false;
}

void scpi_raw_run(scpi_raw_t *r)
{
	const port_t *p = r->port;

	switch (r->state) {
	case RAW_IDLE:
		input(r);
		break;
	case RAW_WRITE:
		if (r->job.status == GPIB_PENDING)
			break;
		r->len = 0;
		if (r->sent_part && (r->job.status == GPIB_OK)) {
			r->state = RAW_IDLE;
			break;
		}
		r->state = ((r->job.status == GPIB_OK) && r->query) ? RAW_READ_MORE : RAW_IDLE;
		r->query = r->sent_part = false;
		break;
	case RAW_READ_MORE: /* the bus stays held, the talker held off by NRFD, until the client has room */
		if (p->write_room(p->ctx) >= sizeof(r->rbuf))
			submit_read(r);
		break;
	case RAW_READ:
		if (r->job.status == GPIB_PENDING)
			break;
		p->write(p->ctx, r->rbuf, r->job.done);
		p->flush(p->ctx);
		r->state = (r->job.end || (r->job.status != GPIB_OK)) ? RAW_IDLE : RAW_READ_MORE;
		break;
	}
}
