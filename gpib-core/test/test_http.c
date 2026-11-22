/* Host tests for the HTTP server: requests in pieces, the response through small windows, the settings form (make test) */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "netcfg.h"
#include "settings.h"
#include "instr.h"
#include "board.h"

/* --- fakes -------------------------------------------------------------------------------------------- */

uint8_t _settings_start[2 * BOARD_FLASH_PAGE_SIZE];
void board_flash_erase(const void *page) { memset((void *)page, 0xFF, BOARD_FLASH_PAGE_SIZE); }
void board_flash_write(const void *dst, const void *src, unsigned len) { memcpy((void *)dst, src, len); }
void board_get_uid_hex(char *buf) { strcpy(buf, "0123456789ABCDEF01234567"); }
static uint32_t s_now;
uint32_t board_millis(void) { return s_now; }
uint8_t gpib_addr = 7;
static const char *s_id = "ACME,<M1> & co,42,1.0";
const char *instr_id(void) { return s_id; }
static bool s_ready = true;
bool instr_ready(void) { return s_ready; }
const char *board_chip_info(void) { return "CH32 <test> & co"; }

bool board_net_status(netcfg_status_t *st)
{
	static const uint8_t ip[4] = { 192, 168, 1, 23 }, mask[4] = { 255, 255, 255, 0 }, gw[4] = { 192, 168, 1, 1 };
	static const uint8_t mac[6] = { 0x02, 0xAB, 0x00, 0x12, 0x34, 0xF0 };
	memcpy(st->ip, ip, 4);
	memcpy(st->mask, mask, 4);
	memcpy(st->gw, gw, 4);
	memcpy(st->mac, mac, 6);
	st->hostname = "langpib-34f0";
	st->dhcp = true;
	return true;
}

/* the board's update hooks: the body lands in s_img */
static bool     s_upd_on;
static uint8_t  s_img[200000];
static uint32_t s_img_len, s_img_max = sizeof(s_img);
static int      s_begins, s_dones, s_end_result, s_write_fail_at = -1;
bool board_update_supported(void) { return s_upd_on; }
int board_update_begin(uint32_t len)
{
	s_begins++;
	s_img_len = 0;
	return (len > s_img_max) ? HTTP_UPD_TOO_LARGE : HTTP_UPD_OK;
}
int board_update_write(const uint8_t *buf, uint32_t n)
{
	assert(s_img_len + n <= sizeof(s_img));
	if ((s_write_fail_at >= 0) && (s_img_len + n > (uint32_t)s_write_fail_at))
		return HTTP_UPD_FLASH;
	memcpy(&s_img[s_img_len], buf, n);
	s_img_len += n;
	return HTTP_UPD_OK;
}
int board_update_end(void) { return s_end_result; }
void board_update_done(void) { s_dones++; }

static char     s_in[300000], s_out[16384];
static uint32_t s_in_len, s_in_pos, s_out_len, s_room, s_chunk;
static int      s_closed, s_slots = 1000000; /* writes accepted per run, as lwIP's send queue (TCP_SND_QUEUELEN) */

static uint32_t fake_read(void *ctx, uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	(void)ctx;
	while ((n < max) && (n < s_chunk) && (s_in_pos < s_in_len))
		buf[n++] = (uint8_t)s_in[s_in_pos++];
	return n;
}
static uint32_t fake_write_room(void *ctx) { (void)ctx; return (s_slots > 0) ? s_room : 0; }
static void fake_write(void *ctx, const void *buf, uint32_t n)
{
	(void)ctx;
	assert(s_slots > 0 && n <= s_room && s_out_len + n < sizeof(s_out));
	s_slots--;
	memcpy(&s_out[s_out_len], buf, n);
	s_out_len += n;
	s_room -= n;
}
static void fake_flush(void *ctx) { (void)ctx; }
static void fake_close(void *ctx) { (void)ctx; s_closed++; }

static const port_t s_port = { fake_read, fake_write_room, fake_write, fake_flush, NULL, fake_close };
static http_t s_h;

/* --- helpers ------------------------------------------------------------------------------------------ */

