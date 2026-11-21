/* Host tests for gpib_phy + gpib_bus against simulated instruments (make test) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "gpib_sim_pins.h"

static const uint8_t ADDRESS_LISTEN_5[] = { 0x5F, 0x40, 0x3F, 0x25 }; /* UNT, MTA0, UNL, LAG5 */
static const uint8_t ADDRESS_TALK_5[]   = { 0x3F, 0x20, 0x45 };       /* UNL, MLA0, TAG5 */
static const uint8_t UNADDRESS[]        = { 0x3F, 0x5F };

static bool cmds_are(const sim_dev_t *d, uint16_t from, const uint8_t *expect, uint16_t n)
{
	return (d->ncmds >= from + n) && (memcmp(&d->cmds[from], expect, n) == 0);
}

static void setup(void)
{
	sim_reset();
	gpib_init();
}

static gpib_status_t run(gpib_job_t *j)
{
	return gpib_run(j, GPIB_OWNER_TMC);
}

static void answer_idn(sim_dev_t *d)
{
	sim_answer(d, "ACME,M1,42,1.0\n", true);
}

static void test_write(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	gpib_job_t j = { .op = GPIB_OP_WRITE, .addr = 5, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"*RST\n", .len = 5 };
	assert(run(&j) == GPIB_OK && j.done == 5);
	assert(d->in_len == 5 && memcmp(d->in, "*RST\n", 5) == 0);
	for (int i = 0; i < 5; i++)
		assert(d->in_eoi[i] == (i == 4));
	assert(cmds_are(d, 0, ADDRESS_LISTEN_5, 4) && cmds_are(d, 4, UNADDRESS, 2) && d->ncmds == 6);
	assert(!d->listener);
}

static void test_write_in_chunks_addresses_once(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	gpib_job_t a = { .op = GPIB_OP_WRITE, .addr = 5, .buf = (uint8_t *)"ABC", .len = 3 };
	gpib_job_t b = { .op = GPIB_OP_WRITE, .addr = 5, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"DE", .len = 2 };
	assert(run(&a) == GPIB_OK);
	assert(d->listener && d->ncmds == 4);
	assert(run(&b) == GPIB_OK);
	assert(d->in_len == 5 && memcmp(d->in, "ABCDE", 5) == 0 && d->messages == 1);
	assert(!d->in_eoi[2] && d->in_eoi[4]);
	assert(d->ncmds == 6);
}

static void test_read(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	answer_idn(d);
	uint8_t buf[64];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = sizeof(buf) };
	assert(run(&j) == GPIB_OK);
	assert(j.end && j.done == 15 && memcmp(buf, "ACME,M1,42,1.0\n", 15) == 0);
	assert(cmds_are(d, 0, ADDRESS_TALK_5, 3) && cmds_are(d, 3, UNADDRESS, 2));
	assert(!d->talker);
}

static void test_read_term_char(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	sim_answer(d, "1.5\n2.5\n", false);
	uint8_t buf[64];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_TERM, .term = '\n', .buf = buf, .len = sizeof(buf) };
	assert(run(&j) == GPIB_OK && j.end && j.done == 4 && memcmp(buf, "1.5\n", 4) == 0);
}

static void test_read_in_chunks(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	answer_idn(d);
	uint8_t buf[64];
	gpib_job_t a = { .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = 10 };
	assert(run(&a) == GPIB_OK && !a.end && a.done == 10);
	assert(d->talker && d->ncmds == 3); /* still addressed, held off by NRFD */
	gpib_job_t b = { .op = GPIB_OP_READ, .addr = 5, .buf = buf + 10, .len = 54 };
	assert(run(&b) == GPIB_OK && b.end && b.done == 5);
	assert(memcmp(buf, "ACME,M1,42,1.0\n", 15) == 0);
	assert(d->ncmds == 5); /* no second addressing, then UNL UNT */
}

