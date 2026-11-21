/* Host tests for the Prologix front end: a fake CDC port, simulated instruments (make test) */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "prologix.h"
#include "port.h"
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "settings.h"
#include "board.h"
#include "app_config.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t gpib_addr = 5;
uint8_t _settings_start[2 * BOARD_FLASH_PAGE_SIZE];
static int s_resets, s_bootloader;
static jmp_buf s_noreturn; /* board_reset() and board_enter_bootloader() don't return */

void board_flash_erase(const void *page) { memset((void *)page, 0xFF, BOARD_FLASH_PAGE_SIZE); }
void board_flash_write(const void *dst, const void *src, unsigned len) { memcpy((void *)dst, src, len); }
void board_reset(void) { s_resets++; longjmp(s_noreturn, 1); }
void board_enter_bootloader(void) { s_bootloader++; longjmp(s_noreturn, 1); }
static bool s_trace_on;
void trace_enable(bool on) { s_trace_on = on; }
bool trace_enabled(void) { return s_trace_on; }

static uint8_t  s_in[4096], s_out[4096];
static uint32_t s_in_len, s_in_pos, s_out_len, s_room = 1024;

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
static prologix_t s_px;

/* --- helpers ------------------------------------------------------------------------------------------ */

static void setup(void)
{
	sim_reset();
	memset(_settings_start, 0xFF, sizeof(_settings_start));
	settings_init();
	gpib_init();
	prologix_init(&s_px, &s_port, GPIB_OWNER_CDC);
	s_in_len = s_in_pos = s_out_len = 0;
	s_resets = s_bootloader = 0;
}

static void send_bytes(const void *p, uint32_t n)
{
	memcpy(&s_in[s_in_len], p, n);
	s_in_len += n;
}

static void send(const char *s)
{
	send_bytes(s, (uint32_t)strlen(s));
}

