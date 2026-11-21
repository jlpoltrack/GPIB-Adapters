/* VXI-11 (VXIbus Consortium, VXI-11 rev 1.0) over ONC RPC v2 (RFC 5531), TCP record marking. One call per
 * connection at a time: a complete record is served, and the next one is only read once its reply has gone out.
 * Calls that need the bus wait their turn (one VXI-11 job on the bus at a time), then reply from vxi_task(). */

#include <string.h>
#include "vxi11.h"
#include "xdr.h"
#include "gpib_bus.h"
#include "instr.h"
#include "board.h"

/* ONC RPC */
#define RPC_CALL         0u
#define RPC_REPLY        1u
#define MSG_ACCEPTED     0u
#define MSG_DENIED       1u
#define RPC_MISMATCH     0u
#define ACCEPT_SUCCESS   0u
#define PROG_UNAVAIL     1u
#define PROG_MISMATCH    2u
#define PROC_UNAVAIL     3u
#define GARBAGE_ARGS     4u
#define AUTH_MAX         400u

#define PMAP_PROG        100000u
#define PMAP_VERS        2u
#define PMAP_NULL        0u
#define PMAP_GETPORT     3u
#define IPPROTO_TCP      6u

#define CORE_PROG        0x0607AFu
#define ABORT_PROG       0x0607B0u
#define VXI_VERS         1u

enum {
	CREATE_LINK = 10, DEVICE_WRITE, DEVICE_READ, DEVICE_READSTB, DEVICE_TRIGGER, DEVICE_CLEAR, DEVICE_REMOTE,
	DEVICE_LOCAL, DEVICE_LOCK, DEVICE_UNLOCK, DEVICE_ENABLE_SRQ, DEVICE_DOCMD = 22, DESTROY_LINK,
	CREATE_INTR_CHAN = 25, DESTROY_INTR_CHAN,
};
#define DEVICE_ABORT     1u

enum {
	ERR_NONE = 0, ERR_SYNTAX = 1, ERR_NOT_ACCESSIBLE = 3, ERR_INVALID_LINK = 4, ERR_PARAMETER = 5,
	ERR_NOT_SUPPORTED = 8, ERR_RESOURCES = 9, ERR_LOCKED = 11, ERR_NO_LOCK = 12, ERR_IO_TIMEOUT = 15, ERR_IO = 17,
	ERR_ABORT = 23,
};

#define FLAG_WAITLOCK    0x01u
#define FLAG_END         0x08u
#define FLAG_TERMCHRSET  0x80u
#define REASON_REQCNT    1u
#define REASON_CHR       2u
#define REASON_END       4u

#define DEVICE_NAME_MAX  64u
#define FOREVER          0x7FFFFFFFu /* io_timeout from here on: no timeout */

#define RX_MAX        (VXI_MAX_DATA + 2u * AUTH_MAX + 128u)
#define REPLY_HDR     28u                /* record mark, xid, REPLY, MSG_ACCEPTED, verifier (2 words), SUCCESS */
#define READ_DATA_OFS (REPLY_HDR + 12u)  /* + error, reason, data length */
#define TX_MAX        (READ_DATA_OFS + VXI_MAX_DATA + 4u)

typedef struct {
	bool        used, locked;
	uint8_t     addr;
	uint32_t    id;
	vxi_conn_t *conn;
} link_t;

typedef enum {
	ST_IDLE,      /* no call, or replied */
	ST_WAIT_LOCK, /* another link holds the device lock (waitlock) */
	ST_WAIT_BUS,  /* waiting for the bus */
	ST_WAIT_PRIO, /* serial poll waiting for the priority slot */
	ST_JOB,       /* GPIB job running */
} stage_t;

struct vxi_conn {
	bool     used, failed, ready; /* ready: a complete record waits in rx */
	uint8_t  ch;
	/* record marking */
	uint8_t  mark[4], nmark;
	bool     frag_last;
	uint32_t frag_left;
	uint8_t  rx[RX_MAX];
	uint32_t rx_len;
	uint8_t  tx[TX_MAX];
	uint32_t tx_len, tx_pos;
	/* the call being served */
	stage_t  stage;
	uint32_t xid, proc, flags;
	uint32_t io_deadline, lock_deadline;
	bool     io_forever, aborted;
	link_t  *link;
	const uint8_t *wdata;
	uint32_t wlen, rsize;
	uint8_t  term, stb;
	gpib_job_t job;
};