static void test_secondary_address(void)
{
	setup();
	sim_dev_t *d = sim_add(7, 2);
	answer_idn(d);
	uint8_t addr = 7 | ((2 + 1) << 5);
	uint8_t buf[64];
	gpib_job_t w = { .op = GPIB_OP_WRITE, .addr = addr, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"X", .len = 1 };
	assert(run(&w) == GPIB_OK && d->in_len == 1);
	gpib_job_t r = { .op = GPIB_OP_READ, .addr = addr, .buf = buf, .len = sizeof(buf) };
	assert(run(&r) == GPIB_OK && r.end && r.done == 15);
}

static void test_two_instruments(void)
{
	setup();
	sim_dev_t *a = sim_add(5, -1), *b = sim_add(9, -1);
	gpib_job_t j = { .op = GPIB_OP_WRITE, .addr = 9, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"HI", .len = 2 };
	assert(run(&j) == GPIB_OK);
	assert(a->in_len == 0 && b->in_len == 2);
}

static void test_serial_poll(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->status = 0x10;
	d->srq = true;
	uint8_t sb = 0;
	gpib_job_t j = { .op = GPIB_OP_SPOLL, .addr = 5, .buf = &sb, .timeout_ms = 500 };
	assert(gpib_srq());
	assert(run(&j) == GPIB_OK && sb == 0x50);
	static const uint8_t seq[] = { 0x3F, 0x20, 0x18, 0x45, 0x19, 0x5F };
	assert(cmds_are(d, 0, seq, sizeof(seq)));
	assert(!gpib_srq() && !d->spe);
}

static void test_addressed_commands(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	gpib_job_t j = { .op = GPIB_OP_GET, .addr = 5, .timeout_ms = 500 };
	assert(run(&j) == GPIB_OK && d->get == 1);
	j = (gpib_job_t){ .op = GPIB_OP_SDC, .addr = 5, .timeout_ms = 500 };
	uint64_t t0 = sim_now_us;
	assert(run(&j) == GPIB_OK && d->sdc == 1 && (sim_now_us - t0) >= 150000);
	j = (gpib_job_t){ .op = GPIB_OP_GTL, .addr = 5, .timeout_ms = 500 };
	assert(run(&j) == GPIB_OK && d->gtl == 1);
	j = (gpib_job_t){ .op = GPIB_OP_LLO, .timeout_ms = 500 };
	assert(run(&j) == GPIB_OK && d->llo == 1);
	assert(!d->listener);
}

static void test_no_listener(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	gpib_job_t j = { .op = GPIB_OP_WRITE, .addr = 12, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"X", .len = 1, .timeout_ms = 100 };
	assert(run(&j) == GPIB_NO_LISTENER);
	assert(gpib_last_error()->status == GPIB_NO_LISTENER && gpib_last_error()->phase == GPIB_PHASE_WRITE);
	assert(sim_led_errors == 1 && d->ifc == 1); /* recovery IFC */
}

static void test_read_timeout_recovers(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1); /* never answers */
	uint8_t buf[8];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = sizeof(buf), .timeout_ms = 50 };
	uint64_t t0 = sim_now_us;
	assert(run(&j) == GPIB_TIMEOUT);
	assert((sim_now_us - t0) >= 50000 && (sim_now_us - t0) < 100000);
	assert(gpib_last_error()->phase == GPIB_PHASE_READ);
	assert(!d->talker && d->ifc == 1);
	sim_answer(d, "OK", true); /* and the bus works afterwards */
	j = (gpib_job_t){ .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = sizeof(buf), .timeout_ms = 50 };
	assert(run(&j) == GPIB_OK && j.done == 2);
}

static void test_abort(void)
{
	setup();
	sim_add(5, -1);
	uint8_t buf[8];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = sizeof(buf) };
	assert(gpib_submit(&j, GPIB_OWNER_TMC));
	for (int i = 0; i < 5; i++)
		gpib_task();
	assert(j.status == GPIB_PENDING);
	gpib_abort(&j);
	assert(j.status == GPIB_ABORTED && sim_led_errors == 0);
	gpib_job_t k = { .op = GPIB_OP_GET, .addr = 5, .timeout_ms = 100 };
	assert(run(&k) == GPIB_OK); /* the owner can submit again */
}

