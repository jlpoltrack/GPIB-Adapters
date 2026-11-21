#include <string.h>
#include "prologix.h"
#include "port.h"
#include "gpib_bus.h"
#include "settings.h"
#include "board.h"
#include "trace.h"
#include "instr.h"
#include "netcfg.h"
#include "app_config.h" /* APP_PROLOGIX_VERSION: the ++ver answer */

/* Input is collected into lines ended by an unescaped CR or LF. ESC makes the next byte literal; unescaped ESC
 * and '+' are dropped. A line starting with an unescaped "++" is a command, anything else goes to the instrument
 * with the ++eos terminator and EOI (++eoi). Data lines longer than the buffer go out in pieces. */

#define LINE_MAX  PROLOGIX_LINE_MAX
#define ESC       27
#define WRITE_TMO 10000 /* ms: a listener that never takes a byte */

enum { PX_IDLE, PX_WRITE, PX_READ, PX_READ_MORE, PX_JOB, PX_LON, PX_LON_MORE };
enum { JOB_CLR, JOB_LOC, JOB_LLO, JOB_TRG, JOB_SPOLL };

static prologix_t *P; /* the instance prologix_run() is serving */

static uint32_t port_read(uint8_t *buf, uint32_t max) { return P->port->read(P->port->ctx, buf, max); }
static uint32_t port_write_room(void) { return P->port->write_room(P->port->ctx); }
static void port_write(const void *buf, uint32_t n) { P->port->write(P->port->ctx, buf, n); }
static void port_flush(void) { P->port->flush(P->port->ctx); }

/* --- output ----------------------------------------------------------------------------------------- */

static void put(const char *s)
{
	port_write(s, (uint32_t)strlen(s));
}

static void put_uint(unsigned v)
{
	char b[8];
	int i = sizeof(b) - 1;

	b[i] = '\0';
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	put(&b[i]);
}

static void endl(void)
{
	put("\r\n");
	port_flush();
}

static void reply_uint(unsigned v)
{
	put_uint(v);
	endl();
}

static void reply(const char *s)
{
	put(s);
	endl();
}

/* --- configuration ---------------------------------------------------------------------------------- */

static uint8_t addr(void)
{
	return (settings.px_addr != SETTINGS_NO_ADDR) ? settings.px_addr : gpib_addr;
}

static void config_changed(void)
{
	if (P->savecfg)
		settings_changed();
}

static bool parse_uint(const char *s, unsigned *v)
{
	unsigned n = 0;

	if (!*s)
		return false;
	for (; *s; s++) {
		if ((*s < '0') || (*s > '9') || (n > 100000u))
			return false;
		n = n * 10u + (unsigned)(*s - '0');
	}
	*v = n;
	return true;
}

/* PAD 0..30, SAD 96..102 (the bus encodes secondary addresses 0..6) */
static bool parse_addr(const char *pad, const char *sad, uint8_t *out)
{
	unsigned p, s;

	if (!parse_uint(pad, &p) || (p > 30))
		return false;
	if (!sad) {
		*out = (uint8_t)p;
		return true;
	}
	if (!parse_uint(sad, &s) || (s < 96) || (s > 102))
		return false;
	*out = (uint8_t)(p | ((s - 96 + 1) << 5));
	return true;
}

static void put_addr(uint8_t a)
{
	put_uint(a & 0x1Fu);
	if (a >> 5) {
		put(" ");
		put_uint(96u + (a >> 5) - 1u);
	}
}

/* ++flag [0|1] style setting */
static void flag_cmd(int argc, char **argv, uint8_t *field)
{
	unsigned v;

	if (argc == 1)
		reply_uint(*field);
	else if (parse_uint(argv[1], &v) && (v <= 1)) {
		*field = (uint8_t)v;
		config_changed();
	}
}

/* --- GPIB jobs -------------------------------------------------------------------------------------- */

static void submit(gpib_job_t *j)
{
	gpib_ren(true);
	gpib_submit(j, P->owner);
}

static void submit_read(void)
{
	P->job = (gpib_job_t){ .op = GPIB_OP_READ, .addr = addr(), .flags = P->rflags, .term = P->rterm,
	                      .buf = P->rbuf, .len = sizeof(P->rbuf), .timeout_ms = settings.px_read_tmo_ms };
	submit(&P->job);
	P->state = PX_READ;
}

static void start_read(uint8_t flags, uint8_t term)
{
	P->rflags = (uint8_t)(flags | GPIB_SOFT_TMO);
	P->rterm = term;
	P->state = PX_READ_MORE; /* submits once the port has room for a chunk */
}