static void setup(void)
{
	memset(_settings_start, 0xFF, sizeof(_settings_start));
	settings_init();
	http_init(&s_h, &s_port);
	s_in_len = s_in_pos = s_out_len = 0;
	s_closed = 0;
}

/* the request, read chunk bytes at a time, answered room bytes in at most slots writes per run; the whole response */
static const char *run_in(uint32_t len, uint32_t chunk, uint32_t room, int slots)
{
	http_reset(&s_h);
	s_in_len = len;
	s_in_pos = s_out_len = 0;
	s_closed = 0;
	s_chunk = chunk;
	for (int i = 0; (i < 100000) && !s_closed; i++) {
		s_room = room;
		s_slots = slots;
		http_run(&s_h);
	}
	s_slots = 1000000;
	assert(s_closed == 1);
	s_out[s_out_len] = '\0';
	return s_out;
}

static const char *get_slots(const char *req, uint32_t chunk, uint32_t room, int slots)
{
	strcpy(s_in, req);
	return run_in((uint32_t)strlen(req), chunk, room, slots);
}

static const char *get(const char *req, uint32_t chunk, uint32_t room)
{
	return get_slots(req, chunk, room, 1000000);
}

static const char *body_of(const char *resp)
{
	const char *b = strstr(resp, "\r\n\r\n");
	assert(b);
	return b + 4;
}

static unsigned content_length(const char *resp)
{
	const char *p = strstr(resp, "Content-Length: ");
	assert(p);
	return (unsigned)atoi(p + 16);
}

/* --- tests -------------------------------------------------------------------------------------------- */

static void test_home_in_windows(void)
{
	static char whole[16384];
	setup();
	strcpy(whole, get("GET / HTTP/1.1\r\nHost: langpib-34f0.local\r\n\r\n", 1000, 100000));
	assert(!strncmp(whole, "HTTP/1.1 200 OK\r\n", 17) && strstr(whole, "Content-Type: text/html"));
	assert(content_length(whole) == strlen(body_of(whole)));
	for (uint32_t room = 1; room < 1500; room = room * 3 + 1) {
		const char *r = get("GET / HTTP/1.1\r\nHost: langpib-34f0.local\r\n\r\n", 3, room);
		assert(!strcmp(r, whole)); /* the same bytes, whatever the windows */
	}
	for (int slots = 1; slots < 20; slots += 3) /* a full queue takes less than the window (seen on the board) */
		assert(!strcmp(get_slots("GET / HTTP/1.1\r\nHost: langpib-34f0.local\r\n\r\n", 1000, 5840, slots), whole));
	assert(strstr(whole, "ACME,&lt;M1&gt; &amp; co,42,1.0")); /* escaped */
	assert(strstr(whole, "TCPIP::192.168.1.23::inst0::INSTR") && strstr(whole, "langpib-34f0.local::inst0::INSTR"));
	assert(strstr(whole, "02:AB:00:12:34:F0") && strstr(whole, "value=\"dhcp\" checked"));
}

static void test_identification(void)
{
	setup();
	const char *r = get("GET /lxi/identification HTTP/1.1\r\n\r\n", 7, 333);
	assert(strstr(r, "Content-Type: text/xml") && (content_length(r) == strlen(body_of(r))));
	assert(strstr(r, "<SerialNumber>0123456789ABCDEF01234567</SerialNumber>"));
	assert(strstr(r, "<FirmwareRevision>0.0.0</FirmwareRevision>"));
	assert(strstr(r, "GPIB 7: ACME,&lt;M1&gt; &amp; co,42,1.0</ManufacturerDescription>"));
	assert(strstr(r, "<IdentificationURL>http://192.168.1.23/lxi/identification</IdentificationURL>"));
	assert(strstr(r, "<Hostname>langpib-34f0.local</Hostname>") && strstr(r, "<DHCPEnabled>true</DHCPEnabled>"));
}

