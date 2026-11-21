#include <stddef.h>
#include "gpib_bus.h"
#include "gpib_phy.h"
#include "board.h"

/* Each job compiles to a short program: command bytes (sent with ATN), addressing steps that are skipped when the
 * bus is already addressed that way, the data phase, and delays. */
#define P_LISTEN        0x100 /* job->addr listens, controller talks */
#define P_TALK          0x101 /* job->addr talks, controller listens */
#define P_DATA          0x102
#define P_STATUS        0x103 /* one serial poll byte */
#define P_UNADDR_IF_END 0x104
#define P_DELAY(ms)     (0x200 | (ms))

#define UNL 0x3F
#define UNT 0x5F
#define LAG 0x20
#define TAG 0x40
#define SCG 0x60
#define SPE 0x18
#define SPD 0x19
#define SDC 0x04
#define GET 0x08
#define GTL 0x01
#define LLO 0x11

#define OWNER_PRIO GPIB_OWNERS
#define OWNER_BUS  (GPIB_OWNERS + 1) /* the bus's own unaddress after an abort */
#define RX_GRACE_MS 1000 /* a soft timeout waits this long for a byte already in its handshake */
#define BUDGET_MS  2 /* per gpib_task() call, so USB keeps being serviced */

enum { A_BUSY, A_MORE, A_DONE, A_NO_LISTENER };

static gpib_job_t *s_queue[GPIB_OWNERS]; /* each owner's job, until it finishes */
static gpib_job_t *s_prio;
static gpib_job_t *s_active;
static gpib_job_t *s_suspended;          /* job a priority job cut into */
static gpib_job_t  s_unaddr;
static int8_t      s_lock = -1;          /* owner with an instrument left addressed */
static uint8_t     s_rr;

static enum { ADDR_NONE, ADDR_WRITE, ADDR_READ } s_mode;
static uint8_t      s_peer;
static uint8_t      s_q[5], s_nq, s_iq;  /* command bytes of the current step */
static enum { IF_NONE, IF_CMD, IF_TX, IF_RX } s_inflight;
static bool         s_delaying;
static uint32_t     s_delay_end;
static gpib_phase_t s_phase;
static gpib_error_t s_error;
static void (*s_trace)(const gpib_job_t *job);

static void add(gpib_job_t *j, uint16_t p)
{
	j->prog[j->nprog++] = p;
}

static void add_addr(gpib_job_t *j, uint8_t group)
{
	add(j, group | (j->addr & 0x1F));
	if (j->addr >> 5)
		add(j, SCG | ((j->addr >> 5) - 1));
}

static void compile(gpib_job_t *j)
{
	j->nprog = 0;
	switch (j->op) {
	case GPIB_OP_WRITE:
		add(j, P_LISTEN);
		add(j, P_DATA);
		if (j->flags & GPIB_END) {
			add(j, UNL);
			add(j, UNT);
		}
		break;
	case GPIB_OP_READ:
		add(j, P_TALK);
		add(j, P_DATA);
		add(j, P_UNADDR_IF_END);
		break;
	case GPIB_OP_SPOLL:
		add(j, UNL);
		add(j, LAG);
		add(j, SPE);
		add_addr(j, TAG);
		add(j, P_STATUS);
		add(j, SPD);
		add(j, UNT);
		break;
	case GPIB_OP_SDC: /* the delays are not in the NI adapter, but instruments with NI chipsets need them */
		add_addr(j, LAG);
		add(j, SDC);
		add(j, UNL);
		add(j, P_DELAY(150));
		break;
	case GPIB_OP_GET:
		add_addr(j, LAG);
		add(j, GET);
		add(j, UNL);
		break;
	case GPIB_OP_GTL:
		add_addr(j, LAG);
		add(j, GTL);
		add(j, P_DELAY(100));
		add(j, UNL);
		break;
	case GPIB_OP_LLO:
		add(j, LLO);
		break;
	case GPIB_OP_UNADDRESS:
		add(j, UNL);
		add(j, UNT);
		break;
	case GPIB_OP_LISTEN_ONLY:
	case GPIB_OP_TALK_ONLY:
		add(j, P_DATA);
		break;
	}
}

static void queue_cmd(uint8_t b)
{
	s_q[s_nq++] = b;
}

static void queue_addr(uint8_t group, uint8_t addr)
{
	queue_cmd(group | (addr & 0x1F));
	if (addr >> 5)
		queue_cmd(SCG | ((addr >> 5) - 1));
}

static uint8_t s_last_owner = GPIB_OWNER_TMC;

uint8_t gpib_last_owner(void)
{
	return s_last_owner;
}