static vxi_conn_t  s_conns[VXI_MAX_CONNS];
static link_t      s_links[VXI_MAX_LINKS];
static uint32_t    s_next_lid;
static vxi_conn_t *s_bus;         /* connection whose job is on the bus */
static link_t     *s_holder;      /* link that left its instrument addressed (a message split over calls) */
static uint8_t     s_rr;

static bool expired(uint32_t deadline)
{
	return (int32_t)(board_millis() - deadline) >= 0;
}

/* --- replies ---------------------------------------------------------------------------------------- */

/* an accepted reply; with_mark leaves room for the TCP record mark (filled in by end_reply()) */
static xdr_out_t begin_reply(uint8_t *buf, uint32_t max, uint32_t xid, uint32_t stat, bool with_mark)
{
	xdr_out_t o = { .p = buf, .max = max };
	if (with_mark)
		xdr_put_u32(&o, 0);
	xdr_put_u32(&o, xid);
	xdr_put_u32(&o, RPC_REPLY);
	xdr_put_u32(&o, MSG_ACCEPTED);
	xdr_put_u32(&o, 0); /* verifier: AUTH_NONE, empty */
	xdr_put_u32(&o, 0);
	xdr_put_u32(&o, stat);
	return o;
}

/* queues the reply and retires the call and its record */
static void end_reply(vxi_conn_t *c, xdr_out_t *o)
{
	uint32_t n = o->len - 4u;
	c->tx[0] = (uint8_t)(0x80u | (n >> 24)); /* last fragment */
	c->tx[1] = (uint8_t)(n >> 16);
	c->tx[2] = (uint8_t)(n >> 8);
	c->tx[3] = (uint8_t)n;
	c->tx_len = o->len;
	c->tx_pos = 0;
	c->stage = ST_IDLE;
	c->link = NULL;
	c->ready = false;
	c->rx_len = 0;
}

static void reply_status(vxi_conn_t *c, uint32_t stat)
{
	xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, stat, true);
	if (stat == PROG_MISMATCH) {
		xdr_put_u32(&o, (c->ch == VXI_CH_PORTMAP) ? PMAP_VERS : VXI_VERS);
		xdr_put_u32(&o, (c->ch == VXI_CH_PORTMAP) ? PMAP_VERS : VXI_VERS);
	}
	end_reply(c, &o);
}

/* the result of c->proc, in its own layout; a read's data is already in place at READ_DATA_OFS */
static void reply_result(vxi_conn_t *c, uint32_t err, uint32_t n, uint32_t reason)
{
	xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, ACCEPT_SUCCESS, true);
	xdr_put_u32(&o, err);
	switch (c->proc) {
	case DEVICE_WRITE:
		xdr_put_u32(&o, n);
		break;
	case DEVICE_READ:
		xdr_put_u32(&o, reason);
		xdr_put_u32(&o, n);
		memset(&c->tx[READ_DATA_OFS + n], 0, xdr_pad(n) - n);
		o.len = READ_DATA_OFS + xdr_pad(n);
		break;
	case DEVICE_READSTB:
		xdr_put_u32(&o, c->stb);
		break;
	case DEVICE_DOCMD:
		xdr_put_u32(&o, 0); /* empty data_out */
		break;
	default:
		break;
	}
	end_reply(c, &o);
}

/* --- links and locks -------------------------------------------------------------------------------- */

static link_t *find_link(uint32_t id)
{
	for (unsigned i = 0; i < VXI_MAX_LINKS; i++)
		if (s_links[i].used && (s_links[i].id == id))
			return &s_links[i];
	return NULL;
}

static bool lock_conflict(const link_t *l)
{
	for (unsigned i = 0; i < VXI_MAX_LINKS; i++) {
		const link_t *o = &s_links[i];
		if (o->used && o->locked && (o != l) && (o->addr == l->addr))
			return true;
	}
	return false;
}

static void destroy_link(link_t *l)
{
	if (l == s_holder) { /* its message ends here: gpib_bus unaddresses the instrument */
		s_holder = NULL;
		gpib_abort(&l->conn->job);
	}
	l->used = false;
}