static void start_job(uint8_t op, uint8_t a, uint8_t kind)
{
	P->job = (gpib_job_t){ .op = op, .addr = a, .buf = &P->status, .timeout_ms = (op == GPIB_OP_SPOLL) ? settings.px_read_tmo_ms : 1000 };
	submit(&P->job);
	P->kind = kind;
	P->state = PX_JOB;
}

static void send_data(bool final)
{
	if (final) {
		static const char *const eos[] = { "\r\n", "\r", "\n", "" };
		const char *t = eos[settings.px_eos & 3];
		while (*t)
			P->line[P->len++] = (uint8_t)*t++;
	}
	P->final = final;
	/* a piece keeps its last byte back, so the final piece always has one to carry EOI */
	uint16_t len = final ? P->len : (uint16_t)(P->len - 1u);
	if (P->ton)
		P->job = (gpib_job_t){ .op = GPIB_OP_TALK_ONLY, .buf = P->line, .len = len, .timeout_ms = WRITE_TMO,
		                      .flags = (final && settings.px_eoi) ? GPIB_EOI : 0 };
	else
		P->job = (gpib_job_t){ .op = GPIB_OP_WRITE, .addr = addr(), .buf = P->line, .len = len, .timeout_ms = WRITE_TMO,
		                      .flags = final ? (uint8_t)(GPIB_END | (settings.px_eoi ? GPIB_EOI : 0)) : 0 };
	submit(&P->job);
	P->state = PX_WRITE;
}

/* --- commands --------------------------------------------------------------------------------------- */

static const char *const s_autoid_names[] = { "on", "off", "slow", "slower", "slowest" };

static void cmd_scan(void)
{
	uint8_t found[30];
	int n = gpib_scan(found, sizeof(found));

	if (n < 0) {
		reply("busy");
		return;
	}
	for (int i = 0; i < n; i++) {
		if (i)
			put(" ");
		put_uint(found[i]);
	}
	endl();
}

static void cmd_err(void)
{
	static const char *const status[] = { "none", "pending", "timeout", "aborted", "no listener" };
	static const char *const phase[] = { "", " addressing", " command", " write", " read" };
	const gpib_error_t *e = gpib_last_error();

	put(status[e->status]);
	if (e->status != GPIB_OK) {
		put(phase[e->phase]);
		put(" addr ");
		put_addr(e->addr);
	}
	endl();
}