static void recover(void)
{
	phy_init();
	s_mode = ADDR_NONE;
	s_nq = s_iq = 0;
	s_inflight = IF_NONE;
	s_delaying = false;
	s_lock = -1;
}

static void start(gpib_job_t *j)
{
	s_active = j;
	j->pc = 0;
	j->deadline = board_millis() + j->timeout_ms;
	if (j->flags & GPIB_SOFT_TMO) /* the per-byte timeout starts with the data; addressing gets a second */
		j->deadline += 1000;
}

static void pick(void)
{
	if (s_prio) {
		start(s_prio);
		return;
	}
	if (s_lock >= 0) {
		if (s_queue[s_lock])
			start(s_queue[s_lock]);
		return;
	}
	for (uint8_t k = 0; k < GPIB_OWNERS; k++) {
		uint8_t o = (uint8_t)((s_rr + k) % GPIB_OWNERS);
		if (s_queue[o]) {
			s_rr = (uint8_t)(o + 1);
			start(s_queue[o]);
			return;
		}
	}
}

static void release(gpib_job_t *j, gpib_status_t status)
{
	if (j->owner == OWNER_PRIO)
		s_prio = NULL;
	else if (j->owner < GPIB_OWNERS)
		s_queue[j->owner] = NULL;
	j->status = status;
	if (s_trace)
		s_trace(j);
}

static void finish(gpib_job_t *j, gpib_status_t status)
{
	if (status != GPIB_OK) {
		recover();
		s_error = (gpib_error_t){ status, s_phase, j->op, j->addr };
		if (status != GPIB_ABORTED)
			board_led_error();
	} else if (j->owner < GPIB_OWNERS) {
		s_lock = (s_mode != ADDR_NONE) ? (int8_t)j->owner : -1;
	}
	s_nq = s_iq = 0;
	s_inflight = IF_NONE;
	s_delaying = false;
	s_active = NULL;
	if (s_suspended) /* resumes at its addressing step, with a fresh timeout */
		start(s_suspended);
	s_suspended = NULL;
	release(j, status);
	if (!s_active)
		pick();
}

/* A priority job may cut in between the bytes of a write, or while a read waits for its first byte */
static bool preemptable(gpib_job_t *j)
{
	if ((s_iq < s_nq) || s_delaying || (j->pc >= j->nprog) || (j->prog[j->pc] != P_DATA))
		return false;
	if ((j->op == GPIB_OP_LISTEN_ONLY) || (j->op == GPIB_OP_TALK_ONLY)) /* nobody to re-address afterwards */
		return false;
	if (s_inflight == IF_NONE)
		return (j->op == GPIB_OP_WRITE) || (j->done == 0);
	return (s_inflight == IF_RX) && (j->done == 0) && phy_rx_cancel();
}

static void rx_byte(gpib_job_t *j)
{
	uint8_t c;
	bool eoi;

	if (phy_rx_poll(&c, &eoi) == PHY_BUSY)
		return;
	s_inflight = IF_NONE;
	if (j->op == GPIB_OP_SPOLL) {
		j->buf[0] = c;
		j->done = 1;
		j->pc++;
		return;
	}
	j->buf[j->done++] = c;
	j->eoi = eoi;
	if (j->flags & GPIB_SOFT_TMO)
		j->deadline = board_millis() + j->timeout_ms;
	if ((eoi && !(j->flags & GPIB_IGNORE_EOI)) || ((j->flags & GPIB_TERM) && (c == j->term)))
		j->end = true;
}