static void test_spoll_cuts_into_waiting_read(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	answer_idn(d);
	d->out_delay = 200000; /* slow measurement */
	d->status = 0x01;
	uint8_t buf[64], sb = 0;
	gpib_job_t r = { .op = GPIB_OP_READ, .addr = 5, .buf = buf, .len = sizeof(buf) };
	assert(gpib_submit(&r, GPIB_OWNER_TMC));
	for (int i = 0; i < 10; i++)
		gpib_task();
	assert(d->talker);
	gpib_job_t p = { .op = GPIB_OP_SPOLL, .addr = 5, .buf = &sb, .timeout_ms = 500 };
	assert(gpib_submit_priority(&p));
	while (p.status == GPIB_PENDING)
		gpib_task();
	assert(p.status == GPIB_OK && sb == 0x01 && r.status == GPIB_PENDING);
	while (r.status == GPIB_PENDING)
		gpib_task();
	assert(r.status == GPIB_OK && r.end && r.done == 15);
	/* talker address, spoll, talker address again, unaddress */
	assert(cmds_are(d, 0, ADDRESS_TALK_5, 3) && cmds_are(d, 9, ADDRESS_TALK_5, 3) && cmds_are(d, 12, UNADDRESS, 2));
}

static void test_spoll_cuts_into_write(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->slow = 2000; /* slow listener: about 4 ms per byte */
	uint8_t msg[20], sb = 0;
	memset(msg, 'x', sizeof(msg));
	gpib_job_t w = { .op = GPIB_OP_WRITE, .addr = 5, .flags = GPIB_EOI | GPIB_END, .buf = msg, .len = sizeof(msg) };
	assert(gpib_submit(&w, GPIB_OWNER_TMC));
	while (d->in_len < 3)
		gpib_task();
	gpib_job_t p = { .op = GPIB_OP_SPOLL, .addr = 5, .buf = &sb, .timeout_ms = 2000 };
	assert(gpib_submit_priority(&p));
	while (p.status == GPIB_PENDING)
		gpib_task();
	assert(p.status == GPIB_OK && w.status == GPIB_PENDING && d->in_len < sizeof(msg));
	while (w.status == GPIB_PENDING)
		gpib_task();
	assert(w.status == GPIB_OK && d->in_len == sizeof(msg) && d->messages == 1);
	for (unsigned i = 0; i < sizeof(msg); i++)
		assert(d->in_eoi[i] == (i == sizeof(msg) - 1));
	/* listen address, spoll (6 bytes), listen address again, unaddress */
	assert(cmds_are(d, 0, ADDRESS_LISTEN_5, 4) && cmds_are(d, 10, ADDRESS_LISTEN_5, 4) && cmds_are(d, 14, UNADDRESS, 2));
}

static void test_owner_lock(void)
{
	setup();
	sim_dev_t *a = sim_add(5, -1), *b = sim_add(9, -1);
	gpib_job_t w1 = { .op = GPIB_OP_WRITE, .addr = 5, .buf = (uint8_t *)"AB", .len = 2 };
	assert(run(&w1) == GPIB_OK); /* message not finished: TMC keeps the bus */
	gpib_job_t other = { .op = GPIB_OP_WRITE, .addr = 9, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"Z", .len = 1 };
	assert(gpib_submit(&other, GPIB_OWNER_CDC));
	for (int i = 0; i < 20; i++)
		gpib_task();
	assert(other.status == GPIB_PENDING && b->in_len == 0);
	gpib_job_t w2 = { .op = GPIB_OP_WRITE, .addr = 5, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"C", .len = 1 };
	assert(gpib_submit(&w2, GPIB_OWNER_TMC));
	while ((w2.status == GPIB_PENDING) || (other.status == GPIB_PENDING))
		gpib_task();
	assert(a->in_len == 3 && memcmp(a->in, "ABC", 3) == 0 && b->in_len == 1);
}

