/* Host tests for the VXI-11 server: hand-built RPC records against simulated instruments (make test) */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vxi11.h"
#include "xdr.h"
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "board.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t gpib_addr = 5;
void board_led_busy(bool busy) { (void)busy; }

/* --- RPC client ----------------------------------------------------------------------------------------- */

#define CORE  0x0607AFu
#define ABORT 0x0607B0u
#define PMAP  100000u

enum { CREATE_LINK = 10, WRITE, READ, READSTB, TRIGGER, CLEAR, REMOTE, LOCAL, LOCK, UNLOCK, ENABLE_SRQ,
       DOCMD = 22, DESTROY_LINK, CREATE_INTR_CHAN = 25 };

static uint8_t  s_req[4096], s_rep[4096];
static uint32_t s_xid = 100;

static xdr_out_t call(uint32_t prog, uint32_t vers, uint32_t proc)
{
	xdr_out_t o = { .p = s_req, .max = sizeof(s_req) };
	xdr_put_u32(&o, 0); /* record mark */
	xdr_put_u32(&o, ++s_xid);
	xdr_put_u32(&o, 0); /* CALL */
	xdr_put_u32(&o, 2);
	xdr_put_u32(&o, prog);
	xdr_put_u32(&o, vers);
	xdr_put_u32(&o, proc);
	for (int i = 0; i < 4; i++) /* AUTH_NONE credential and verifier */
		xdr_put_u32(&o, 0);
	return o;
}

/* the xid of each connection's outstanding call */
static struct { vxi_conn_t *c; uint32_t xid; } s_sent[VXI_MAX_CONNS];

static void sent(vxi_conn_t *c, uint32_t xid)
{
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		if ((s_sent[i].c == c) || !s_sent[i].c || (i == VXI_MAX_CONNS - 1)) {
			s_sent[i].c = c;
			s_sent[i].xid = xid;
			return;
		}
}

static uint32_t sent_xid(vxi_conn_t *c)
{
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		if (s_sent[i].c == c)
			return s_sent[i].xid;
	return 0;
}

static void spin(int n)
{
	for (int i = 0; i < n; i++) {
		vxi_task();
		gpib_task();
	}
}

/* sends the record, in chunks of `chunk` bytes (0: all at once) */
static void send_chunked(vxi_conn_t *c, xdr_out_t *o, uint32_t chunk)
{
	uint32_t n = o->len - 4;
	s_req[0] = (uint8_t)(0x80 | (n >> 24));
	s_req[1] = (uint8_t)(n >> 16);
	s_req[2] = (uint8_t)(n >> 8);
	s_req[3] = (uint8_t)n;
	sent(c, ((uint32_t)s_req[4] << 24) | ((uint32_t)s_req[5] << 16) | ((uint32_t)s_req[6] << 8) | s_req[7]);
	for (uint32_t pos = 0; pos < o->len;) {
		uint32_t len = (chunk && (o->len - pos > chunk)) ? chunk : o->len - pos;
		uint32_t used = vxi_rx(c, &s_req[pos], len);
		assert(used == len);
		pos += used;
	}
}

static void send(vxi_conn_t *c, xdr_out_t *o)
{
	send_chunked(c, o, 0);
}

static bool reply_ready(vxi_conn_t *c)
{
	uint32_t n;
	return vxi_tx(c, &n) != NULL;
}

/* waits for the reply to the last call, checks its header; returns the results with the accept status in *stat */
static xdr_in_t reply_stat(vxi_conn_t *c, uint32_t *stat)
{
	uint32_t n = 0, got = 0;
	for (int i = 0; (i < 5000000) && !reply_ready(c); i++)
		spin(1);
	const uint8_t *p = vxi_tx(c, &n);
	assert(p && n <= sizeof(s_rep));
	while (p) { /* take it in two pieces, like a short send */
		uint32_t take = (n > 16) ? n / 2 : n;
		memcpy(&s_rep[got], p, take);
		got += take;
		vxi_tx_done(c, take);
		p = vxi_tx(c, &n);
	}
	xdr_in_t in = { .p = s_rep, .len = got };
	uint32_t mark = xdr_u32(&in);
	assert(mark == (0x80000000u | (got - 4)));
	assert(xdr_u32(&in) == sent_xid(c));
	assert(xdr_u32(&in) == 1); /* REPLY */
	assert(xdr_u32(&in) == 0); /* MSG_ACCEPTED */
	assert(xdr_u32(&in) == 0 && xdr_u32(&in) == 0);
	*stat = xdr_u32(&in);
	return in;
}