/* "inst0" is the instrument auto-ID found; "gpib0,PAD[,SAD]" (also hpib) any address, SAD 0-6 or 96-102 */
static bool parse_device(const uint8_t *s, uint32_t n, uint8_t *addr)
{
	char b[DEVICE_NAME_MAX + 1];
	unsigned v[2] = { 0, 0 }, nv = 0;
	uint32_t i = 4;

	for (uint32_t k = 0; k < n; k++)
		b[k] = (char)(((s[k] >= 'A') && (s[k] <= 'Z')) ? (s[k] + 32) : s[k]);
	b[n] = '\0';

	if (!strcmp(b, "inst0")) {
		*addr = gpib_addr;
		return true;
	}
	if ((n < 6) || (strncmp(b, "gpib", 4) && strncmp(b, "hpib", 4)))
		return false;
	while ((i < n) && (b[i] >= '0') && (b[i] <= '9')) /* interface number */
		i++;
	while ((i < n) && (b[i] == ',') && (nv < 2)) {
		bool digits = false;
		for (i++; (i < n) && (b[i] >= '0') && (b[i] <= '9') && (v[nv] < 1000u); i++, digits = true)
			v[nv] = v[nv] * 10u + (unsigned)(b[i] - '0');
		if (!digits)
			return false;
		nv++;
	}
	if ((i != n) || (nv == 0) || (v[0] > 30u))
		return false;
	if (nv == 2) {
		if ((v[1] >= 96u) && (v[1] <= 102u))
			v[1] -= 96u;
		if (v[1] > 6u)
			return false;
		v[0] |= (v[1] + 1u) << 5;
	}
	*addr = (uint8_t)v[0];
	return true;
}

/* --- core channel ----------------------------------------------------------------------------------- */

static void create_link(vxi_conn_t *c, xdr_in_t *in)
{
	(void)xdr_u32(in); /* clientId */
	bool lock = xdr_u32(in) != 0;
	(void)xdr_u32(in); /* lock_timeout: a lock held elsewhere fails at once */
	uint32_t n;
	const uint8_t *name = xdr_opaque(in, &n, DEVICE_NAME_MAX);
	if (in->err) {
		reply_status(c, GARBAGE_ARGS);
		return;
	}

	uint32_t err = ERR_NONE, lid = 0;
	uint8_t addr;
	link_t *l = NULL;
	if (!parse_device(name, n, &addr)) {
		err = ERR_NOT_ACCESSIBLE;
	} else {
		for (unsigned i = 0; (i < VXI_MAX_LINKS) && !l; i++)
			if (!s_links[i].used)
				l = &s_links[i];
		if (!l) {
			err = ERR_RESOURCES;
		} else {
			*l = (link_t){ .used = true, .addr = addr, .id = ++s_next_lid, .conn = c };
			if (lock && lock_conflict(l)) {
				l->used = false;
				err = ERR_LOCKED;
			} else {
				l->locked = lock;
				lid = l->id;
			}
		}
	}

	xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, ACCEPT_SUCCESS, true);
	xdr_put_u32(&o, err);
	xdr_put_u32(&o, lid);
	xdr_put_u32(&o, VXI_ABORT_PORT);
	xdr_put_u32(&o, VXI_MAX_DATA);
	end_reply(c, &o);
}

static void start_call(vxi_conn_t *c);

/* a call on a link: checks the lock, then runs (or waits for the bus) */
static void link_call(vxi_conn_t *c, uint32_t lid, uint32_t flags, uint32_t lock_timeout, uint32_t io_timeout)
{
	link_t *l = find_link(lid);
	if (!l || (l->conn != c)) {
		reply_result(c, ERR_INVALID_LINK, 0, 0);
		return;
	}
	c->link = l;
	c->flags = flags;
	c->aborted = false;
	c->io_forever = io_timeout >= FOREVER;
	c->io_deadline = board_millis() + (c->io_forever ? 0 : io_timeout);
	if (lock_conflict(l)) {
		if (!(flags & FLAG_WAITLOCK) || !lock_timeout) {
			reply_result(c, ERR_LOCKED, 0, 0);
			return;
		}
		c->lock_deadline = board_millis() + lock_timeout;
		c->stage = ST_WAIT_LOCK;
		return;
	}
	start_call(c);
}