static void test_abort_after_partial_write_releases_bus(void)
{
	setup();
	sim_dev_t *a = sim_add(5, -1), *b = sim_add(9, -1);
	gpib_job_t w1 = { .op = GPIB_OP_WRITE, .addr = 5, .buf = (uint8_t *)"AB", .len = 2 };
	assert(run(&w1) == GPIB_OK && a->listener);
	gpib_abort(&w1); /* the host abandons the message */
	gpib_job_t other = { .op = GPIB_OP_WRITE, .addr = 9, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"Z", .len = 1 };
	assert(gpib_run(&other, GPIB_OWNER_CDC) == GPIB_OK && b->in_len == 1);
	for (int i = 0; i < 20; i++)
		gpib_task();
	assert(!a->listener && !b->listener && a->ifc == 0);

	setup(); /* idle after the abort: the bus unaddresses itself */
	a = sim_add(5, -1);
	assert(run(&w1) == GPIB_OK && a->listener);
	gpib_abort(&w1);
	for (int i = 0; i < 20; i++)
		gpib_task();
	assert(!a->listener && a->ifc == 0 && sim_led_errors == 0);
	sim_powered = false;
	assert(!gpib_connected());
}

static void test_spoll_into_soft_read_keeps_it_soft(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->out_delay = 1000000; /* never answers in time */
	uint8_t buf[16], sb = 0;
	gpib_job_t r = { .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_SOFT_TMO, .buf = buf, .len = sizeof(buf), .timeout_ms = 20 };
	assert(gpib_submit(&r, GPIB_OWNER_TMC));
	while (!d->talker)
		gpib_task();
	uint64_t t = sim_now_us;
	while (sim_now_us < t + 15000)
		gpib_task();
	d->slow = 2000; /* the poll outlasts the read's remaining 5 ms */
	gpib_job_t p = { .op = GPIB_OP_SPOLL, .addr = 5, .buf = &sb, .timeout_ms = 2000 };
	assert(gpib_submit_priority(&p));
	while ((p.status == GPIB_PENDING) || (r.status == GPIB_PENDING))
		gpib_task();
	assert(p.status == GPIB_OK && r.status == GPIB_OK && r.timed_out && r.done == 0);
	assert(d->ifc == 0 && sim_led_errors == 0);
}

static void test_recovery_keeps_ren(void)
{
	setup();
	gpib_ren(false);
	gpib_job_t j = { .op = GPIB_OP_WRITE, .addr = 5, .flags = GPIB_EOI | GPIB_END, .buf = (uint8_t *)"X", .len = 1 };
	assert(run(&j) == GPIB_NO_LISTENER);
	assert(sim_ctrl & L_REN);
	gpib_ren(true);
	assert(!(sim_ctrl & L_REN));
}

static void test_find_device(void)
{
	setup();
	assert(gpib_find_device() == GPIB_NO_ADDR);
	setup();
	sim_add(17, -1);
	assert(gpib_find_device() == 17);
	setup();
	sim_add(4, 0); /* needs secondary address 0 */
	assert(gpib_find_device() == (4 | (1 << 5)));
}

static void test_soft_timeout_ends_read(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	sim_answer(d, "12", false); /* no EOI, no terminator: only the timeout ends it */
	uint8_t buf[16];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_SOFT_TMO, .buf = buf, .len = sizeof(buf), .timeout_ms = 20 };
	assert(run(&j) == GPIB_OK && j.timed_out && j.end && j.done == 2);
	assert(cmds_are(d, 3, UNADDRESS, 2) && d->ifc == 0 && sim_led_errors == 0); /* unaddressed, no IFC */
}

static void test_soft_timeout_is_per_byte(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->slow = 3000; /* ~6 ms per byte, against a 20 ms per-byte timeout */
	sim_answer(d, "0123456789", true);
	uint8_t buf[16];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_SOFT_TMO, .buf = buf, .len = sizeof(buf), .timeout_ms = 20 };
	assert(run(&j) == GPIB_OK && !j.timed_out && j.end && j.done == 10);
}