static xdr_in_t reply(vxi_conn_t *c)
{
	uint32_t stat;
	xdr_in_t in = reply_stat(c, &stat);
	assert(stat == 0);
	return in;
}

static uint32_t create_link(vxi_conn_t *c, const char *dev, bool lock, uint32_t *err)
{
	xdr_out_t o = call(CORE, 1, CREATE_LINK);
	xdr_put_u32(&o, 1234);
	xdr_put_u32(&o, lock);
	xdr_put_u32(&o, 0);
	xdr_put_opaque(&o, dev, (uint32_t)strlen(dev));
	send(c, &o);
	xdr_in_t in = reply(c);
	*err = xdr_u32(&in);
	uint32_t lid = xdr_u32(&in);
	assert(xdr_u32(&in) == VXI_ABORT_PORT);
	assert(xdr_u32(&in) == VXI_MAX_DATA);
	assert(!in.err && in.pos == in.len);
	return lid;
}

static uint32_t link_ok(vxi_conn_t *c, const char *dev)
{
	uint32_t err;
	uint32_t lid = create_link(c, dev, false, &err);
	assert(err == 0 && lid);
	return lid;
}

static void send_write(vxi_conn_t *c, uint32_t lid, const char *s, uint32_t flags, uint32_t lock_tmo)
{
	xdr_out_t o = call(CORE, 1, WRITE);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, 2000);
	xdr_put_u32(&o, lock_tmo);
	xdr_put_u32(&o, flags);
	xdr_put_opaque(&o, s, (uint32_t)strlen(s));
	send(c, &o);
}

static uint32_t write_flags(vxi_conn_t *c, uint32_t lid, const char *s, uint32_t flags)
{
	send_write(c, lid, s, flags, 0);
	xdr_in_t in = reply(c);
	uint32_t err = xdr_u32(&in);
	uint32_t size = xdr_u32(&in);
	assert(err || (size == strlen(s)));
	return err;
}

static uint32_t write_end(vxi_conn_t *c, uint32_t lid, const char *s)
{
	return write_flags(c, lid, s, 8);
}

static void send_read(vxi_conn_t *c, uint32_t lid, uint32_t size, uint32_t io_tmo, int term)
{
	xdr_out_t o = call(CORE, 1, READ);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, size);
	xdr_put_u32(&o, io_tmo);
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, (term >= 0) ? 0x80 : 0);
	xdr_put_u32(&o, (term >= 0) ? (uint32_t)term : 0);
	send(c, &o);
}

/* returns the error; data (NUL-terminated) and reason out */
static uint32_t read_reply(vxi_conn_t *c, char *data, uint32_t *reason)
{
	xdr_in_t in = reply(c);
	uint32_t err = xdr_u32(&in), n;
	*reason = xdr_u32(&in);
	const uint8_t *p = xdr_opaque(&in, &n, 4096);
	assert(!in.err && in.pos == in.len);
	memcpy(data, p, n);
	data[n] = '\0';
	return err;
}

static uint32_t generic(vxi_conn_t *c, uint32_t proc, uint32_t lid, uint32_t *extra)
{
	xdr_out_t o = call(CORE, 1, proc);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, 2000);
	send(c, &o);
	xdr_in_t in = reply(c);
	uint32_t err = xdr_u32(&in);
	if (extra)
		*extra = xdr_u32(&in);
	assert(!in.err && in.pos == in.len);
	return err;
}

