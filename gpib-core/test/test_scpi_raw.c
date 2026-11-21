/* Host tests for the raw SCPI socket protocol: a fake port, simulated instruments (make test) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scpi_raw.h"
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "instr.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t gpib_addr = 5;
static char s_readterm;
char instr_get_readterm(void) { return s_readterm; }

static uint8_t  s_in[4096], s_out[4096];
static uint32_t s_in_len, s_in_pos, s_out_len, s_room;

static uint32_t fake_read(void *ctx, uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	(void)ctx;
	while ((n < max) && (s_in_pos < s_in_len))
		buf[n++] = s_in[s_in_pos++];
	return n;
}
static uint32_t fake_write_room(void *ctx) { (void)ctx; return s_room; }
static void fake_write(void *ctx, const void *buf, uint32_t n)
{
	(void)ctx;
	memcpy(&s_out[s_out_len], buf, n);
	s_out_len += n;
}
static void fake_flush(void *ctx) { (void)ctx; }

static const port_t s_port = { fake_read, fake_write_room, fake_write, fake_flush, NULL, NULL };
static scpi_raw_t s_raw;

/* --- helpers ------------------------------------------------------------------------------------------ */

static void setup(void)
{
	sim_reset();
	gpib_init();
	scpi_raw_init(&s_raw, &s_port);
	s_in_len = s_in_pos = s_out_len = 0;
	s_room = 1024;
	s_readterm = 0;
}

static void send(const char *s)
{
	memcpy(&s_in[s_in_len], s, strlen(s));
	s_in_len += (uint32_t)strlen(s);
}

static void run(void)
{
	for (int i = 0; i < 200000; i++) {
		scpi_raw_run(&s_raw);
		gpib_task();
	}
}

static bool out_is(const char *s)
{
	bool ok = (s_out_len == strlen(s)) && !memcmp(s_out, s, s_out_len);
	if (!ok)
		fprintf(stderr, "output: \"%.*s\" (%u bytes), expected \"%s\"\n", (int)s_out_len, s_out, s_out_len, s);
	s_out_len = 0;
	return ok;
}

static bool dev_got(const sim_dev_t *d, const char *s)
{
	bool ok = (d->in_len == strlen(s)) && !memcmp(d->in, s, d->in_len);
	if (!ok)
		fprintf(stderr, "instrument got: \"%.*s\", expected \"%s\"\n", d->in_len, d->in, s);
	return ok;
}

static void answer_idn(sim_dev_t *d)
{
	sim_answer(d, "ACME,M1,42,1.0\n", true);
}

/* --- tests -------------------------------------------------------------------------------------------- */

static void test_command_no_read(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("*RST\n");
	run();
	assert(dev_got(d, "*RST\n") && d->messages == 1 && d->in_eoi[4]);
	assert(out_is("") && !d->talker);
}

static void test_query_reads_reply(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	send("*IDN?\n");
	run();
	assert(dev_got(d, "*IDN?\n"));
	assert(out_is("ACME,M1,42,1.0\n"));
	assert(!gpib_holds_bus(GPIB_OWNER_RAW) && !d->talker && !d->listener);
}

/* the bus stays with the query from its write to its reply: another front end waits */
static void test_query_holds_bus(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	s_room = 10; /* the read waits for room */
	send("*IDN?\n");
	run();
	assert(gpib_holds_bus(GPIB_OWNER_RAW));
	uint8_t msg[] = "*RST\n";
	gpib_job_t other = { .op = GPIB_OP_WRITE, .addr = 5, .buf = msg, .len = 5, .flags = GPIB_EOI | GPIB_END };
	assert(gpib_submit(&other, GPIB_OWNER_VXI));
	run();
	assert(other.status == GPIB_PENDING && d->messages == 1);
	s_room = 1024;
	run();
	assert(out_is("ACME,M1,42,1.0\n"));
	assert(other.status == GPIB_OK && d->messages == 2);
}

static void test_crlf_and_blank_lines(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("\n\n:VOLT 1\r\n\n");
	run();
	assert(dev_got(d, ":VOLT 1\r\n") && d->messages == 1);
}

static void test_several_lines(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	send("*IDN?\n*IDN?\n");
	run();
	assert(d->messages == 2);
	assert(out_is("ACME,M1,42,1.0\nACME,M1,42,1.0\n"));
}

static void test_long_line_in_pieces(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	char big[SCPI_RAW_LINE_MAX + 40];
	memset(big, 'x', sizeof(big) - 2);
	big[3] = '?'; /* the query mark is in the first piece */
	big[sizeof(big) - 2] = '\n';
	big[sizeof(big) - 1] = '\0';
	d->on_message = answer_idn;
	send(big);
	run();
	assert(dev_got(d, big) && d->messages == 1);
	for (unsigned i = 0; i + 1 < d->in_len; i++)
		assert(!d->in_eoi[i]); /* EOI on the last byte only */
	assert(out_is("ACME,M1,42,1.0\n"));
}

static void test_read_termination(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	s_readterm = '\n';
	send("MEAS?\n");
	sim_answer(d, "1.5\n2.5\n", false); /* no EOI: the termination ends the reply */
	run();
	assert(out_is("1.5\n"));
}

static void test_waits_for_room(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	s_room = 10;
	send("*IDN?\n");
	run();
	assert(out_is("") && d->messages == 1); /* written, but no read until the client has room */
	s_room = 1024;
	run();
	assert(out_is("ACME,M1,42,1.0\n"));
}

static void test_no_listener_no_read(void)
{
	setup();
	send("*IDN?\n");
	run();
	assert(out_is(""));
	assert(gpib_last_error()->status == GPIB_NO_LISTENER);
}

static void test_reset_drops_line(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("*RS");
	run();
	scpi_raw_reset(&s_raw);
	send("*CLS\n");
	run();
	assert(dev_got(d, "*CLS\n"));
}

int main(void)
{
	test_command_no_read();
	test_query_reads_reply();
	test_query_holds_bus();
	test_crlf_and_blank_lines();
	test_several_lines();
	test_long_line_in_pieces();
	test_read_termination();
	test_waits_for_room();
	test_no_listener_no_read();
	test_reset_drops_line();
	puts("scpi raw: all tests passed");
	return 0;
}