static void start_call(vxi_conn_t *c)
{
	switch (c->proc) {
	case DEVICE_REMOTE:
		gpib_ren(true);
		reply_result(c, ERR_NONE, 0, 0);
		break;
	case DEVICE_LOCK:
		c->link->locked = true;
		reply_result(c, ERR_NONE, 0, 0);
		break;
	case DEVICE_READSTB:
		c->stage = ST_WAIT_PRIO;
		break;
	default:
		c->stage = ST_WAIT_BUS;
		break;
	}
}

static void core_call(vxi_conn_t *c, xdr_in_t *in)
{
	uint32_t lid, flags, lock_tmo, io_tmo;

	switch (c->proc) {
	case CREATE_LINK:
		create_link(c, in);
		return;
	case DEVICE_WRITE:
		lid = xdr_u32(in);
		io_tmo = xdr_u32(in);
		lock_tmo = xdr_u32(in);
		flags = xdr_u32(in);
		c->wdata = xdr_opaque(in, &c->wlen, VXI_MAX_DATA);
		break;
	case DEVICE_READ:
		lid = xdr_u32(in);
		c->rsize = xdr_u32(in);
		io_tmo = xdr_u32(in);
		lock_tmo = xdr_u32(in);
		flags = xdr_u32(in);
		c->term = (uint8_t)xdr_u32(in);
		break;
	case DEVICE_READSTB:
	case DEVICE_TRIGGER:
	case DEVICE_CLEAR:
	case DEVICE_REMOTE:
	case DEVICE_LOCAL:
		lid = xdr_u32(in);
		flags = xdr_u32(in);
		lock_tmo = xdr_u32(in);
		io_tmo = xdr_u32(in);
		break;
	case DEVICE_LOCK:
		lid = xdr_u32(in);
		flags = xdr_u32(in);
		lock_tmo = xdr_u32(in);
		io_tmo = FOREVER;
		break;
	case DEVICE_UNLOCK:
	case DESTROY_LINK: {
		link_t *l = find_link(xdr_u32(in));
		if (in->err) {
			reply_status(c, GARBAGE_ARGS);
		} else if (!l || (l->conn != c)) {
			reply_result(c, ERR_INVALID_LINK, 0, 0);
		} else if (c->proc == DESTROY_LINK) {
			destroy_link(l);
			reply_result(c, ERR_NONE, 0, 0);
		} else {
			reply_result(c, l->locked ? ERR_NONE : ERR_NO_LOCK, 0, 0);
			l->locked = false;
		}
		return;
	}
	case DEVICE_ENABLE_SRQ: /* no interrupt channel: VISA polls the status byte instead */
	case DEVICE_DOCMD:
	case CREATE_INTR_CHAN:
	case DESTROY_INTR_CHAN:
		reply_result(c, ERR_NOT_SUPPORTED, 0, 0);
		return;
	default:
		reply_status(c, PROC_UNAVAIL);
		return;
	}
	if (in->err) {
		reply_status(c, GARBAGE_ARGS);
		return;
	}
	link_call(c, lid, flags, lock_tmo, io_tmo);
}

/* --- abort channel ---------------------------------------------------------------------------------- */

static void abort_call(vxi_conn_t *c, xdr_in_t *in)
{
	if (c->proc == 0) { /* NULL procedure */
		xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, ACCEPT_SUCCESS, true);
		end_reply(c, &o);
		return;
	}
	if (c->proc != DEVICE_ABORT) {
		reply_status(c, PROC_UNAVAIL);
		return;
	}
	link_t *l = find_link(xdr_u32(in));
	if (in->err) {
		reply_status(c, GARBAGE_ARGS);
		return;
	}
	if (l) {
		vxi_conn_t *t = l->conn;
		if ((t->link == l) && (t->stage != ST_IDLE)) {
			t->aborted = true;
			if (t->stage == ST_JOB)
				gpib_abort(&t->job); /* vxi_task() replies once the bus let go */
			else
				reply_result(t, ERR_ABORT, 0, 0);
		}
	}
	xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, ACCEPT_SUCCESS, true);
	xdr_put_u32(&o, l ? ERR_NONE : ERR_INVALID_LINK);
	end_reply(c, &o);
}