static uint32_t simple(vxi_conn_t *c, uint32_t proc, uint32_t lid)
{
	xdr_out_t o = call(CORE, 1, proc);
	xdr_put_u32(&o, lid);
	send(c, &o);
	xdr_in_t in = reply(c);
	return xdr_u32(&in);
}

static uint32_t lock(vxi_conn_t *c, uint32_t lid, uint32_t flags, uint32_t lock_tmo)
{
	xdr_out_t o = call(CORE, 1, LOCK);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, flags);
	xdr_put_u32(&o, lock_tmo);
	send(c, &o);
	xdr_in_t in = reply(c);
	return xdr_u32(&in);
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

static void setup(void)
{
	memset(s_sent, 0, sizeof(s_sent));
	sim_reset();
	gpib_init();
	vxi_init();
}

/* --- tests -------------------------------------------------------------------------------------------- */

static void test_portmap(void)
{
	setup();
	vxi_conn_t *c = vxi_open(VXI_CH_PORTMAP);
	uint32_t progs[] = { CORE, ABORT, 0x0607B1u }, ports[] = { VXI_CORE_PORT, VXI_ABORT_PORT, 0 };
	for (int i = 0; i < 3; i++) {
		xdr_out_t o = call(PMAP, 2, 3);
		xdr_put_u32(&o, progs[i]);
		xdr_put_u32(&o, 1);
		xdr_put_u32(&o, 6);
		xdr_put_u32(&o, 0);
		send(c, &o);
		xdr_in_t in = reply(c);
		assert(xdr_u32(&in) == ports[i] && in.pos == in.len);
	}
	xdr_out_t o = call(PMAP, 2, 0); /* NULL */
	send(c, &o);
	xdr_in_t in = reply(c);
	assert(in.pos == in.len);

	uint32_t stat;
	o = call(PMAP, 3, 3); /* wrong version */
	send(c, &o);
	in = reply_stat(c, &stat);
	assert(stat == 2 && xdr_u32(&in) == 2 && xdr_u32(&in) == 2);
	o = call(CORE, 1, CREATE_LINK); /* wrong program for this port */
	send(c, &o);
	reply_stat(c, &stat);
	assert(stat == 1);
	o = call(PMAP, 2, 4); /* DUMP: not served */
	send(c, &o);
	reply_stat(c, &stat);
	assert(stat == 3);
	vxi_close(c);
}

static void test_portmap_udp(void)
{
	setup();
	xdr_out_t o = call(PMAP, 2, 3);
	xdr_put_u32(&o, CORE);
	xdr_put_u32(&o, 1);
	xdr_put_u32(&o, 6);
	xdr_put_u32(&o, 0);
	uint32_t n = vxi_udp(&s_req[4], o.len - 4, s_rep, sizeof(s_rep));
	xdr_in_t in = { .p = s_rep, .len = n };
	assert(xdr_u32(&in) == s_xid && xdr_u32(&in) == 1 && xdr_u32(&in) == 0);
	xdr_u32(&in);
	xdr_u32(&in);
	assert(xdr_u32(&in) == 0 && xdr_u32(&in) == VXI_CORE_PORT && in.pos == in.len);

	o = call(CORE, 1, 3);
	n = vxi_udp(&s_req[4], o.len - 4, s_rep, sizeof(s_rep));
	in = (xdr_in_t){ .p = s_rep, .len = n };
	for (int i = 0; i < 5; i++)
		xdr_u32(&in);
	assert(xdr_u32(&in) == 1); /* PROG_UNAVAIL */
	assert(vxi_udp(s_req, 7, s_rep, sizeof(s_rep)) == 0);
}