static int advance(gpib_job_t *j)
{
	phy_status_t st;

	switch (s_inflight) {
	case IF_CMD:
	case IF_TX:
		st = phy_tx_poll();
		if (st == PHY_BUSY)
			return A_BUSY;
		if (st == PHY_NO_LISTENER)
			return A_NO_LISTENER;
		if (s_inflight == IF_CMD)
			s_iq++;
		else
			j->done++;
		s_inflight = IF_NONE;
		return A_MORE;
	case IF_RX:
		rx_byte(j);
		return (s_inflight == IF_RX) ? A_BUSY : A_MORE;
	case IF_NONE:
		break;
	}

	if (s_iq < s_nq) {
		phy_atn(true);
		phy_tx_start(s_q[s_iq], false);
		s_inflight = IF_CMD;
		return A_MORE;
	}
	s_nq = s_iq = 0;
	if (s_delaying) {
		if ((int32_t)(board_millis() - s_delay_end) < 0)
			return A_BUSY;
		s_delaying = false;
	}
	if (j->pc >= j->nprog)
		return A_DONE;

	uint16_t p = j->prog[j->pc];
	if (p < 0x100) {
		queue_cmd((uint8_t)p);
		s_mode = ADDR_NONE;
		s_phase = GPIB_PHASE_COMMAND;
		j->pc++;
		return A_MORE;
	}
	if (p >= 0x200) {
		s_delay_end = board_millis() + (p & 0xFFu) + 1u; /* at least the full delay */
		s_delaying = true;
		j->pc++;
		return A_MORE;
	}

	switch (p) {
	case P_LISTEN: /* v2 make_listener: UNT, controller talks, UNL, target listens */
		j->pc++;
		if ((s_mode != ADDR_WRITE) || (s_peer != j->addr)) {
			queue_cmd(UNT);
			queue_cmd(TAG);
			queue_cmd(UNL);
			queue_addr(LAG, j->addr);
			s_phase = GPIB_PHASE_ADDRESS;
		}
		s_mode = ADDR_WRITE;
		s_peer = j->addr;
		return A_MORE;
	case P_TALK: /* v2 make_talker: UNL, controller listens, target talks */
		j->pc++;
		if ((s_mode != ADDR_READ) || (s_peer != j->addr)) {
			queue_cmd(UNL);
			queue_cmd(LAG);
			queue_addr(TAG, j->addr);
			s_phase = GPIB_PHASE_ADDRESS;
		}
		s_mode = ADDR_READ;
		s_peer = j->addr;
		return A_MORE;
	case P_DATA:
		if ((j->op == GPIB_OP_WRITE) || (j->op == GPIB_OP_TALK_ONLY)) {
			if (j->done >= j->len) {
				j->pc++;
				return A_MORE;
			}
			s_phase = GPIB_PHASE_WRITE;
			phy_atn(false);
			phy_tx_start(j->buf[j->done], (j->flags & GPIB_EOI) && (j->done + 1u == j->len));
			s_inflight = IF_TX;
			return A_MORE;
		}
		if (j->end || j->eoi || (j->done >= j->len)) {
			j->pc++;
			if (j->flags & GPIB_SOFT_TMO) /* unaddressing gets a second, like addressing */
				j->deadline = board_millis() + 1000;
			return A_MORE;
		}
		/* fall through */
		if (j->flags & GPIB_SOFT_TMO) /* per byte: from when we start waiting for it */
			j->deadline = board_millis() + j->timeout_ms;
		/* fall through */
	case P_STATUS:
		s_phase = GPIB_PHASE_READ;
		phy_rx_start();
		s_inflight = IF_RX;
		return A_MORE;
	case P_UNADDR_IF_END:
		j->pc++;
		if (j->end) {
			queue_cmd(UNL);
			queue_cmd(UNT);
			s_mode = ADDR_NONE;
			s_phase = GPIB_PHASE_COMMAND;
		}
		return A_MORE;
	}
	return A_DONE;
}

void gpib_init(void)
{
	recover();
}

static void prepare(gpib_job_t *j, uint8_t owner)
{
	j->status = GPIB_PENDING;
	j->done = 0;
	j->end = false;
	j->eoi = false;
	j->timed_out = false;
	j->abort = false;
	j->owner = owner;
	if (owner < GPIB_OWNERS)
		s_last_owner = owner;
	compile(j);
}

bool gpib_submit(gpib_job_t *job, uint8_t owner)
{
	if ((owner >= GPIB_OWNERS) || s_queue[owner])
		return false;
	prepare(job, owner);
	s_queue[owner] = job;
	return true;
}

bool gpib_submit_priority(gpib_job_t *job)
{
	if (s_prio)
		return false;
	prepare(job, OWNER_PRIO);
	s_prio = job;
	return true;
}

void gpib_task(void)
{
	if (!s_active)
		pick();
	if (!s_active && (s_lock < 0) && (s_mode != ADDR_NONE)) { /* an aborted message left an instrument addressed */
		s_unaddr = (gpib_job_t){ .op = GPIB_OP_UNADDRESS, .timeout_ms = 1000 };
		prepare(&s_unaddr, OWNER_BUS);
		start(&s_unaddr);
	}

	uint32_t t0 = board_millis();
	while (s_active && ((board_millis() - t0) < BUDGET_MS)) {
		gpib_job_t *j = s_active;

		if (s_prio && (j != s_prio) && preemptable(j)) {
			s_inflight = IF_NONE;
			j->pc = 0;
			s_suspended = j;
			start(s_prio);
			continue;
		}
		if (j->abort) {
			finish(j, GPIB_ABORTED);
			continue;
		}
		int32_t late = (int32_t)(board_millis() - j->deadline);
		if (j->timeout_ms && (late >= 0)) {
			bool soft = (j->flags & GPIB_SOFT_TMO) && (j->pc < j->nprog) && (j->prog[j->pc] == P_DATA) && (s_iq >= s_nq);
			if (soft && ((s_inflight == IF_NONE) || ((s_inflight == IF_RX) && phy_rx_cancel()))) {
				/* a quiet talker ends the read: unaddress normally */
				s_inflight = IF_NONE;
				j->timed_out = true;
				j->end = true;
				j->pc++;
				j->deadline = board_millis() + 1000;
				continue;
			}
			if (!soft || (s_inflight != IF_RX) || (late >= RX_GRACE_MS)) {
				finish(j, GPIB_TIMEOUT);
				continue;
			}
			/* a byte is in its handshake: let it complete */
		}
		switch (advance(j)) {
		case A_DONE:
			finish(j, GPIB_OK);
			break;
		case A_NO_LISTENER:
			finish(j, GPIB_NO_LISTENER);
			break;
		default:
			break;
		}
	}
}