static void test_errors(void)
{
	setup();
	assert(!strncmp(get("GET /x HTTP/1.1\r\n\r\n", 100, 9999), "HTTP/1.1 404", 12));
	assert(!strncmp(get("DELETE / HTTP/1.1\r\n\r\n", 100, 9999), "HTTP/1.1 405", 12));
	assert(strstr(s_out, "Allow: GET, HEAD, POST"));
	assert(!strncmp(get("GET /net HTTP/1.1\r\n\r\n", 100, 9999), "HTTP/1.1 405", 12));
	assert(!strncmp(get("garbage\r\n\r\n", 100, 9999), "HTTP/1.1 400", 12));
	assert(!strncmp(get("POST /net HTTP/1.1\r\nContent-Length: 999\r\n\r\n", 100, 9999), "HTTP/1.1 413", 12));
	const char *r = get("HEAD / HTTP/1.1\r\n\r\n", 100, 9999);
	assert(!strncmp(r, "HTTP/1.1 200", 12) && content_length(r) > 1000 && !*body_of(r));
	char longline[300] = "GET /";
	memset(&longline[5], 'a', 200);
	strcpy(&longline[205], " HTTP/1.1\r\n\r\n");
	assert(!strncmp(get(longline, 100, 9999), "HTTP/1.1 400", 12));
}

static void test_waits_for_request(void)
{
	setup();
	strcpy(s_in, "GET / HTTP/1.1\r\nHost: langpib-34f0.local\r\n");
	s_in_len = (uint32_t)strlen(s_in);
	s_chunk = 100;
	for (int i = 0; i < 10; i++) {
		s_room = 9999;
		http_run(&s_h);
	}
	assert(!s_out_len && !s_closed); /* no blank line yet */
}

static const char *post(const char *form)
{
	static char req[1024];
	snprintf(req, sizeof(req), "POST /net HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded\r\n"
	                           "content-length: %u\r\n\r\n%s", (unsigned)strlen(form), form);
	return get(req, 5, 700);
}

static void test_form(void)
{
	setup();
	assert(strstr(post("mode=static&ip=10.0.0.5&mask=255.255.255.0&gw=10.0.0.1&name=bench-dmm"), "msg ok"));
	assert(settings.net_mode == NET_MODE_STATIC && settings.net_ip[3] == 5 && settings.net_gw[3] == 1);
	assert(!strcmp(settings.net_name, "bench-dmm"));
	assert(strstr(s_out, "value=\"static\" checked") && strstr(s_out, "name=\"ip\" value=\"10.0.0.5\""));
	settings_init(); /* in flash */
	assert(settings.net_mode == NET_MODE_STATIC && !strcmp(settings.net_name, "bench-dmm"));

	assert(strstr(post("mode=static&ip=10.0.0.255&mask=255.255.255.0&gw=&name=x"), "msg bad")); /* broadcast */
	assert(strstr(post("mode=static&ip=10.0.0.7&mask=255.0.255.0&gw=&name=x"), "msg bad"));
	assert(strstr(post("mode=static&ip=10.0.0.7&mask=&gw=&name=x"), "msg bad"));
	assert(strstr(post("mode=dhcp&name=bad.name"), "msg bad"));
	assert(strstr(post("mode=bogus"), "msg bad"));
	assert(settings.net_ip[3] == 5 && !strcmp(settings.net_name, "bench-dmm")); /* nothing changed */

	assert(strstr(post("mode=dhcp&ip=&mask=&gw=&name=+lab-2+"), "msg ok")); /* '+' is a space, trimmed */
	assert(settings.net_mode == NET_MODE_DHCP && !strcmp(settings.net_name, "lab-2"));
	assert(strstr(post("mode=static&ip=%31%30.1.2.3&mask=255.255.0.0"), "msg ok")); /* %xx; no gw; name kept */
	assert(settings.net_ip[0] == 10 && settings.net_gw[0] == 0 && !strcmp(settings.net_name, "lab-2"));
	assert(strstr(post("mode=dhcp&name="), "msg ok") && !settings.net_name[0]); /* empty: the default */
	assert(strstr(s_out, "value=\"dhcp\" checked") && strstr(s_out, "name=\"ip\" value=\"\""));
}