static void test_write_read(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	vxi_conn_t *c = vxi_open(VXI_CH_CORE);
	uint32_t lid = link_ok(c, "inst0");

	assert(write_end(c, lid, "*IDN?\n") == 0);
	assert(dev_got(d, "*IDN?\n") && d->in_eoi[5] && d->messages == 1);

	char data[2048];
	uint32_t reason;
	send_read(c, lid, 1024, 2000, -1);
	assert(read_reply(c, data, &reason) == 0);
	assert(!strcmp(data, "ACME,M1,42,1.0\n") && reason == 4);

	/* term character, no EOI */
	sim_answer(d, "1.23\nXYZ", false);
	send_read(c, lid, 1024, 2000, '\n');
	assert(read_reply(c, data, &reason) == 0 && !strcmp(data, "1.23\n") && reason == 2);

	/* request count: the rest comes with the next read */
	sim_answer(d, "ABCDEFGH", true);
	send_read(c, lid, 4, 2000, -1);
	assert(read_reply(c, data, &reason) == 0 && !strcmp(data, "ABCD") && reason == 1);
	send_read(c, lid, 100, 2000, -1);
	assert(read_reply(c, data, &reason) == 0 && !strcmp(data, "EFGH") && reason == 4);

	/* split write: EOI only on the last byte */
	d->in_len = 0;
	d->messages = 0;
	assert(write_flags(c, lid, "AB", 0) == 0);
	assert(write_end(c, lid, "CD\n") == 0);
	assert(dev_got(d, "ABCD\n") && !d->in_eoi[1] && d->in_eoi[4] && d->messages == 1);

	assert(simple(c, DESTROY_LINK, lid) == 0);
	assert(simple(c, DESTROY_LINK, lid) == 4);
	vxi_close(c);
}

static void test_byte_by_byte_and_fragments(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	vxi_conn_t *c = vxi_open(VXI_CH_CORE);
	uint32_t lid = link_ok(c, "inst0");

	xdr_out_t o = call(CORE, 1, WRITE);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, 2000);
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, 8);
	xdr_put_opaque(&o, "HELLO\n", 6);
	send_chunked(c, &o, 1);
	xdr_in_t in = reply(c);
	assert(xdr_u32(&in) == 0 && xdr_u32(&in) == 6 && dev_got(d, "HELLO\n"));

	/* the same call as two fragments */
	d->in_len = 0;
	o = call(CORE, 1, WRITE);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, 2000);
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, 8);
	xdr_put_opaque(&o, "XY\n", 3);
	uint8_t rec[256];
	uint32_t body = o.len - 4, first = 12;
	uint32_t m1 = first, m2 = 0x80000000u | (body - first);
	uint8_t marks[8] = { (uint8_t)(m1 >> 24), (uint8_t)(m1 >> 16), (uint8_t)(m1 >> 8), (uint8_t)m1,
	                     (uint8_t)(m2 >> 24), (uint8_t)(m2 >> 16), (uint8_t)(m2 >> 8), (uint8_t)m2 };
	memcpy(rec, marks, 4);
	memcpy(&rec[4], &s_req[4], first);
	memcpy(&rec[4 + first], &marks[4], 4);
	memcpy(&rec[8 + first], &s_req[4 + first], body - first);
	sent(c, s_xid);
	assert(vxi_rx(c, rec, body + 8) == body + 8);
	in = reply(c);
	assert(xdr_u32(&in) == 0 && dev_got(d, "XY\n"));
	vxi_close(c);
}

static void test_addresses(void)
{
	setup();
	sim_dev_t *a = sim_add(7, -1), *b = sim_add(9, 2);
	vxi_conn_t *c = vxi_open(VXI_CH_CORE);
	uint32_t err;

	uint32_t l1 = link_ok(c, "gpib0,7");
	uint32_t l2 = link_ok(c, "GPIB0,9,98");
	assert(write_end(c, l1, "A\n") == 0 && dev_got(a, "A\n"));
	assert(write_end(c, l2, "B\n") == 0 && dev_got(b, "B\n"));
	assert(simple(c, DESTROY_LINK, l2) == 0);
	l2 = link_ok(c, "hpib,9,2");
	assert(write_end(c, l2, "C\n") == 0 && dev_got(b, "B\nC\n"));

	const char *bad[] = { "inst1", "gpib0", "gpib0,", "gpib0,31", "gpib0,7,7", "gpib0,7x", "com1", "" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		assert(create_link(c, bad[i], false, &err) == 0 && err == 3);
	}
	assert(write_end(c, 999, "X") == 4);

	/* links run out */
	link_ok(c, "inst0");
	link_ok(c, "inst0");
	assert(create_link(c, "inst0", false, &err) == 0 && err == 9);

	/* another connection can't use this one's links */
	vxi_conn_t *c2 = vxi_open(VXI_CH_CORE);
	assert(write_end(c2, l1, "X") == 4);
	vxi_close(c2);
	vxi_close(c);

	/* closing freed them */
	c = vxi_open(VXI_CH_CORE);
	link_ok(c, "inst0");
	vxi_close(c);
}