/* --- portmapper ------------------------------------------------------------------------------------- */

static uint32_t getport(xdr_in_t *in)
{
	uint32_t prog = xdr_u32(in), vers = xdr_u32(in), prot = xdr_u32(in);
	(void)xdr_u32(in); /* port: ignored */
	if ((vers != VXI_VERS) || (prot != IPPROTO_TCP))
		return 0;
	return (prog == CORE_PROG) ? VXI_CORE_PORT : (prog == ABORT_PROG) ? VXI_ABORT_PORT : 0;
}

/* --- calls ------------------------------------------------------------------------------------------ */

typedef struct {
	uint32_t xid, rpcvers, prog, vers, proc;
} call_t;

/* false: not an RPC call at all */
static bool parse_call(xdr_in_t *in, call_t *k)
{
	uint32_t n;

	k->xid = xdr_u32(in);
	if (xdr_u32(in) != RPC_CALL)
		return false;
	k->rpcvers = xdr_u32(in);
	k->prog = xdr_u32(in);
	k->vers = xdr_u32(in);
	k->proc = xdr_u32(in);
	(void)xdr_u32(in); /* credential and verifier: accepted whatever they are */
	(void)xdr_opaque(in, &n, AUTH_MAX);
	(void)xdr_u32(in);
	(void)xdr_opaque(in, &n, AUTH_MAX);
	return !in->err;
}

static void denied(xdr_out_t *o, uint32_t xid)
{
	xdr_put_u32(o, xid);
	xdr_put_u32(o, RPC_REPLY);
	xdr_put_u32(o, MSG_DENIED);
	xdr_put_u32(o, RPC_MISMATCH);
	xdr_put_u32(o, 2);
	xdr_put_u32(o, 2);
}

static void serve(vxi_conn_t *c)
{
	static const uint32_t progs[] = { [VXI_CH_PORTMAP] = PMAP_PROG, [VXI_CH_CORE] = CORE_PROG, [VXI_CH_ABORT] = ABORT_PROG };
	xdr_in_t in = { .p = c->rx, .len = c->rx_len };
	call_t k;

	if (!parse_call(&in, &k)) {
		c->failed = true;
		return;
	}
	c->xid = k.xid;
	c->proc = k.proc;
	if (k.rpcvers != 2) {
		xdr_out_t o = { .p = c->tx, .max = TX_MAX };
		xdr_put_u32(&o, 0);
		denied(&o, k.xid);
		end_reply(c, &o);
	} else if (k.prog != progs[c->ch]) {
		reply_status(c, PROG_UNAVAIL);
	} else if (k.vers != ((c->ch == VXI_CH_PORTMAP) ? PMAP_VERS : VXI_VERS)) {
		reply_status(c, PROG_MISMATCH);
	} else if (c->ch == VXI_CH_CORE) {
		core_call(c, &in);
	} else if (c->ch == VXI_CH_ABORT) {
		abort_call(c, &in);
	} else if ((k.proc == PMAP_NULL) || (k.proc == PMAP_GETPORT)) {
		uint32_t port = (k.proc == PMAP_GETPORT) ? getport(&in) : 0;
		if (in.err) {
			reply_status(c, GARBAGE_ARGS);
			return;
		}
		xdr_out_t o = begin_reply(c->tx, TX_MAX, c->xid, ACCEPT_SUCCESS, true);
		if (k.proc == PMAP_GETPORT)
			xdr_put_u32(&o, port);
		end_reply(c, &o);
	} else {
		reply_status(c, PROC_UNAVAIL);
	}
}