void gpib_abort(gpib_job_t *job)
{
	if (job->status != GPIB_PENDING) { /* finished mid-message: the owner gives up the bus, gpib_task() unaddresses */
		if (s_lock == job->owner)
			s_lock = -1;
		return;
	}
	if (job == s_active) {
		finish(job, GPIB_ABORTED);
	} else if (job == s_suspended) { /* the priority job unaddresses the bus */
		s_suspended = NULL;
		s_lock = -1;
		release(job, GPIB_ABORTED);
	} else {
		release(job, GPIB_ABORTED);
		if (s_lock == job->owner)
			s_lock = -1;
	}
}

gpib_status_t gpib_run(gpib_job_t *job, uint8_t owner)
{
	if (!gpib_submit(job, owner))
		return GPIB_ABORTED;
	while (job->status == GPIB_PENDING) {
		gpib_task();
		board_watchdog_feed();
	}
	return job->status;
}

void gpib_ifc(void)
{
	if (s_suspended)
		release(s_suspended, GPIB_ABORTED);
	s_suspended = NULL;
	s_active = NULL;
	if (s_prio)
		release(s_prio, GPIB_ABORTED);
	for (uint8_t o = 0; o < GPIB_OWNERS; o++)
		if (s_queue[o])
			release(s_queue[o], GPIB_ABORTED);
	recover();
}

void gpib_ren(bool asserted)
{
	phy_ren(asserted);
}

bool gpib_connected(void)
{
	if (s_active || s_suspended || (s_lock >= 0) || (s_mode != ADDR_NONE))
		return true;
	return phy_connected();
}

bool gpib_srq(void)
{
	return phy_srq();
}

bool gpib_holds_bus(uint8_t owner)
{
	return s_lock == (int8_t)owner;
}

/* For the start-up scan only: 5 ms per command byte, like v2 */
static void cmd_blocking(uint8_t b)
{
	uint32_t t0 = board_millis();

	phy_atn(true);
	phy_tx_start(b, false);
	while (phy_tx_poll() == PHY_BUSY) {
		if ((board_millis() - t0) > 5) {
			recover();
			return;
		}
	}
}

static void lag_blocking(uint8_t addr)
{
	cmd_blocking(LAG | (addr & 0x1F));
	if (addr >> 5)
		cmd_blocking(SCG | ((addr >> 5) - 1));
}

/* Addresses 1..30, then 1..30 with secondary address 0; an addressed listener holds NDAC low */
uint8_t gpib_find_device(void)
{
	uint8_t found = GPIB_NO_ADDR;

	cmd_blocking(UNL);
	for (uint8_t addr = 1; (addr < 63) && (found == GPIB_NO_ADDR); addr++) {
		if ((addr & 0x1F) == 31)
			continue;
		lag_blocking(addr);
		if (phy_probe_listener())
			found = addr;
	}
	cmd_blocking(UNL);

	/* answered with a secondary address: if it also answers without one, use primary addressing */
	if ((found != GPIB_NO_ADDR) && (found >= 32)) {
		lag_blocking(found & 0x1F);
		if (phy_probe_listener())
			found &= 0x1F;
		cmd_blocking(UNL);
	}
	s_mode = ADDR_NONE;
	return found;
}

int gpib_scan(uint8_t *found, uint8_t max)
{
	int n = 0;

	if (s_active || s_suspended || s_prio || (s_lock >= 0))
		return -1;
	for (uint8_t o = 0; o < GPIB_OWNERS; o++)
		if (s_queue[o])
			return -1;
	cmd_blocking(UNL);
	for (uint8_t addr = 1; addr <= 30; addr++) {
		lag_blocking(addr);
		if (phy_probe_listener() && (n < max))
			found[n++] = addr;
		cmd_blocking(UNL);
	}
	s_mode = ADDR_NONE;
	return n;
}

const gpib_error_t *gpib_last_error(void)
{
	return &s_error;
}

void gpib_set_trace(void (*fn)(const gpib_job_t *job))
{
	s_trace = fn;
}