static void test_ignore_eoi_returns_at_eoi(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	sim_answer(d, "AB", true);
	uint8_t buf[16];
	gpib_job_t j = { .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_IGNORE_EOI | GPIB_SOFT_TMO, .buf = buf, .len = sizeof(buf), .timeout_ms = 20 };
	assert(run(&j) == GPIB_OK && j.eoi && !j.end && j.done == 2 && d->talker);
	j = (gpib_job_t){ .op = GPIB_OP_READ, .addr = 5, .flags = GPIB_IGNORE_EOI | GPIB_SOFT_TMO, .buf = buf, .len = sizeof(buf), .timeout_ms = 20 };
	assert(run(&j) == GPIB_OK && j.timed_out && j.done == 0 && !d->talker);
}

static void test_scan(void)
{
	setup();
	sim_add(3, -1);
	sim_add(22, -1);
	uint8_t found[8];
	assert(gpib_scan(found, sizeof(found)) == 2 && found[0] == 3 && found[1] == 22);
	gpib_job_t w = { .op = GPIB_OP_WRITE, .addr = 3, .buf = (uint8_t *)"A", .len = 1 };
	assert(run(&w) == GPIB_OK);
	assert(gpib_scan(found, sizeof(found)) == -1); /* an instrument is still addressed */
}

static void test_listen_only(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->talker = true; /* talk-only mode: talks without being addressed */
	sim_answer(d, "PU;PA100,200;", true);
	uint8_t buf[32];
	gpib_job_t j = { .op = GPIB_OP_LISTEN_ONLY, .flags = GPIB_IGNORE_EOI, .buf = buf, .len = sizeof(buf) };
	assert(run(&j) == GPIB_OK && j.eoi && j.done == 13 && !memcmp(buf, "PU;PA100,200;", 13));
	assert(d->ncmds == 0); /* no ATN traffic at all */
}

static void test_talk_only(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->listener = true; /* listen-only device */
	gpib_job_t j = { .op = GPIB_OP_TALK_ONLY, .flags = GPIB_EOI, .buf = (uint8_t *)"HELLO", .len = 5, .timeout_ms = 100 };
	assert(run(&j) == GPIB_OK);
	assert(d->in_len == 5 && !memcmp(d->in, "HELLO", 5) && d->in_eoi[4] && d->ncmds == 0);
}

static int s_traced;
static void count_trace(const gpib_job_t *j) { (void)j; s_traced++; }

static void test_trace_hook(void)
{
	setup();
	sim_add(5, -1);
	gpib_set_trace(count_trace);
	gpib_job_t j = { .op = GPIB_OP_GET, .addr = 5, .timeout_ms = 100 };
	run(&j);
	gpib_set_trace(NULL);
	assert(s_traced == 1);
}

static void test_connected(void)
{
	setup();
	sim_add(5, -1);
	assert(gpib_connected());
	sim_powered = false;
	assert(!gpib_connected());
}

int main(void)
{
	test_write();
	test_write_in_chunks_addresses_once();
	test_read();
	test_read_term_char();
	test_read_in_chunks();
	test_secondary_address();
	test_two_instruments();
	test_serial_poll();
	test_addressed_commands();
	test_no_listener();
	test_read_timeout_recovers();
	test_abort();
	test_spoll_cuts_into_waiting_read();
	test_spoll_cuts_into_write();
	test_owner_lock();
	test_abort_after_partial_write_releases_bus();
	test_spoll_into_soft_read_keeps_it_soft();
	test_recovery_keeps_ren();
	test_find_device();
	test_soft_timeout_ends_read();
	test_soft_timeout_is_per_byte();
	test_ignore_eoi_returns_at_eoi();
	test_scan();
	test_listen_only();
	test_talk_only();
	test_trace_hook();
	test_connected();
	puts("gpib bus: all tests passed");
	return 0;
}