/* POST /update with the body; pw NULL: no password header; len false: no Content-Length */
static const char *upload(const char *pw, const uint8_t *data, uint32_t n, bool len, uint32_t chunk)
{
	int k = snprintf(s_in, sizeof(s_in), "POST /update HTTP/1.1\r\nHost: langpib-34f0.local\r\nContent-Type: application/octet-stream\r\n");
	if (pw)
		k += snprintf(&s_in[k], sizeof(s_in) - (size_t)k, "X-Update-Password:  %s \r\n", pw);
	if (len)
		k += snprintf(&s_in[k], sizeof(s_in) - (size_t)k, "Content-Length: %u\r\n", (unsigned)n);
	k += snprintf(&s_in[k], sizeof(s_in) - (size_t)k, "\r\n");
	memcpy(&s_in[k], data, n);
	s_begins = s_dones = 0;
	s_end_result = HTTP_UPD_OK;
	return run_in((uint32_t)k + n, chunk, 1460, 1000000);
}

static uint8_t s_blob[150000];

static void test_update(void)
{
	char r[512];
	for (unsigned i = 0; i < sizeof(s_blob); i++)
		s_blob[i] = (uint8_t)(i * 7u + (i >> 9));

	setup();
	s_upd_on = false; /* a board without it: no route, no form */
	assert(!strncmp(upload("pw", s_blob, 100, true, 64), "HTTP/1.1 404", 12) && !s_begins);
	assert(!strstr(get("GET / HTTP/1.1\r\n\r\n", 1000, 100000), "Firmware update"));

	s_upd_on = true; /* no password set: open, the form has no password field */
	const char *home = get("GET / HTTP/1.1\r\n\r\n", 1000, 100000);
	assert(strstr(home, "id=\"fw\"") && !strstr(home, "type=\"password\""));
	assert(!strncmp(get("GET /update HTTP/1.1\r\n\r\n", 100, 9999), "HTTP/1.1 405", 12));
	strcpy(r, upload(NULL, s_blob, 5000, true, 333)); /* no header */
	assert(!strncmp(r, "HTTP/1.1 200", 12) && strstr(r, "staged") && (s_begins == 1) && (s_dones == 1));
	assert((s_img_len == 5000) && !memcmp(s_img, s_blob, 5000));
	assert(!strncmp(upload("anything", s_blob, 5000, true, 333), "HTTP/1.1 200", 12)); /* a header is ignored */

	char line[] = "net password Op3n-Sesame";
	char *argv[3] = { "net", "password", &line[13] };
	netcfg_command(3, argv, r, sizeof(r), true);
	assert(strstr(get("GET / HTTP/1.1\r\n\r\n", 1000, 100000), "type=\"password\"")); /* the form asks for it */

	s_now = 5000;
	strcpy(r, upload("wrong", s_blob, 5000, true, 333));
	assert(!strncmp(r, "HTTP/1.1 403", 12) && strstr(r, "wrong password") && !s_begins && (s_in_pos == s_in_len));
	strcpy(r, upload("Op3n-Sesame", s_blob, 5000, true, 333)); /* within a second of a wrong one */
	assert(!strncmp(r, "HTTP/1.1 429", 12) && !s_begins);
	s_now += 1000;
	assert(!strncmp(upload(NULL, s_blob, 5000, true, 333), "HTTP/1.1 403", 12) && !s_begins); /* no header */
	s_now += 1000;

	for (uint32_t chunk = 1; chunk < 3000; chunk = chunk * 5 + 3) { /* whole image, any read sizes */
		strcpy(r, upload("Op3n-Sesame", s_blob, (chunk == 1) ? 3000 : sizeof(s_blob), true, chunk));
		assert(!strncmp(r, "HTTP/1.1 200", 12) && strstr(r, "staged") && (s_begins == 1) && (s_dones == 1));
		assert((s_img_len == ((chunk == 1) ? 3000 : sizeof(s_blob))) && !memcmp(s_img, s_blob, s_img_len));
		assert(content_length(r) == strlen(body_of(r)));
	}

	assert(!strncmp(upload("Op3n-Sesame", s_blob, 5000, false, 333), "HTTP/1.1 411", 12) && !s_begins);
	s_img_max = 1000;
	assert(!strncmp(upload("Op3n-Sesame", s_blob, 5000, true, 333), "HTTP/1.1 413", 12) && !s_dones);
	s_img_max = sizeof(s_img);
	s_write_fail_at = 2500;
	strcpy(r, upload("Op3n-Sesame", s_blob, 5000, true, 333));
	assert(!strncmp(r, "HTTP/1.1 500", 12) && !s_dones && (s_img_len < 2500) && (s_in_pos == s_in_len));
	s_write_fail_at = -1;

	/* the board refuses the image at the end (CRC): no install */
	int k = snprintf(s_in, sizeof(s_in), "POST /update HTTP/1.1\r\nx-update-password: Op3n-Sesame\r\ncontent-length: 4000\r\n\r\n");
	memcpy(&s_in[k], s_blob, 4000);
	s_dones = 0;
	s_end_result = HTTP_UPD_BAD_IMAGE;
	strcpy(r, run_in((uint32_t)k + 4000, 100, 1460, 1000000));
	assert(!strncmp(r, "HTTP/1.1 400", 12) && strstr(r, "not a valid image") && !s_dones);
}

