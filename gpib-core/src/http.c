#include <string.h>
#include "http.h"
#include "netcfg.h"
#include "settings.h"
#include "instr.h"
#include "board.h"
#include "app_config.h"

enum { ST_LINE, ST_HEADERS, ST_BODY, ST_UPLOAD, ST_DRAIN, ST_SEND, ST_DONE };
enum { M_GET, M_HEAD, M_POST, M_OTHER };
enum { PG_HOME, PG_IDENT, PG_NOT_FOUND, PG_BAD, PG_METHOD, PG_TOO_LARGE, PG_FORBIDDEN, PG_UPDATE };
enum { MSG_NONE, MSG_SAVED, MSG_BAD_ADDR, MSG_BAD_NAME };
enum { UPD_STAGED, UPD_PW, UPD_WAIT, UPD_LENGTH, UPD_TOO_LARGE, UPD_IMAGE, UPD_FLASH, UPD_SUPERSEDED };

#define PW_WAIT_MS 1000u      /* after a wrong password, every attempt is refused for this long */
#define DRAIN_MAX  (1u << 20) /* a refused upload is read and dropped up to this, so the client sees the reply */

static const http_t *s_upd_owner; /* the connection whose upload the board is taking */
static bool          s_pw_failed;
static uint32_t      s_pw_failed_at;

__attribute__((weak)) bool board_update_supported(void) { return false; }
__attribute__((weak)) int board_update_begin(uint32_t len) { (void)len; return HTTP_UPD_FLASH; }
__attribute__((weak)) int board_update_write(const uint8_t *buf, uint32_t n) { (void)buf; (void)n; return HTTP_UPD_FLASH; }
__attribute__((weak)) int board_update_end(void) { return HTTP_UPD_FLASH; }
__attribute__((weak)) void board_update_done(void) {}
__attribute__((weak)) const char *board_chip_info(void) { return NULL; }

const char *http_firmware(void)
{
#ifdef APP_FW_VERSION
	return APP_FW_VERSION;
#else
	static char v[12];
	unsigned b = APP_USB_BCD, j = ((b >> 12) & 0xFu) * 10u + ((b >> 8) & 0xFu), k = 0;
	if (j >= 10)
		v[k++] = (char)('0' + j / 10u);
	v[k++] = (char)('0' + j % 10u);
	v[k++] = '.';
	v[k++] = (char)('0' + ((b >> 4) & 0xFu));
	v[k++] = '.';
	v[k++] = (char)('0' + (b & 0xFu));
	v[k] = '\0';
	return v;
#endif
}

/* --- output: count the whole response, or pass the window [from, to) of it to the port ------------------------ */

typedef struct {
	const port_t *port; /* NULL: count only */
	uint32_t      pos, from, to, n;
	uint32_t      done; /* bytes the port took; it can take less than write_room() said (lwIP's queue) */
	bool          full;
	char          buf[192];
} out_t;

static void out_flush(out_t *o)
{
	if (o->n && o->port && !o->full) {
		uint32_t room = o->port->write_room(o->port->ctx), k = (o->n < room) ? o->n : room;
		if (k)
			o->port->write(o->port->ctx, o->buf, k);
		o->done += k;
		o->full = (k < o->n);
	}
	o->n = 0;
}

static void outn(out_t *o, const char *s, uint32_t len)
{
	uint32_t a = o->pos, b = o->pos + len;

	o->pos = b;
	if (!o->port || o->full || (b <= o->from) || (a >= o->to))
		return;
	if (a < o->from) {
		s += o->from - a;
		a = o->from;
	}
	if (b > o->to)
		b = o->to;
	for (uint32_t k = b - a; k;) {
		uint32_t c = sizeof(o->buf) - o->n;
		if (c > k)
			c = k;
		memcpy(&o->buf[o->n], s, c);
		o->n += c;
		s += c;
		k -= c;
		if (o->n == sizeof(o->buf))
			out_flush(o);
	}
}

static void outs(out_t *o, const char *s)
{
	outn(o, s, (uint32_t)strlen(s));
}