uint32_t vxi_udp(const uint8_t *req, uint32_t len, uint8_t *resp, uint32_t max)
{
	xdr_in_t in = { .p = req, .len = len };
	call_t k;

	if (!parse_call(&in, &k))
		return 0;
	if (k.rpcvers != 2) {
		xdr_out_t o = { .p = resp, .max = max };
		denied(&o, k.xid);
		return o.err ? 0 : o.len;
	}
	uint32_t stat = (k.prog != PMAP_PROG) ? PROG_UNAVAIL : (k.vers != PMAP_VERS) ? PROG_MISMATCH :
	                ((k.proc != PMAP_NULL) && (k.proc != PMAP_GETPORT)) ? PROC_UNAVAIL : ACCEPT_SUCCESS;
	uint32_t port = ((stat == ACCEPT_SUCCESS) && (k.proc == PMAP_GETPORT)) ? getport(&in) : 0;
	if (in.err)
		stat = GARBAGE_ARGS;
	xdr_out_t o = begin_reply(resp, max, k.xid, stat, false);
	if (stat == PROG_MISMATCH) {
		xdr_put_u32(&o, PMAP_VERS);
		xdr_put_u32(&o, PMAP_VERS);
	} else if ((stat == ACCEPT_SUCCESS) && (k.proc == PMAP_GETPORT)) {
		xdr_put_u32(&o, port);
	}
	return o.err ? 0 : o.len;
}

/* --- GPIB jobs -------------------------------------------------------------------------------------- */

static bool start_job(vxi_conn_t *c)
{
	gpib_job_t *j = &c->job;
	uint32_t left = (c->io_forever || expired(c->io_deadline)) ? 0 : (c->io_deadline - board_millis());

	*j = (gpib_job_t){ .addr = c->link->addr, .timeout_ms = (uint16_t)((left > 65535u) ? 65535u : left) };
	if (!c->io_forever && !j->timeout_ms)
		j->timeout_ms = 1;
	switch (c->proc) {
	case DEVICE_WRITE:
		j->op = GPIB_OP_WRITE;
		j->buf = (uint8_t *)(uintptr_t)c->wdata;
		j->len = (uint16_t)c->wlen;
		j->flags = (c->flags & FLAG_END) ? (GPIB_EOI | GPIB_END) : 0;
		break;
	case DEVICE_READ:
		j->op = GPIB_OP_READ;
		j->buf = &c->tx[READ_DATA_OFS];
		j->len = (uint16_t)((c->rsize < VXI_MAX_DATA) ? c->rsize : VXI_MAX_DATA);
		j->flags = (c->flags & FLAG_TERMCHRSET) ? GPIB_TERM : 0;
		j->term = c->term;
		break;
	case DEVICE_READSTB:
		j->op = GPIB_OP_SPOLL;
		j->buf = &c->stb;
		break;
	case DEVICE_TRIGGER:
		j->op = GPIB_OP_GET;
		break;
	case DEVICE_CLEAR:
		j->op = GPIB_OP_SDC;
		break;
	default: /* DEVICE_LOCAL */
		j->op = GPIB_OP_GTL;
		break;
	}
	gpib_ren(true);
	if (c->proc == DEVICE_READSTB)
		return gpib_submit_priority(j);
	if (!gpib_submit(j, GPIB_OWNER_VXI))
		return false;
	s_bus = c;
	board_led_busy(true);
	return true;
}

static void job_done(vxi_conn_t *c)
{
	gpib_job_t *j = &c->job;
	uint32_t err = (j->status == GPIB_OK) ? ERR_NONE : (j->status == GPIB_TIMEOUT) ? ERR_IO_TIMEOUT :
	               (j->status == GPIB_ABORTED) ? ERR_ABORT : ERR_IO;
	uint32_t reason = 0;

	if (c->proc != DEVICE_READSTB) {
		s_bus = NULL;
		board_led_busy(false);
		s_holder = gpib_holds_bus(GPIB_OWNER_VXI) ? c->link : NULL;
	}
	if (c->aborted)
		err = ERR_ABORT;
	if (c->proc == DEVICE_READ) {
		if (j->done == c->rsize)
			reason |= REASON_REQCNT;
		if (j->end && (j->flags & GPIB_TERM) && j->done && (j->buf[j->done - 1] == j->term))
			reason |= REASON_CHR;
		if (j->end && j->eoi)
			reason |= REASON_END;
	}
	if (c->proc == DEVICE_READSTB && err)
		c->stb = 0;
	reply_result(c, err, j->done, reason);
}

/* the next connection waiting for the bus, round robin; a holder's own link goes first */
static void grant_bus(void)
{
	if (s_bus)
		return;
	for (unsigned k = 0; k < VXI_MAX_CONNS; k++) {
		vxi_conn_t *c = &s_conns[(s_rr + k) % VXI_MAX_CONNS];
		if (c->used && (c->stage == ST_WAIT_BUS) && (!s_holder || (s_holder == c->link))) {
			if (start_job(c)) {
				c->stage = ST_JOB;
				s_rr = (uint8_t)((s_rr + k + 1) % VXI_MAX_CONNS);
			}
			return;
		}
	}
}