/* run until all input is consumed and the bus is quiet */
static void run(void)
{
	for (int i = 0; i < 200000; i++) {
		prologix_run(&s_px);
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

static void test_addr(void)
{
	setup();
	send("++addr\n");
	run();
	assert(out_is("5\r\n")); /* defaults to the autoid instrument */
	send("++addr 9 96\n++addr\n++addr 31\n++addr\n");
	run();
	assert(out_is("9 96\r\n9 96\r\n"));
	send("++addr 5\n"); /* the autoid instrument: not pinned, follows a new one */
	run();
	gpib_addr = 8;
	send("++addr\n");
	run();
	assert(out_is("8\r\n") && settings.px_addr == SETTINGS_NO_ADDR);
	gpib_addr = 5;
}

static void test_write_appends_eos_and_eoi(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("*RST\r\n");
	run();
	assert(dev_got(d, "*RST\r\n") && d->messages == 1);
	for (int i = 0; i < 6; i++)
		assert(d->in_eoi[i] == (i == 5));
	assert(out_is(""));
}

static void test_eos_and_eoi_settings(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("++eos 3\n++eoi 0\nAB\n++eos\n++eoi\n");
	run();
	assert(dev_got(d, "AB") && !d->in_eoi[1] && d->messages == 0);
	assert(out_is("3\r\n0\r\n"));
}

static void test_escapes(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("++eos 2\n");
	send("A\x1b\nB\x1b+C+D\x1b\x1b\n");
	run();
	assert(dev_got(d, "A\nB+CD\x1b\n"));
}

static void test_empty_lines_ignored(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	send("\r\n\n\r");
	run();
	assert(d->in_len == 0 && d->ncmds == 0);
}

static void test_auto_read_after_write(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = answer_idn;
	send("++auto 1\n*IDN?\n");
	run();
	assert(out_is("ACME,M1,42,1.0\n"));
}

static void test_read_modes(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	answer_idn(d);
	send("++read eoi\n");
	run();
	assert(out_is("ACME,M1,42,1.0\n"));

	answer_idn(d);
	send("++eot_enable 1\n++eot_char 42\n++read eoi\n");
	run();
	assert(out_is("ACME,M1,42,1.0\n*"));

	sim_answer(d, "1.5\n2.5\n", false);
	send("++eot_enable 0\n++read 10\n");
	run();
	assert(out_is("1.5\n"));

	sim_answer(d, "XY", true); /* plain ++read goes on past EOI until the timeout */
	send("++eot_enable 1\n++read_tmo_ms 50\n++read\n");
	run();
	assert(out_is("XY*"));
	assert(!d->talker && d->ifc == 0);
}

static void test_long_read_in_chunks(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	char big[201];
	for (int i = 0; i < 200; i++)
		big[i] = (char)('a' + i % 26);
	big[200] = '\0';
	sim_answer(d, big, true);
	send("++read eoi\n");
	run();
	assert(out_is(big));
}

static void test_long_write_in_pieces(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	static char line[601];
	for (int i = 0; i < 600; i++)
		line[i] = (char)('0' + i % 10);
	line[600] = '\0';
	send("++eos 2\n");
	send(line);
	send("\n");
	run();
	assert(d->in_len == 601 && !memcmp(d->in, line, 600) && d->in[600] == '\n' && d->messages == 1);
	for (int i = 0; i < 601; i++)
		assert(d->in_eoi[i] == (i == 600));
}

/* no EOS and a multiple of the 256-byte piece size: EOI still goes with the last byte */
static void test_long_write_no_eos(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	static char line[513];
	for (int i = 0; i < 512; i++)
		line[i] = (char)('0' + i % 10);
	line[512] = '\0';
	send("++eos 3\n");
	send(line);
	send("\n");
	run();
	assert(d->in_len == 512 && !memcmp(d->in, line, 512) && d->messages == 1);
	for (int i = 0; i < 512; i++)
		assert(d->in_eoi[i] == (i == 511));
}

static void test_spoll_srq_clr_trg_loc(void)
{
	setup();
	sim_dev_t *a = sim_add(5, -1), *b = sim_add(9, -1);
	a->status = 0x10;
	a->srq = true;
	send("++srq\n++spoll\n++srq\n");
	run();
	assert(out_is("1\r\n80\r\n0\r\n"));
	b->status = 3;
	send("++spoll 9\n++clr\n++trg 5 9\n++loc\n++llo\n");
	run();
	assert(out_is("3\r\n"));
	assert(a->sdc == 1 && a->get == 1 && b->get == 1 && a->gtl == 1 && a->llo == 1 && b->sdc == 0);
}

static void test_scan_and_err(void)
{
	setup();
	sim_add(3, -1);
	sim_add(22, -1);
	send("++scan\n++err\n++addr 12\nX\n++err\n");
	run();
	assert(out_is("3 22\r\nnone\r\nno listener write addr 12\r\n"));
}

static void test_misc(void)
{
	setup();
	send("++ver\n++mode\n++savecfg\n++bogus\n++autoid slow\n++autoid\n++idstring short\n++idstring\n");
	run();
	assert(out_is(APP_PROLOGIX_VERSION "\r\n1\r\n1\r\nslow\r\nshort\r\n"));
	assert(settings.autoid == AUTOID_SLOW && settings.idstring_short);
	if (!setjmp(s_noreturn)) {
		send("++rst\n");
		run();
		assert(0);
	}
	if (!setjmp(s_noreturn)) {
		send("++dfu\n");
		run();
		assert(0);
	}
	assert(s_resets == 1 && s_bootloader == 1);
}

static void test_listen_only_capture(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->talker = true; /* plotter output from an instrument in talk-only mode */
	sim_answer(d, "IN;SP1;PU0,0;", true);
	send("++eot_enable 1\n++eot_char 10\n++lon 1\n");
	run();
	assert(out_is("IN;SP1;PU0,0;\n"));
	assert(d->ncmds == 0);
	send("++lon\n"); /* input ends the capture, then runs as usual */
	run();
	assert(out_is("0\r\n"));
}

static void test_talk_only_writes(void)
{
	setup();
	sim_dev_t *d = sim_add(5, -1);
	d->listener = true; /* listen-only printer */
	send("++ton 1\n++eos 2\nPRINT\n++ton\n");
	run();
	assert(dev_got(d, "PRINT\n") && d->in_eoi[5] && d->ncmds == 0);
	assert(out_is("1\r\n"));
}

static void test_trace_and_usb(void)
{
	setup();
	send("++trace\n++trace on\n++trace\n++usb\n");
	run();
	assert(out_is("off\r\non\r\nboth\r\n"));
	if (!setjmp(s_noreturn)) {
		send("++usb cdc\n");
		run();
		assert(0);
	}
	settings_init();
	assert(settings.usb_mode == USB_MODE_CDC && s_resets == 1);
}

static void test_settings_persist(void)
{
	setup();
	send("++addr 7\n++auto 1\n++read_tmo_ms 1200\n");
	run();
	settings_flush();
	settings_init();
	assert(settings.px_addr == 7 && settings.px_auto == 1 && settings.px_read_tmo_ms == 1200);
}

static void test_net(void)
{
	setup();
	send("++net\n"); /* no board network hook here */
	run();
	assert(out_is("no network\r\n"));
	send("++net static 10.0.0.5 255.255.255.0 10.0.0.1\n++net name bench\n");
	run();
	assert(out_is("") && settings.net_mode == NET_MODE_STATIC && settings.net_ip[3] == 5);
	assert(!strcmp(settings.net_name, "bench"));
	send("++net password abc\n"); /* the CDC port: allowed */
	run();
	assert(!strcmp(settings.update_pw, "abc"));
	prologix_init(&s_px, &s_port, GPIB_OWNER_PX_NET); /* TCP 1234: other settings yes, the password never */
	send("++net password xyz\n++net password off\n++net name lab\n");
	run();
	assert(!strcmp(settings.update_pw, "abc") && !strcmp(settings.net_name, "lab"));
	s_bootloader = 0;
	send("++dfu\n"); /* TCP 1234: refused */
	run();
	assert(out_is("dfu: from USB only\r\n") && (s_bootloader == 0));
}

int main(void)
{
	test_addr();
	test_write_appends_eos_and_eoi();
	test_eos_and_eoi_settings();
	test_escapes();
	test_empty_lines_ignored();
	test_auto_read_after_write();
	test_read_modes();
	test_long_read_in_chunks();
	test_long_write_in_pieces();
	test_long_write_no_eos();
	test_spoll_srq_clr_trg_loc();
	test_scan_and_err();
	test_misc();
	test_listen_only_capture();
	test_talk_only_writes();
	test_trace_and_usb();
	test_settings_persist();
	test_net();
	puts("prologix: all tests passed");
	return 0;
}