/* text for HTML and XML: markup characters escaped, control characters dropped */
static void esc(out_t *o, const char *s)
{
	const char *run = s;
	for (; *s; s++) {
		const char *e = (*s == '&') ? "&amp;" : (*s == '<') ? "&lt;" : (*s == '>') ? "&gt;" : (*s == '"') ? "&quot;"
		              : (*s == '\'') ? "&#39;" : ((unsigned char)*s < 0x20) ? "" : NULL;
		if (e) {
			outn(o, run, (uint32_t)(s - run));
			outs(o, e);
			run = s + 1;
		}
	}
	outn(o, run, (uint32_t)(s - run));
}

static void out_u(out_t *o, uint32_t v)
{
	char b[11], *p = &b[10];
	*p = '\0';
	do {
		*--p = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	outs(o, p);
}

static void out_ip(out_t *o, const uint8_t ip[4])
{
	for (int i = 0; i < 4; i++) {
		if (i)
			outs(o, ".");
		out_u(o, ip[i]);
	}
}

/* dotted decimal into buf[16] */
static void ip_text(const uint8_t ip[4], char *buf)
{
	unsigned k = 0;
	for (int i = 0; i < 4; i++) {
		unsigned v = ip[i];
		if (i)
			buf[k++] = '.';
		if (v >= 100)
			buf[k++] = (char)('0' + v / 100u);
		if (v >= 10)
			buf[k++] = (char)('0' + (v / 10u) % 10u);
		buf[k++] = (char)('0' + v % 10u);
	}
	buf[k] = '\0';
}

static void out_mac(out_t *o, const uint8_t mac[6])
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < 6; i++) {
		char b[3] = { hex[mac[i] >> 4], hex[mac[i] & 0xF], ':' };
		outn(o, b, (i < 5) ? 3u : 2u);
	}
}

/* --- pages ----------------------------------------------------------------------------------------------------- */

typedef struct {
	netcfg_status_t st;
	bool            net;
	char            serial[BOARD_SERIAL_LEN + 1];
} info_t;

static void info_get(info_t *in)
{
	memset(in, 0, sizeof(*in));
	in->net = board_net_status(&in->st);
	if (!in->st.hostname)
		in->st.hostname = "";
	board_get_uid_hex(in->serial);
}

static void row(out_t *o, const char *label)
{
	outs(o, "<tr><td>");
	outs(o, label);
	outs(o, "</td><td>");
}

static void visa(out_t *o, const char *host, const char *suffix, const char *note)
{
	outs(o, "<code>TCPIP::");
	esc(o, host);
	outs(o, suffix);
	outs(o, "</code>");
	if (note[0]) {
		outs(o, " <span class=\"sub\">");
		outs(o, note);
		outs(o, "</span>");
	}
	outs(o, "<br>");
}

static void field(out_t *o, const char *label, const char *name, const uint8_t ip[4], bool show)
{
	row(o, label);
	outs(o, "<input name=\"");
	outs(o, name);
	outs(o, "\" value=\"");
	if (show)
		out_ip(o, ip);
	outs(o, "\" inputmode=\"decimal\"></td></tr>\n");
}

static const char s_style[] =
	"<style>\n"
	":root{color-scheme:light dark;--fg:#1d1d1f;--mute:#6e6e73;--bg:#fff;--line:#d2d2d7;--ok:#e3f3e6;--bad:#fbe4e4}\n"
	"@media(prefers-color-scheme:dark){:root{--fg:#e8e8ed;--mute:#98989d;--bg:#1c1c1e;--line:#3a3a3c;"
	"--ok:#1f3a26;--bad:#4a2224}}\n"
	"body{font:15px/1.45 system-ui,sans-serif;color:var(--fg);background:var(--bg);margin:0 auto;max-width:46em;"
	"padding:1em 16px}\n"
	"h1{font-size:1.5em;margin:.3em 0 0}h2{font-size:1.05em;margin:1.6em 0 .3em}.sub{color:var(--mute)}\n"
	"table{border-collapse:collapse;width:100%}td{padding:.3em .4em;border-top:1px solid var(--line);"
	"vertical-align:top;overflow-wrap:anywhere}\n"
	"td:first-child{width:9.5em;color:var(--mute)}code{font-size:.92em}input{font:inherit;padding:.15em .35em;"
	"width:13em;max-width:100%}\n"
	"input[type=radio]{width:auto}button{font:inherit;padding:.3em 1.2em}.msg{padding:.5em .8em;margin-top:1em;"
	"border-radius:6px}\n"
	".ok{background:var(--ok)}.bad{background:var(--bad)}\n"
	"@media(max-width:36em){td:first-child{width:6.5em}code{word-break:break-all}}\n"
	"</style>\n";