/* a second upload takes over from one in progress; the first is answered 409 and never installed */
static void test_update_superseded(void)
{
	static http_t a;
	char r[512];
	setup();
	s_upd_on = true;
	char line[] = "abc";
	char *argv[3] = { "net", "password", line };
	netcfg_command(3, argv, r, sizeof(r), true);

	int k = snprintf(s_in, sizeof(s_in), "POST /update HTTP/1.1\r\nX-Update-Password: abc\r\nContent-Length: 6000\r\n\r\n");
	memcpy(&s_in[k], s_blob, 3000); /* half the body for now */
	s_in_len = (uint32_t)k + 3000;
	s_in_pos = s_out_len = 0;
	s_closed = 0;
	s_chunk = 500;
	s_room = 1460;
	http_init(&a, &s_port);
	for (int i = 0; i < 100; i++)
		http_run(&a);
	assert(s_img_len == 3000 && !s_out_len);
	uint32_t a_pos = s_in_pos;

	strcpy(r, upload("abc", s_blob, 2000, true, 700)); /* another connection, all of it */
	assert(!strncmp(r, "HTTP/1.1 200", 12) && (s_dones == 1) && (s_img_len == 2000));

	memcpy(s_in, s_blob, 3000); /* the rest of the first */
	s_in_len = 3000;
	s_in_pos = s_out_len = 0;
	(void)a_pos;
	s_closed = 0;
	s_dones = 0;
	for (int i = 0; (i < 100) && !s_closed; i++) {
		s_room = 1460;
		http_run(&a);
	}
	s_out[s_out_len] = '\0';
	assert(s_closed && !strncmp(s_out, "HTTP/1.1 409", 12) && !s_dones && (s_img_len == 2000));
}

static void test_searching(void)
{
	setup();
	s_ready = false; /* no instrument yet: the page and the update form, no ID or VISA strings */
	const char *r = get("GET / HTTP/1.1\r\nHost: langpib-34f0.local\r\n\r\n", 1000, 100000);
	assert(!strncmp(r, "HTTP/1.1 200 OK\r\n", 17) && (content_length(r) == strlen(body_of(r))));
	assert(strstr(r, "Searching for an instrument") && !strstr(r, "ACME") && !strstr(r, "::INSTR"));
	assert(strstr(r, "02:AB:00:12:34:F0"));
	s_ready = true;
}