/* --- connections ------------------------------------------------------------------------------------ */

static void pump(vxi_conn_t *c)
{
	if (c->ready && (c->stage == ST_IDLE) && !c->tx_len && !c->failed)
		serve(c);
}

void vxi_init(void)
{
	memset(s_conns, 0, sizeof(s_conns));
	memset(s_links, 0, sizeof(s_links));
	s_bus = NULL;
	s_holder = NULL;
}

vxi_conn_t *vxi_open(vxi_channel_t ch)
{
	for (unsigned i = 0; i < VXI_MAX_CONNS; i++) {
		vxi_conn_t *c = &s_conns[i];
		if (!c->used) {
			memset(c, 0, sizeof(*c));
			c->used = true;
			c->ch = (uint8_t)ch;
			return c;
		}
	}
	return NULL;
}

uint32_t vxi_rx(vxi_conn_t *c, const uint8_t *data, uint32_t len)
{
	uint32_t used = 0;

	while ((used < len) && !c->ready && !c->failed) {
		if (c->nmark < 4) {
			c->mark[c->nmark++] = data[used++];
			if (c->nmark == 4) {
				uint32_t v = ((uint32_t)c->mark[0] << 24) | ((uint32_t)c->mark[1] << 16) |
				             ((uint32_t)c->mark[2] << 8) | c->mark[3];
				c->frag_last = (v >> 31) != 0;
				c->frag_left = v & 0x7FFFFFFFu;
				if (c->frag_left > RX_MAX - c->rx_len)
					c->failed = true;
			}
		} else {
			uint32_t n = len - used;
			if (n > c->frag_left)
				n = c->frag_left;
			memcpy(&c->rx[c->rx_len], &data[used], n);
			c->rx_len += n;
			c->frag_left -= n;
			used += n;
		}
		if ((c->nmark == 4) && !c->frag_left && !c->failed) {
			c->nmark = 0;
			c->ready = c->frag_last;
		}
	}
	pump(c);
	return used;
}

const uint8_t *vxi_tx(vxi_conn_t *c, uint32_t *len)
{
	*len = c->tx_len - c->tx_pos;
	return *len ? &c->tx[c->tx_pos] : NULL;
}

void vxi_tx_done(vxi_conn_t *c, uint32_t n)
{
	c->tx_pos += n;
	if (c->tx_pos >= c->tx_len) {
		c->tx_len = c->tx_pos = 0;
		pump(c);
	}
}

bool vxi_failed(const vxi_conn_t *c)
{
	return c->failed;
}

void vxi_close(vxi_conn_t *c)
{
	if (c->stage == ST_JOB) {
		gpib_abort(&c->job);
		if (s_bus == c) {
			s_bus = NULL;
			board_led_busy(false);
		}
	}
	for (unsigned i = 0; i < VXI_MAX_LINKS; i++)
		if (s_links[i].used && (s_links[i].conn == c))
			destroy_link(&s_links[i]);
	c->used = false;
}

void vxi_task(void)
{
	for (unsigned i = 0; i < VXI_MAX_CONNS; i++) {
		vxi_conn_t *c = &s_conns[i];
		if (!c->used)
			continue;
		switch (c->stage) {
		case ST_WAIT_LOCK:
			if (!lock_conflict(c->link))
				start_call(c);
			else if (expired(c->lock_deadline))
				reply_result(c, ERR_LOCKED, 0, 0);
			break;
		case ST_WAIT_BUS:
		case ST_WAIT_PRIO:
			if (!c->io_forever && expired(c->io_deadline))
				reply_result(c, ERR_IO_TIMEOUT, 0, 0);
			else if ((c->stage == ST_WAIT_PRIO) && start_job(c))
				c->stage = ST_JOB;
			break;
		case ST_JOB:
			if (c->job.status != GPIB_PENDING)
				job_done(c);
			break;
		case ST_IDLE:
			break;
		}
		pump(c);
	}
	grant_bus();
}