/* the .dfu goes up as the raw body, so the board can stream it to flash; the password in a header */
static void update_form(out_t *o)
{
	outs(o, "<h2>Firmware update</h2>\n");
	bool pw = netcfg_password_set(); /* none set (the default): open to this network */
	outs(o, "<form id=\"fw\"><table>\n");
	row(o, "Image");
	outs(o, "<input type=\"file\" name=\"f\" accept=\".dfu\" required></td></tr>\n");
	if (pw) {
		row(o, "Password");
		outs(o, "<input type=\"password\" name=\"p\" required></td></tr>\n");
	}
	outs(o, "</table>\n<p><button>Update</button> <span class=\"sub\" id=\"fws\">The <code>.dfu</code> file, as for USB DFU; "
	        "the adapter restarts to install it.</span></p></form>\n"
	        "<script>document.getElementById('fw').onsubmit=async e=>{e.preventDefault();const f=e.target,"
	        "s=document.getElementById('fws');s.textContent='Uploading...';try{const r=await fetch('/update',"
	        "{method:'POST',headers:f.p?{'X-Update-Password':f.p.value}:{},body:f.f.files[0]});s.textContent=await r.text()}"
	        "catch(x){s.textContent='Failed: '+x}};</script>\n");
}

static void page_home(out_t *o, const http_t *h, const info_t *in)
{
	const char *id = instr_id();
	const char *ipaddr = NULL;
	char ipbuf[16];

	outs(o, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
	        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>");
	esc(o, in->st.hostname[0] ? in->st.hostname : APP_USB_PRODUCT);
	outs(o, "</title>\n");
	outs(o, s_style);
	outs(o, "</head><body>\n<h1>" APP_USB_PRODUCT "</h1><div class=\"sub\">" APP_USB_MANUFACTURER);
	if (in->st.hostname[0]) {
		outs(o, " &middot; ");
		esc(o, in->st.hostname);
		outs(o, ".local");
	}
	outs(o, "</div>\n");
	if (h->msg == MSG_SAVED)
		outs(o, "<div class=\"msg ok\">Saved. The new settings apply at the next reset.</div>\n");
	else if (h->msg == MSG_BAD_ADDR)
		outs(o, "<div class=\"msg bad\">Not saved: the address, mask or gateway isn't valid.</div>\n");
	else if (h->msg == MSG_BAD_NAME)
		outs(o, "<div class=\"msg bad\">Not saved: a hostname is 1-23 letters, digits and inner hyphens.</div>\n");

	if (in->net) { /* the address as text, for the VISA strings */
		ip_text(in->st.ip, ipbuf);
		ipaddr = ipbuf;
	}

	outs(o, "<h2>Instrument</h2><table>\n");
	if (!instr_ready()) { /* still searching: nothing to identify, and the instrument ports aren't open yet */
		row(o, "Status");
		outs(o, "Searching for an instrument. VXI-11, raw SCPI and Prologix open once one answers.</td></tr>\n");
	} else {
		row(o, "GPIB address");
		out_u(o, gpib_addr);
		outs(o, "</td></tr>\n");
		row(o, "Identification");
		if (id[0])
			esc(o, id);
		else
			outs(o, "<span class=\"sub\">none found</span>");
		outs(o, "</td></tr>\n");
		if (ipaddr) {
			row(o, "VISA");
			visa(o, ipaddr, "::inst0::INSTR", "");
			if (in->st.hostname[0]) {
				char host[NETCFG_NAME_MAX + 7];
				strcpy(host, in->st.hostname);
				strcat(host, ".local");
				visa(o, host, "::inst0::INSTR", "");
			}
			visa(o, ipaddr, "::gpib0,<i>N</i>::INSTR", "any GPIB address");
			visa(o, ipaddr, "::5025::SOCKET", "raw SCPI");
			outs(o, "Prologix on TCP port 1234</td></tr>\n");
		}
	}
	outs(o, "</table>\n");

	if (in->net) {
		outs(o, "<h2>Network</h2><table>\n");
		row(o, "IP address");
		out_ip(o, in->st.ip);
		outs(o, in->st.dhcp ? " (DHCP or AutoIP)" : " (static)");
		outs(o, "</td></tr>\n");
		row(o, "Subnet mask");
		out_ip(o, in->st.mask);
		outs(o, "</td></tr>\n");
		row(o, "Gateway");
		out_ip(o, in->st.gw);
		outs(o, "</td></tr>\n");
		row(o, "MAC address");
		out_mac(o, in->st.mac);
		outs(o, "</td></tr>\n</table>\n");
	}

	outs(o, "<h2>Adapter</h2><table>\n");
	row(o, "Serial number");
	outs(o, in->serial);
	outs(o, "</td></tr>\n");
	row(o, "Firmware");
	outs(o, http_firmware());
	outs(o, "</td></tr>\n");
	const char *chip = board_chip_info();
	if (chip) {
		row(o, "Chip");
		esc(o, chip);
		outs(o, "</td></tr>\n");
	}
	row(o, "LXI");
	outs(o, "<a href=\"/lxi/identification\">identification</a></td></tr>\n</table>\n");

	bool fixed = settings.net_mode == NET_MODE_STATIC;
	outs(o, "<h2>Network settings</h2>\n<form method=\"post\" action=\"/net\"><table>\n");
	row(o, "Address");
	outs(o, "<label><input type=\"radio\" name=\"mode\" value=\"dhcp\"");
	outs(o, fixed ? "" : " checked");
	outs(o, "> DHCP, AutoIP without a server</label><br><label><input type=\"radio\" name=\"mode\" value=\"static\"");
	outs(o, fixed ? " checked" : "");
	outs(o, "> Static</label></td></tr>\n");
	field(o, "IP address", "ip", settings.net_ip, fixed);
	field(o, "Subnet mask", "mask", settings.net_mask, fixed);
	field(o, "Gateway", "gw", settings.net_gw, fixed);
	row(o, "Hostname");
	outs(o, "<input name=\"name\" maxlength=\"23\" value=\"");
	esc(o, settings.net_name);
	outs(o, "\" placeholder=\"default\"></td></tr>\n</table>\n"
	        "<p><button>Save</button> <span class=\"sub\">Applies at the next reset: power, <code>++rst</code> or "
	        "<code>!reset</code>.</span></p></form>\n");
	if (board_update_supported())
		update_form(o);
	outs(o, "</body></html>\n");
}

static void page_ident(out_t *o, const info_t *in)
{
	outs(o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	        "<LXIDevice xmlns=\"http://www.lxistandard.org/InstrumentIdentification/1.0\" "
	        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
	        "xsi:schemaLocation=\"http://www.lxistandard.org/InstrumentIdentification/1.0 "
	        "http://www.lxistandard.org/InstrumentIdentification/1.0/LXIIdentification.xsd\">\n"
	        "  <Manufacturer>" APP_USB_MANUFACTURER "</Manufacturer>\n"
	        "  <Model>" APP_USB_PRODUCT "</Model>\n  <SerialNumber>");
	outs(o, in->serial);
	outs(o, "</SerialNumber>\n  <FirmwareRevision>");
	outs(o, http_firmware());
	outs(o, "</FirmwareRevision>\n  <ManufacturerDescription>LAN to GPIB adapter; ");
	if (instr_ready()) {
		outs(o, "GPIB ");
		out_u(o, gpib_addr);
		outs(o, ": ");
		esc(o, instr_id()[0] ? instr_id() : "no instrument found");
	} else {
		outs(o, "searching for an instrument"); /* no VXI-11 or raw SCPI yet: no address strings either */
	}
	outs(o, "</ManufacturerDescription>\n");
	if (in->net) {
		outs(o, "  <HomepageURL>http://");
		out_ip(o, in->st.ip);
		outs(o, "/</HomepageURL>\n");
	}
	outs(o, "  <UserDescription>");
	esc(o, in->st.hostname);
	outs(o, "</UserDescription>\n");
	if (in->net) {
		outs(o, "  <IdentificationURL>http://");
		out_ip(o, in->st.ip);
		outs(o, "/lxi/identification</IdentificationURL>\n"
		        "  <Interface xsi:type=\"NetworkInformation\" InterfaceType=\"LXI\" IPType=\"IPv4\" "
		        "InterfaceName=\"eth0\">\n");
		if (instr_ready()) {
			outs(o, "    <InstrumentAddressString>TCPIP::");
			out_ip(o, in->st.ip);
			outs(o, "::inst0::INSTR</InstrumentAddressString>\n    <InstrumentAddressString>TCPIP::");
			out_ip(o, in->st.ip);
			outs(o, "::5025::SOCKET</InstrumentAddressString>\n");
		}
		outs(o, "    <Hostname>");
		esc(o, in->st.hostname);
		outs(o, ".local</Hostname>\n    <IPAddress>");
		out_ip(o, in->st.ip);
		outs(o, "</IPAddress>\n    <SubnetMask>");
		out_ip(o, in->st.mask);
		outs(o, "</SubnetMask>\n    <MACAddress>");
		out_mac(o, in->st.mac);
		outs(o, "</MACAddress>\n    <Gateway>");
		out_ip(o, in->st.gw);
		outs(o, "</Gateway>\n    <DHCPEnabled>");
		outs(o, in->st.dhcp ? "true" : "false");
		outs(o, "</DHCPEnabled>\n    <AutoIPEnabled>");
		outs(o, in->st.dhcp ? "true" : "false");
		outs(o, "</AutoIPEnabled>\n  </Interface>\n");
	}
	outs(o, "  <LXIVersion>1.6</LXIVersion>\n</LXIDevice>\n");
}

static const struct {
	const char *status, *text;
} s_upd[] = {
	[UPD_STAGED]     = { "200 OK", "OK: image checked and staged; the adapter restarts to install it\n" },
	[UPD_PW]         = { "403 Forbidden", "wrong password\n" },
	[UPD_WAIT]       = { "429 Too Many Requests", "wait a second after a wrong password\n" },
	[UPD_LENGTH]     = { "411 Length Required", "Content-Length required\n" },
	[UPD_TOO_LARGE]  = { "413 Content Too Large", "the image doesn't fit the update area\n" },
	[UPD_IMAGE]      = { "400 Bad Request", "not a valid image (header, length or CRC): nothing changed\n" },
	[UPD_FLASH]      = { "500 Internal Server Error", "flash write failed: nothing changed\n" },
	[UPD_SUPERSEDED] = { "409 Conflict", "another upload started: this one was dropped\n" },
};

static void body(out_t *o, const http_t *h, const info_t *in)
{
	switch (h->page) {
	case PG_UPDATE:
		outs(o, s_upd[h->msg].text);
		break;
	case PG_HOME:
		page_home(o, h, in);
		break;
	case PG_IDENT:
		page_ident(o, in);
		break;
	case PG_NOT_FOUND:
		outs(o, "not found\n");
		break;
	case PG_METHOD:
		outs(o, "method not allowed\n");
		break;
	case PG_TOO_LARGE:
		outs(o, "request too large\n");
		break;
	case PG_FORBIDDEN:
		outs(o, "refused: not from the adapter's own page\n");
		break;
	default:
		outs(o, "bad request\n");
		break;
	}
}

static void response(out_t *o, const http_t *h, const info_t *in)
{
	static const char *const status[] = { "200 OK", "200 OK", "404 Not Found", "400 Bad Request",
	                                      "405 Method Not Allowed", "413 Content Too Large", "403 Forbidden" };
	outs(o, "HTTP/1.1 ");
	outs(o, (h->page == PG_UPDATE) ? s_upd[h->msg].status : status[h->page]);
	outs(o, "\r\nContent-Type: ");
	outs(o, (h->page == PG_HOME) ? "text/html" : (h->page == PG_IDENT) ? "text/xml" : "text/plain");
	outs(o, "; charset=utf-8\r\nContent-Length: ");
	out_u(o, h->body_size);
	if (h->page == PG_METHOD)
		outs(o, "\r\nAllow: GET, HEAD, POST");
	outs(o, "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
	if (h->method != M_HEAD)
		body(o, h, in);
}

/* --- the settings form ----------------------------------------------------------------------------------------- */

static int hexval(char c)
{
	return ((c >= '0') && (c <= '9')) ? c - '0' : ((c >= 'a') && (c <= 'f')) ? c - 'a' + 10
	     : ((c >= 'A') && (c <= 'F')) ? c - 'A' + 10 : -1;
}

/* the decoded value of name in an x-www-form-urlencoded body, cut to max - 1; false if absent */
static bool form_get(const char *b, const char *name, char *v, unsigned max)
{
	size_t n = strlen(name);
	for (const char *p = b; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
		if (strncmp(p, name, n) || (p[n] != '='))
			continue;
		unsigned k = 0;
		for (p += n + 1; *p && (*p != '&'); p++) {
			char c = *p;
			if (c == '+') {
				c = ' ';
			} else if ((c == '%') && (hexval(p[1]) >= 0) && (hexval(p[2]) >= 0)) {
				c = (char)(hexval(p[1]) * 16 + hexval(p[2]));
				p += 2;
			}
			if (k + 1 < max)
				v[k++] = c;
		}
		v[k] = '\0';
		return true;
	}
	return false;
}

static void trim(char *s)
{
	size_t n = strlen(s), i = 0;
	while ((n > 0) && (s[n - 1] == ' '))
		s[--n] = '\0';
	while (s[i] == ' ')
		i++;
	memmove(s, &s[i], n - i + 1);
}

static uint8_t apply_form(const char *b)
{
	char mode[8] = "", ip[16] = "", mask[16] = "", gw[16] = "", name[NETCFG_NAME_MAX + 2] = "";
	uint8_t a[4], m[4];
	bool has_name = form_get(b, "name", name, sizeof(name));

	form_get(b, "mode", mode, sizeof(mode));
	form_get(b, "ip", ip, sizeof(ip));
	form_get(b, "mask", mask, sizeof(mask));
	form_get(b, "gw", gw, sizeof(gw));
	trim(ip);
	trim(mask);
	trim(gw);
	trim(name);
	if (has_name && name[0] && !netcfg_name_valid(name))
		return MSG_BAD_NAME;
	if (!strcmp(mode, "static")) {
		char *argv[5] = { "net", "static", ip, mask, gw };
		char r[4];
		if (!netcfg_parse_ip(ip, a) || !netcfg_parse_ip(mask, m))
			return MSG_BAD_ADDR;
		netcfg_command(gw[0] ? 5 : 4, argv, r, sizeof(r), false);
		if ((settings.net_mode != NET_MODE_STATIC) || memcmp(settings.net_ip, a, 4) || memcmp(settings.net_mask, m, 4))
			return MSG_BAD_ADDR; /* netcfg refused it */
	} else if (!strcmp(mode, "dhcp")) {
		char *argv[2] = { "net", "dhcp" };
		char r[4];
		netcfg_command(2, argv, r, sizeof(r), false);
	} else {
		return MSG_BAD_ADDR;
	}
	if (has_name) {
		char *argv[3] = { "net", "name", name[0] ? name : "default" };
		char r[4];
		netcfg_command(3, argv, r, sizeof(r), false);
	}
	return MSG_SAVED;
}

/* --- request --------------------------------------------------------------------------------------------------- */

void http_init(http_t *h, const port_t *port)
{
	if (s_upd_owner == h) /* the connection went: its upload is abandoned */
		s_upd_owner = NULL;
	memset(h, 0, sizeof(*h));
	h->port = port;
}

void http_reset(http_t *h)
{
	http_init(h, h->port);
}

static bool starts_ci(const char *s, const char *prefix)
{
	for (; *prefix; s++, prefix++) {
		char c = *s;
		if ((c >= 'A') && (c <= 'Z'))
			c = (char)(c + 'a' - 'A');
		if (c != *prefix)
			return false;
	}
	return true;
}

static void request_line(http_t *h)
{
	char *sp = strchr(h->line, ' ');
	char *path = sp ? sp + 1 : NULL;
	char *end = path ? strchr(path, ' ') : NULL;

	h->state = ST_HEADERS;
	if (!end || (h->line_len >= HTTP_LINE_MAX - 1)) {
		h->page = PG_BAD;
		return;
	}
	*sp = *end = '\0';
	char *q = strchr(path, '?');
	if (q)
		*q = '\0';
	h->method = !strcmp(h->line, "GET") ? M_GET : !strcmp(h->line, "HEAD") ? M_HEAD : !strcmp(h->line, "POST") ? M_POST
	          : M_OTHER;
	bool home = !strcmp(path, "/") || !strcmp(path, "/index.html");
	bool ident = !strcmp(path, "/lxi/identification") || !strcmp(path, "/lxi/identification/");
	bool net = !strcmp(path, "/net");
	bool upd = !strcmp(path, "/update") && board_update_supported();
	if (h->method == M_OTHER)
		h->page = PG_METHOD;
	else if (h->method == M_POST)
		h->page = net ? PG_HOME : upd ? PG_UPDATE : (home || ident) ? PG_METHOD : PG_NOT_FOUND;
	else
		h->page = home ? PG_HOME : ident ? PG_IDENT : (net || upd) ? PG_METHOD : PG_NOT_FOUND;
}

static uint8_t upd_msg(int r)
{
	return (r == HTTP_UPD_TOO_LARGE) ? UPD_TOO_LARGE : (r == HTTP_UPD_BAD_IMAGE) ? UPD_IMAGE : UPD_FLASH;
}

/* upload bytes: to the board while this connection owns the upload, else dropped; returns how many were used */
static uint32_t update_body(http_t *h, const uint8_t *p, uint32_t n)
{
	uint32_t k = (n < h->left) ? n : h->left;
	int r;

	if ((h->state == ST_UPLOAD) && k) {
		if (s_upd_owner != h) {
			h->msg = UPD_SUPERSEDED;
			h->state = ST_DRAIN;
		} else if ((r = board_update_write(p, k)) != HTTP_UPD_OK) {
			h->msg = upd_msg(r);
			h->state = ST_DRAIN;
			s_upd_owner = NULL;
		}
	}
	h->left -= k;
	if (!h->left) {
		if (h->state == ST_UPLOAD) {
			r = (s_upd_owner == h) ? board_update_end() : HTTP_UPD_FLASH;
			h->msg = (s_upd_owner != h) ? UPD_SUPERSEDED : (r == HTTP_UPD_OK) ? UPD_STAGED : upd_msg(r);
			s_upd_owner = (s_upd_owner == h) ? NULL : s_upd_owner;
		}
		h->state = ST_SEND;
	}
	return k;
}

/* end of an upload's headers: the password if one is set, then the board takes the body; a refused body is read and dropped */
static void update_start(http_t *h)
{
	uint32_t now = board_millis();
	int r;

	bool locked = netcfg_password_set(); /* no password (the default): open to this network */

	if (locked && s_pw_failed && ((now - s_pw_failed_at) < PW_WAIT_MS)) {
		h->msg = UPD_WAIT;
	} else if (locked && !h->pw_ok) {
		h->msg = UPD_PW;
		s_pw_failed = true;
		s_pw_failed_at = now;
	} else if (!h->has_len) {
		h->msg = UPD_LENGTH;
	} else if ((r = board_update_begin(h->content_len)) != HTTP_UPD_OK) {
		h->msg = upd_msg(r);
	} else {
		s_upd_owner = h;
		h->left = h->content_len;
		h->state = ST_UPLOAD;
		if (!h->left)
			update_body(h, NULL, 0);
		return;
	}
	h->left = h->content_len;
	h->state = (h->has_len && h->left && (h->left <= DRAIN_MAX)) ? ST_DRAIN : ST_SEND;
}

/* a Host or Origin host[:port] names this adapter: its address, <hostname> or <hostname>.local (case aside) */
static bool is_us(const char *v, size_t n)
{
	netcfg_status_t st = { 0 };
	char ip[16];
	const char *colon = memchr(v, ':', n);

	if (colon)
		n = (size_t)(colon - v);
	if (!board_net_status(&st))
		return false;
	ip_text(st.ip, ip);
	if ((n == strlen(ip)) && !memcmp(v, ip, n))
		return true;
	size_t hn = st.hostname ? strlen(st.hostname) : 0;
	if (!hn || ((n != hn) && (n != hn + 6)))
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = v[i], e = (i < hn) ? st.hostname[i] : ".local"[i - hn];
		if ((c >= 'A') && (c <= 'Z'))
			c = (char)(c + 'a' - 'A');
		if ((e >= 'A') && (e <= 'Z'))
			e = (char)(e + 'a' - 'A');
		if (c != e)
			return false;
	}
	return true;
}

/* a POST is refused when a browser says it comes from elsewhere: another site's page (Origin) or a rebound name (Host) */
static bool same_site(const http_t *h)
{
	if (h->host[0] && !is_us(h->host, strlen(h->host)))
		return false;
	if (!h->origin[0])
		return true;
	return !strncmp(h->origin, "http://", 7) && is_us(&h->origin[7], strlen(&h->origin[7]));
}

static void keep(char *dst, size_t max, char *v)
{
	trim(v);
	if (strlen(v) >= max)
		v = "?"; /* too long for any of our names: never matches */
	strcpy(dst, v);
}

static void header_line(http_t *h)
{
	if (h->line_len == 0) { /* end of the headers */
		h->state = ST_SEND;
		if ((h->method == M_POST) && (h->page != PG_METHOD) && (h->page != PG_NOT_FOUND) && !same_site(h)) {
			h->page = PG_FORBIDDEN;
			h->left = h->content_len; /* read and drop the body, so the client gets the reply */
			h->state = (h->has_len && h->left && (h->left <= DRAIN_MAX)) ? ST_DRAIN : ST_SEND;
			return;
		}
		if ((h->method == M_POST) && (h->page == PG_HOME)) {
			if (h->content_len > HTTP_BODY_MAX)
				h->page = PG_TOO_LARGE;
			else if (h->content_len)
				h->state = ST_BODY;
			else
				h->msg = apply_form("");
		} else if (h->page == PG_UPDATE) {
			update_start(h);
		}
		return;
	}
	if (starts_ci(h->line, "content-length:")) {
		uint32_t v = 0;
		for (const char *p = &h->line[15]; *p; p++)
			if ((*p >= '0') && (*p <= '9'))
				v = (v < 100000000u) ? v * 10u + (uint32_t)(*p - '0') : 0xFFFFFFFFu;
		h->content_len = v;
		h->has_len = true;
	} else if (starts_ci(h->line, "x-update-password:")) {
		char *v = &h->line[18];
		trim(v);
		h->pw_ok = netcfg_password_ok(v);
	} else if (starts_ci(h->line, "host:")) {
		keep(h->host, sizeof(h->host), &h->line[5]);
	} else if (starts_ci(h->line, "origin:")) {
		keep(h->origin, sizeof(h->origin), &h->line[7]);
	}
}

static void take(http_t *h, char c)
{
	if (h->state == ST_BODY) {
		h->body[h->body_len++] = c;
		if (h->body_len >= h->content_len) {
			h->body[h->body_len] = '\0';
			h->msg = apply_form(h->body);
			h->state = ST_SEND;
		}
		return;
	}
	if (c == '\r')
		return;
	if (c != '\n') {
		if (h->line_len < HTTP_LINE_MAX - 1)
			h->line[h->line_len++] = c;
		return;
	}
	h->line[h->line_len] = '\0';
	if (h->state == ST_LINE) {
		if (h->line_len) /* blank lines before a request are allowed */
			request_line(h);
	} else {
		header_line(h);
	}
	h->line_len = 0;
}

void http_run(http_t *h)
{
	uint8_t buf[256];
	uint32_t n;

	while ((h->state < ST_SEND) && (n = h->port->read(h->port->ctx, buf, sizeof(buf))))
		for (uint32_t i = 0; (i < n) && (h->state < ST_SEND);)
			if (h->state >= ST_UPLOAD)
				i += update_body(h, &buf[i], n - i);
			else
				take(h, (char)buf[i++]);
	if (h->state != ST_SEND)
		return;

	info_t in;
	info_get(&in);
	if (!h->total) {
		out_t c = { 0 };
		body(&c, h, &in);
		h->body_size = c.pos;
		c.pos = 0;
		response(&c, h, &in);
		h->total = c.pos;
	}
	uint32_t room = h->port->write_room(h->port->ctx);
	if (room) {
		out_t o = { .port = h->port, .from = h->sent, .to = h->sent + room };
		response(&o, h, &in);
		out_flush(&o);
		h->sent += o.done; /* the next call renders again from here */
		h->port->flush(h->port->ctx);
	}
	if (h->sent >= h->total) {
		h->state = ST_DONE;
		if (h->port->close)
			h->port->close(h->port->ctx);
		if ((h->page == PG_UPDATE) && (h->msg == UPD_STAGED))
			board_update_done();
	}
}