static void command(char *line)
{
	char *argv[32];
	int argc = 0;
	unsigned v;
	uint8_t a;

	for (char *p = line; *p && (argc < 32);) {
		while (*p == ' ' || *p == '\t')
			*p++ = '\0';
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && (*p != ' ') && (*p != '\t'))
			p++;
	}
	if (!argc)
		return;
	for (char *p = argv[0]; *p; p++)
		if ((*p >= 'A') && (*p <= 'Z'))
			*p = (char)(*p + 32);
	const char *c = argv[0];

	if (!strcmp(c, "addr")) {
		if (argc == 1) {
			put_addr(addr());
			endl();
		} else if (parse_addr(argv[1], (argc > 2) ? argv[2] : NULL, &a)) {
			/* the found instrument's address keeps following auto-ID, so a later instrument swap still works */
			settings.px_addr = (a == gpib_addr) ? SETTINGS_NO_ADDR : a;
			config_changed();
		}
	} else if (!strcmp(c, "auto")) {
		flag_cmd(argc, argv, &settings.px_auto);
	} else if (!strcmp(c, "eoi")) {
		flag_cmd(argc, argv, &settings.px_eoi);
	} else if (!strcmp(c, "eot_enable")) {
		flag_cmd(argc, argv, &settings.px_eot_enable);
	} else if (!strcmp(c, "eos")) {
		if (argc == 1)
			reply_uint(settings.px_eos);
		else if (parse_uint(argv[1], &v) && (v <= 3)) {
			settings.px_eos = (uint8_t)v;
			config_changed();
		}
	} else if (!strcmp(c, "eot_char")) {
		if (argc == 1)
			reply_uint(settings.px_eot_char);
		else if (parse_uint(argv[1], &v) && (v <= 255)) {
			settings.px_eot_char = (uint8_t)v;
			config_changed();
		}
	} else if (!strcmp(c, "read_tmo_ms")) {
		if (argc == 1)
			reply_uint(settings.px_read_tmo_ms);
		else if (parse_uint(argv[1], &v) && (v >= 1) && (v <= 3000)) {
			settings.px_read_tmo_ms = (uint16_t)v;
			config_changed();
		}
	} else if (!strcmp(c, "read")) {
		if (argc == 1)
			start_read(GPIB_IGNORE_EOI, 0);
		else if (!strcmp(argv[1], "eoi"))
			start_read(0, 0);
		else if (parse_uint(argv[1], &v) && (v <= 255))
			start_read(GPIB_TERM, (uint8_t)v);
	} else if (!strcmp(c, "clr")) {
		start_job(GPIB_OP_SDC, addr(), JOB_CLR);
	} else if (!strcmp(c, "loc")) {
		start_job(GPIB_OP_GTL, addr(), JOB_LOC);
	} else if (!strcmp(c, "llo")) {
		start_job(GPIB_OP_LLO, addr(), JOB_LLO);
	} else if (!strcmp(c, "ifc")) {
		gpib_ifc();
	} else if (!strcmp(c, "trg")) {
		P->ntrg = P->itrg = 0;
		for (int i = 1; (i < argc) && (P->ntrg < sizeof(P->trg)); i++) {
			const char *sad = NULL;
			if ((i + 1 < argc) && parse_uint(argv[i + 1], &v) && (v >= 96))
				sad = argv[i + 1];
			if (!parse_addr(argv[i], sad, &a))
				return;
			P->trg[P->ntrg++] = a;
			if (sad)
				i++;
		}
		if (!P->ntrg)
			P->trg[P->ntrg++] = addr();
		start_job(GPIB_OP_GET, P->trg[0], JOB_TRG);
	} else if (!strcmp(c, "spoll")) {
		a = addr();
		if ((argc > 1) && !parse_addr(argv[1], (argc > 2) ? argv[2] : NULL, &a))
			return;
		start_job(GPIB_OP_SPOLL, a, JOB_SPOLL);
	} else if (!strcmp(c, "srq")) {
		reply_uint(gpib_srq());
	} else if (!strcmp(c, "mode")) {
		if (argc == 1)
			reply_uint(1); /* controller only */
	} else if (!strcmp(c, "savecfg")) {
		if (argc == 1)
			reply_uint(P->savecfg);
		else if (parse_uint(argv[1], &v) && (v <= 1)) {
			P->savecfg = v;
			if (v)
				settings_changed();
		}
	} else if (!strcmp(c, "rst")) {
		board_reset();
	} else if (!strcmp(c, "ver")) {
		reply(APP_PROLOGIX_VERSION);
	} else if (!strcmp(c, "help")) {
		reply("addr auto clr eoi eos eot_enable eot_char ifc llo loc lon mode read read_tmo_ms rst savecfg spoll srq trg ver"
		      " | scan err ton trace usb autoid idstring dfu net");
	/* this adapter's extensions */
	} else if (!strcmp(c, "scan")) {
		cmd_scan();
	} else if (!strcmp(c, "err")) {
		cmd_err();
	} else if (!strcmp(c, "autoid")) {
		if (argc == 1) {
			reply(s_autoid_names[(settings.autoid < 5) ? settings.autoid : 0]);
			return;
		}
		for (uint8_t i = 0; i < 5; i++)
			if (!strcmp(argv[1], s_autoid_names[i])) {
				settings.autoid = i;
				settings_changed();
			}
	} else if (!strcmp(c, "idstring")) {
		if (argc == 1)
			reply(settings.idstring_short ? "short" : "normal");
		else if (!strcmp(argv[1], "short") || !strcmp(argv[1], "normal")) {
			settings.idstring_short = !strcmp(argv[1], "short");
			settings_changed();
		}
	} else if (!strcmp(c, "lon")) { /* listen-only capture until the next input */
		if (argc == 1)
			reply_uint(0);
		else if (parse_uint(argv[1], &v) && (v == 1))
			P->state = PX_LON_MORE;
	} else if (!strcmp(c, "ton")) {
		if (argc == 1)
			reply_uint(P->ton);
		else if (parse_uint(argv[1], &v) && (v <= 1))
			P->ton = v;
	} else if (!strcmp(c, "trace")) {
		if (argc == 1)
			reply(trace_enabled() ? "on" : "off");
		else if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off"))
			trace_enable(!strcmp(argv[1], "on"));
	} else if (!strcmp(c, "usb")) {
		static const char *const modes[] = { "both", "tmc", "cdc" };
		if (argc == 1) {
			reply(modes[(settings.usb_mode < 3) ? settings.usb_mode : 0]);
			return;
		}
		for (uint8_t i = 0; i < 3; i++)
			if (!strcmp(argv[1], modes[i]) && (i != settings.usb_mode)) {
				settings.usb_mode = i;
				settings_changed();
				settings_flush();
				board_reset(); /* enumerate again with the new interfaces */
			}
	} else if (!strcmp(c, "dfu")) {
		if (P->owner == GPIB_OWNER_PX_NET) /* anyone on the LAN could take the adapter offline until a power cycle */
			reply("dfu: from USB only");
		else
			board_enter_bootloader();
	} else if (!strcmp(c, "net")) {
		char r[128];
		if (netcfg_command(argc, argv, r, sizeof(r), P->owner != GPIB_OWNER_PX_NET))
			reply(r);
	}
	/* unknown commands are ignored, like the Prologix */
}