static void test_control(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	vxi_conn_t *c = vxi_open(VXI_CH_CORE);
	uint32_t lid = link_ok(c, "inst0"), stb;

	d->status = 0x42;
	d->srq = true;
	assert(generic(c, READSTB, lid, &stb) == 0 && stb == 0x42);
	assert(generic(c, TRIGGER, lid, NULL) == 0 && d->get == 1);
	assert(generic(c, CLEAR, lid, NULL) == 0 && d->sdc == 1);
	assert(generic(c, LOCAL, lid, NULL) == 0 && d->gtl == 1);
	assert(generic(c, REMOTE, lid, NULL) == 0);
	assert(generic(c, ENABLE_SRQ, lid, NULL) == 8);

	xdr_out_t o = call(CORE, 1, DOCMD);
	send(c, &o);
	xdr_in_t in = reply(c);
	assert(xdr_u32(&in) == 8 && xdr_u32(&in) == 0 && in.pos == in.len);
	o = call(CORE, 1, 99);
	send(c, &o);
	uint32_t stat;
	reply_stat(c, &stat);
	assert(stat == 3);
	o = call(CORE, 1, WRITE); /* no arguments */
	send(c, &o);
	reply_stat(c, &stat);
	assert(stat == 4);
	vxi_close(c);
}

static void test_locks(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	vxi_conn_t *a = vxi_open(VXI_CH_CORE), *b = vxi_open(VXI_CH_CORE);
	uint32_t la = link_ok(a, "inst0"), lb = link_ok(b, "gpib0,5"), err;

	assert(simple(a, UNLOCK, la) == 12);
	assert(lock(a, la, 0, 0) == 0);
	assert(lock(b, lb, 0, 0) == 11);
	assert(write_end(b, lb, "X\n") == 11);
	assert(create_link(b, "inst0", true, &err) == 0 && err == 11);
	assert(write_end(a, la, "A\n") == 0 && dev_got(d, "A\n"));

	/* waitlock: times out while the lock is held */
	uint32_t t0 = board_millis();
	send_write(b, lb, "B\n", 9, 50);
	xdr_in_t in = reply(b);
	assert(xdr_u32(&in) == 11 && (board_millis() - t0) >= 50);

	/* waitlock: proceeds once it's released */
	send_write(b, lb, "B\n", 9, 10000);
	spin(1000);
	assert(!reply_ready(b));
	assert(simple(a, UNLOCK, la) == 0);
	in = reply(b);
	assert(xdr_u32(&in) == 0 && dev_got(d, "A\nB\n"));

	/* a lock dies with its link */
	assert(lock(a, la, 0, 0) == 0);
	assert(simple(a, DESTROY_LINK, la) == 0);
	assert(lock(b, lb, 0, 0) == 0);
	vxi_close(a);
	vxi_close(b);
}

