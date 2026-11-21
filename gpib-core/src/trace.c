#include <stddef.h>
#include "trace.h"
#include "gpib_bus.h"
#include "board.h"

#define RING 1024 /* lines that don't fit are cut short */

static char     s_ring[RING];
static uint16_t s_head, s_tail;
static bool     s_on;

static void put_c(char c)
{
	uint16_t next = (uint16_t)((s_head + 1) % RING);
	if (next != s_tail) {
		s_ring[s_head] = c;
		s_head = next;
	}
}

static void put_s(const char *s)
{
	while (*s)
		put_c(*s++);
}

static void put_u(uint32_t v)
{
	char b[11];
	int i = sizeof(b);
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (i < (int)sizeof(b))
		put_c(b[i++]);
}

static void put_hex(uint8_t v)
{
	static const char hex[] = "0123456789ABCDEF";
	put_c(hex[v >> 4]);
	put_c(hex[v & 15]);
}

/* e.g. "  12345 CDC write 5 6B ok "*IDN?\n"" */
static void trace_job(const gpib_job_t *j)
{
	static const char *const op[] = { "write", "read", "spoll", "sdc", "get", "gtl", "llo", "unaddr", "lon", "ton" };
	static const char *const owner[] = { "TMC", "CDC", "VXI", "PRI", "BUS" };
	static const char *const status[] = { "ok", "pending", "TIMEOUT", "aborted", "NO LISTENER" };

	put_u(board_millis());
	put_c(' ');
	put_s(owner[j->owner]);
	put_c(' ');
	put_s(op[j->op]);
	if ((j->op != GPIB_OP_LLO) && (j->op != GPIB_OP_UNADDRESS) && (j->op < GPIB_OP_LISTEN_ONLY)) {
		put_c(' ');
		put_u(j->addr & 0x1Fu);
		if (j->addr >> 5) {
			put_c('.');
			put_u((j->addr >> 5) - 1u);
		}
	}
	if (j->done) {
		put_c(' ');
		put_u(j->done);
		put_c('B');
	}
	put_c(' ');
	put_s(status[j->status]);
	if (j->timed_out)
		put_s(" (timeout)");
	if (j->done && j->buf && (j->op != GPIB_OP_SPOLL)) {
		put_s(" \"");
		for (uint16_t i = 0; (i < j->done) && (i < 40); i++) {
			uint8_t c = j->buf[i];
			if (c == '\n')
				put_s("\\n");
			else if (c == '\r')
				put_s("\\r");
			else if ((c >= 32) && (c < 127) && (c != '"') && (c != '\\'))
				put_c((char)c);
			else {
				put_s("\\x");
				put_hex(c);
			}
		}
		put_s((j->done > 40) ? "\"..." : "\"");
	} else if (j->op == GPIB_OP_SPOLL && (j->status == GPIB_OK)) {
		put_s(" status ");
		put_u(j->buf[0]);
	}
	put_s("\r\n");
}

void trace_init(void)
{
	board_trace_init();
}

void trace_enable(bool on)
{
	s_on = on;
	gpib_set_trace(on ? trace_job : NULL);
	if (on)
		put_s("trace on\r\n");
}

bool trace_enabled(void)
{
	return s_on;
}

void trace_task(void)
{
	while ((s_tail != s_head) && board_trace_putc((uint8_t)s_ring[s_tail]))
		s_tail = (uint16_t)((s_tail + 1) % RING);
}