/* --- input ------------------------------------------------------------------------------------------ */

static void line_reset(void)
{
	P->len = 0;
	P->esc = P->cmd = P->any = P->sent_part = false;
	P->plus = 0;
}

/* Returns true when the byte started GPIB work: stop reading input until it is done */
static bool input(uint8_t c)
{
	if (P->esc) {
		P->esc = false;
	} else if (c == ESC) {
		P->esc = true;
		P->any = true;
		return false;
	} else if ((c == '\r') || (c == '\n')) {
		if (P->cmd) {
			P->line[P->len] = '\0';
			line_reset();
			command((char *)P->line);
			return P->state != PX_IDLE;
		}
		if (P->len || P->sent_part) {
			send_data(true);
			return true;
		}
		line_reset(); /* empty line, e.g. the LF of a CR LF */
		return false;
	} else if (c == '+') {
		if (!P->any && !P->cmd) { /* leading '+': the start of "++"? */
			if (++P->plus == 2)
				P->cmd = true;
			return false;
		}
		if (!P->cmd)
			return false; /* unescaped '+' in data is dropped */
	}
	P->any = true;
	if (P->len < LINE_MAX)
		P->line[P->len++] = c;
	if ((P->len == LINE_MAX) && !P->cmd) { /* long data: send this piece without ending the message */
		send_data(false);
		return true;
	}
	return false;
}

static void job_done(void)
{
	switch (P->kind) {
	case JOB_SPOLL:
		if (P->job.status == GPIB_OK)
			reply_uint(P->status);
		break;
	case JOB_TRG:
		if (++P->itrg < P->ntrg) {
			start_job(GPIB_OP_GET, P->trg[P->itrg], JOB_TRG);
			return;
		}
		break;
	default:
		break;
	}
	P->state = PX_IDLE;
}

void prologix_init(prologix_t *px, const port_t *port, uint8_t owner)
{
	*px = (prologix_t){ .port = port, .owner = owner, .savecfg = true };
}

void prologix_reset(prologix_t *px)
{
	P = px;
	if (P->job.status == GPIB_PENDING)
		gpib_abort(&P->job);
	P->state = PX_IDLE;
	line_reset();
}

void prologix_run(prologix_t *px)
{
	P = px;
	switch (P->state) {
	case PX_IDLE: {
		uint8_t c;
		for (int n = 0; (n < 64) && port_read(&c, 1); n++)
			if (input(c))
				break;
		break;
	}
	case PX_WRITE:
		if (P->job.status == GPIB_PENDING)
			break;
		if (P->final) {
			line_reset();
			if ((P->job.status == GPIB_OK) && settings.px_auto) { /* read-after-write */
				start_read(0, 0);
				break;
			}
		} else {
			P->line[0] = P->line[P->len - 1u];
			P->len = 1;
			P->sent_part = true;
		}
		P->state = PX_IDLE;
		break;
	case PX_READ:
		if (P->job.status == GPIB_PENDING)
			break;
		port_write(P->rbuf, P->job.done);
		if (P->job.eoi && settings.px_eot_enable)
			port_write(&settings.px_eot_char, 1);
		port_flush();
		P->state = (P->job.end || (P->job.status != GPIB_OK)) ? PX_IDLE : PX_READ_MORE;
		break;
	case PX_READ_MORE: /* the talker stays addressed, held off by NRFD, until the host takes the data */
		if (port_write_room() > sizeof(P->rbuf))
			submit_read();
		break;
	case PX_JOB:
		if (P->job.status != GPIB_PENDING)
			job_done();
		break;
	case PX_LON:
	case PX_LON_MORE: {
		uint8_t c;
		if (port_read(&c, 1)) { /* any input ends the capture */
			gpib_abort(&P->job);
			P->state = PX_IDLE;
			input(c);
			break;
		}
		if (P->state == PX_LON_MORE) {
			if (port_write_room() > sizeof(P->rbuf)) {
				P->job = (gpib_job_t){ .op = GPIB_OP_LISTEN_ONLY, .flags = GPIB_IGNORE_EOI, .buf = P->rbuf, .len = sizeof(P->rbuf) };
				gpib_submit(&P->job, P->owner);
				P->state = PX_LON;
			}
		} else if (P->job.status != GPIB_PENDING) {
			port_write(P->rbuf, P->job.done);
			if (P->job.eoi && settings.px_eot_enable)
				port_write(&settings.px_eot_char, 1);
			port_flush();
			P->state = (P->job.status == GPIB_OK) ? PX_LON_MORE : PX_IDLE;
		}
		break;
	}
	}
}