/* a POST only from the adapter's own page: another site's Origin, a rebound Host or Origin "null" get 403 */
static void test_same_site(void)
{
	static char req[1024];
	static const char *const bad[] = { "Origin: http://evil.example\r\n", "Origin: null\r\n",
	                                   "Origin: https://langpib-34f0.local\r\n", "Host: evil.example\r\n",
	                                   "Host: langpib-34f0.local.evil.example\r\n", "Host: 192.168.1.230\r\n" };
	static const char *const good[] = { "Origin: http://langpib-34f0.local\r\nHost: langpib-34f0.local\r\n",
	                                    "Origin: http://192.168.1.23:80\r\nHost: 192.168.1.23:80\r\n",
	                                    "Host: LANGPIB-34F0\r\n", "" };
	const char *form = "mode=static&ip=10.0.0.9&mask=255.255.255.0";
	setup();
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		snprintf(req, sizeof(req), "POST /net HTTP/1.1\r\n%sContent-Length: %u\r\n\r\n%s", bad[i],
		         (unsigned)strlen(form), form);
		const char *r = get(req, 7, 700);
		assert(!strncmp(r, "HTTP/1.1 403", 12) && strstr(r, "not from the adapter's own page"));
		assert(settings.net_mode != NET_MODE_STATIC && (s_in_pos == s_in_len)); /* nothing applied, body read */
	}
	for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
		snprintf(req, sizeof(req), "POST /net HTTP/1.1\r\n%sContent-Length: %u\r\n\r\n%s", good[i],
		         (unsigned)strlen(form), form);
		assert(strstr(get(req, 7, 700), "msg ok") && (settings.net_ip[3] == 9));
		settings.net_mode = NET_MODE_DHCP;
	}
	/* an open update from another site's script (no-cors fetch: no preflight) */
	s_upd_on = true;
	int k = snprintf(s_in, sizeof(s_in), "POST /update HTTP/1.1\r\nHost: langpib-34f0.local\r\n"
	                                     "Origin: http://evil.example\r\nContent-Length: 3000\r\n\r\n");
	memcpy(&s_in[k], s_blob, 3000);
	s_begins = 0;
	const char *r = run_in((uint32_t)k + 3000, 500, 1460, 1000000);
	assert(!strncmp(r, "HTTP/1.1 403", 12) && !s_begins && (s_in_pos == s_in_len));
	/* GET is unaffected: the page itself may be read from anywhere */
	assert(!strncmp(get("GET / HTTP/1.1\r\nHost: evil.example\r\n\r\n", 1000, 100000), "HTTP/1.1 200", 12));
}

/* the LXI document while searching: no instrument, no address strings for ports that aren't open */
static void test_ident_searching(void)
{
	setup();
	s_ready = false;
	const char *r = get("GET /lxi/identification HTTP/1.1\r\n\r\n", 50, 999);
	assert(strstr(r, "searching for an instrument") && !strstr(r, "ACME") && !strstr(r, "InstrumentAddressString"));
	assert(strstr(r, "<IPAddress>192.168.1.23</IPAddress>"));
	s_ready = true;
}

/* a wrong password's 1 s wait ends when the password is cleared (open again) */
static void test_lockout_cleared(void)
{
	char r[512], line[] = "abc";
	char *set[3] = { "net", "password", line }, off[] = "off", *clear[3] = { "net", "password", off };
	setup();
	s_upd_on = true;
	s_now = 100000;
	netcfg_command(3, set, r, sizeof(r), true);
	assert(!strncmp(upload("wrong", s_blob, 2000, true, 500), "HTTP/1.1 403", 12));
	netcfg_command(3, clear, r, sizeof(r), true);
	assert(!strncmp(upload(NULL, s_blob, 2000, true, 500), "HTTP/1.1 200", 12) && (s_begins == 1));
}

static void test_chip_row(void)
{
	setup();
	assert(strstr(get("GET / HTTP/1.1\r\n\r\n", 1000, 100000), "<td>Chip</td><td>CH32 &lt;test&gt; &amp; co</td>"));
}

static void test_firmware_version(void)
{
	assert(!strcmp(http_firmware(), "0.0.0")); /* APP_USB_BCD 0x0000 in test/app_config.h */
}

int main(void)
{
	test_home_in_windows();
	test_identification();
	test_errors();
	test_waits_for_request();
	test_form();
	test_update();
	test_update_superseded();
	test_searching();
	test_same_site();
	test_ident_searching();
	test_lockout_cleared();
	test_chip_row();
	test_firmware_version();
	puts("http: all tests passed");
	return 0;
}