static void test_timeout_and_abort(void)
{
	setup();
	sim_add(5, -1); /* never talks */
	vxi_conn_t *c = vxi_open(VXI_CH_CORE), *ab = vxi_open(VXI_CH_ABORT);
	uint32_t lid = link_ok(c, "inst0"), reason;
	char data[64];

	send_read(c, lid, 100, 50, -1);
	assert(read_reply(c, data, &reason) == 15 && !data[0]);

	send_read(c, lid, 100, 100000, -1);
	spin(2000);
	assert(!reply_ready(c));
	xdr_out_t o = call(ABORT, 1, 1);
	xdr_put_u32(&o, lid);
	send(ab, &o);
	xdr_in_t in = reply(ab);
	assert(xdr_u32(&in) == 0);
	assert(read_reply(c, data, &reason) == 23);

	o = call(ABORT, 1, 1);
	xdr_put_u32(&o, 999);
	send(ab, &o);
	in = reply(ab);
	assert(xdr_u32(&in) == 4);

	/* a job running when its connection closes: aborted, the bus is free again */
	send_read(c, lid, 100, 100000, -1);
	spin(2000);
	vxi_close(c);
	spin(100);
	c = vxi_open(VXI_CH_CORE);
	lid = link_ok(c, "inst0");
	send_read(c, lid, 100, 20, -1);
	assert(read_reply(c, data, &reason) == 15);
	vxi_close(c);
	vxi_close(ab);
}

static void test_bus_sharing(void)
{
	setup();
	sim_dev_t *a = sim_add(5, -1), *b = sim_add(6, -1);
	vxi_conn_t *ca = vxi_open(VXI_CH_CORE), *cb = vxi_open(VXI_CH_CORE);
	uint32_t la = link_ok(ca, "gpib0,5"), lb = link_ok(cb, "gpib0,6");

	/* A leaves its instrument addressed mid-message: B waits until A's message ends */
	assert(write_flags(ca, la, "AB", 0) == 0);
	send_write(cb, lb, "X\n", 8, 0);
	spin(20000);
	assert(!reply_ready(cb) && b->in_len == 0);
	assert(write_end(ca, la, "C\n") == 0 && dev_got(a, "ABC\n"));
	xdr_in_t in = reply(cb);
	assert(xdr_u32(&in) == 0 && dev_got(b, "X\n"));

	/* a holder that disconnects is unaddressed, and B goes on */
	assert(write_flags(ca, la, "DE", 0) == 0);
	send_write(cb, lb, "Y\n", 8, 0);
	spin(20000);
	assert(!reply_ready(cb));
	vxi_close(ca);
	in = reply(cb);
	assert(xdr_u32(&in) == 0 && dev_got(b, "X\nY\n"));
	assert(!gpib_holds_bus(GPIB_OWNER_VXI));
	vxi_close(cb);
}

static void test_bad_input(void)
{
	setup();
	vxi_conn_t *c = vxi_open(VXI_CH_CORE);
	xdr_out_t o = call(CORE, 1, CREATE_LINK);
	s_req[11] = 1; /* REPLY instead of CALL */
	send(c, &o);
	assert(vxi_failed(c) && !reply_ready(c));
	vxi_close(c);

	c = vxi_open(VXI_CH_CORE);
	uint8_t huge[4] = { 0x80, 0x01, 0x00, 0x00 }; /* 64 KB record */
	vxi_rx(c, huge, 4);
	assert(vxi_failed(c));
	vxi_close(c);

	c = vxi_open(VXI_CH_CORE);
	o = call(CORE, 1, CREATE_LINK);
	s_req[15] = 3; /* RPC version 3 */
	send(c, &o);
	uint32_t n;
	const uint8_t *p = vxi_tx(c, &n);
	xdr_in_t in = { .p = p, .len = n };
	xdr_u32(&in);
	assert(xdr_u32(&in) == s_xid && xdr_u32(&in) == 1 && xdr_u32(&in) == 1 && xdr_u32(&in) == 0);
	vxi_tx_done(c, n);
	vxi_close(c);

	/* every connection taken */
	vxi_conn_t *all[VXI_MAX_CONNS];
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		assert((all[i] = vxi_open(VXI_CH_CORE)) != NULL);
	assert(vxi_open(VXI_CH_CORE) == NULL);
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		vxi_close(all[i]);
}

int main(void)
{
	test_portmap();
	test_portmap_udp();
	test_write_read();
	test_byte_by_byte_and_fragments();
	test_addresses();
	test_control();
	test_locks();
	test_timeout_and_abort();
	test_bus_sharing();
	test_bad_input();
	puts("vxi11: all tests passed");
	return 0;
}
