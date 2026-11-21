/* VXI-11 server on the host: the firmware's vxi11.c, gpib_bus.c and instr.c behind POSIX sockets, with a
 * simulated SCPI instrument on the simulated bus; plus raw SCPI on TCP 5025 and Prologix on 1234 (scpi_raw.c,
 * prologix.c) and the web page on 8080 (http.c). For testing clients without hardware (make host).
 *   build/host/vxi11_host [-p portmap_port] [-w web_port]   (default 111: macOS lets users bind it on INADDR_ANY) */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "vxi11.h"
#include "scpi_raw.h"
#include "prologix.h"
#include "sockets_lwip.h"
#include "trace.h"
#include "gpib_bus.h"
#include "gpib_sim.h"
#include "sim_scpi.h"
#include "instr.h"
#include "settings.h"
#include "board.h"
#include "http.h"
#include "netcfg.h"

/* --- board fakes ------------------------------------------------------------------------------------- */

uint8_t _settings_start[2 * BOARD_FLASH_PAGE_SIZE];
void board_flash_erase(const void *page) { memset((void *)page, 0xFF, BOARD_FLASH_PAGE_SIZE); }
void board_flash_write(const void *dst, const void *src, unsigned len) { memcpy((void *)dst, src, len); }
void board_get_uid_hex(char *buf) { strcpy(buf, "0123456789ABCDEF01234567"); }

/* the web page's network status: the loopback address and a made-up MAC */
bool board_net_status(netcfg_status_t *st)
{
	static const uint8_t ip[4] = { 127, 0, 0, 1 }, mask[4] = { 255, 0, 0, 0 };
	static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };
	memcpy(st->ip, ip, 4);
	memcpy(st->mask, mask, 4);
	memcpy(st->mac, mac, 6);
	st->hostname = settings.net_name[0] ? settings.net_name : "gpib-core-host";
	st->dhcp = settings.net_mode != NET_MODE_STATIC;
	return true;
}
void board_led_mode(led_mode_t mode) { (void)mode; }
void board_led_busy(bool busy) { (void)busy; }
void board_enter_bootloader(void) { exit(0); }
void board_reset(void) { exit(0); }
static bool s_trace_on;
void trace_enable(bool on) { s_trace_on = on; }
bool trace_enabled(void) { return s_trace_on; }

/* --- sockets ----------------------------------------------------------------------------------------- */

#define MAX_FDS (3 + 1 + 2 + 1 + VXI_MAX_CONNS)

typedef struct {
	int         fd;
	vxi_conn_t *vc;
	uint8_t     in[8192];
	uint32_t    in_len;
} conn_t;

static conn_t s_conns[VXI_MAX_CONNS];

static int listen_on(int type, uint16_t port)
{
	int fd = socket(AF_INET, type, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if ((fd < 0) || bind(fd, (struct sockaddr *)&a, sizeof(a)) || ((type == SOCK_STREAM) && listen(fd, 4))) {
		fprintf(stderr, "port %u: %s\n", port, strerror(errno));
		exit(1);
	}
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static void drop(conn_t *k)
{
	vxi_close(k->vc);
	close(k->fd);
	k->fd = -1;
}

static void accept_on(int lfd, vxi_channel_t ch)
{
	int fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		return;
	vxi_conn_t *vc = vxi_open(ch);
	for (int i = 0; vc && (i < VXI_MAX_CONNS); i++)
		if (s_conns[i].fd < 0) {
			fcntl(fd, F_SETFL, O_NONBLOCK);
			s_conns[i] = (conn_t){ .fd = fd, .vc = vc };
			return;
		}
	if (vc)
		vxi_close(vc);
	close(fd);
}

static void service(conn_t *k, short revents)
{
	if (revents & (POLLIN | POLLHUP | POLLERR)) {
		ssize_t n = read(k->fd, &k->in[k->in_len], sizeof(k->in) - k->in_len);
		if ((n == 0) || ((n < 0) && (errno != EAGAIN))) {
			drop(k);
			return;
		}
		if (n > 0)
			k->in_len += (uint32_t)n;
	}
	if (k->in_len) {
		uint32_t used = vxi_rx(k->vc, k->in, k->in_len);
		memmove(k->in, &k->in[used], k->in_len - used);
		k->in_len -= used;
	}
	uint32_t len;
	const uint8_t *p = vxi_tx(k->vc, &len);
	if (p) {
		ssize_t n = send(k->fd, p, len, 0);
		if (n > 0)
			vxi_tx_done(k->vc, (uint32_t)n);
	}
	if (vxi_failed(k->vc))
		drop(k);
}

/* --- raw SCPI (5025) and Prologix (1234): one client each, a new one replaces the old ------------------------ */

typedef struct {
	int     lfd, fd;
	uint8_t in[4096];
	uint32_t in_len, in_pos;
	port_t  port;
	void  (*reset)(void *proto);
	void   *proto;
} stream_t;

static uint32_t st_read(void *ctx, uint8_t *buf, uint32_t max)
{
	stream_t *t = ctx;
	if ((t->in_pos == t->in_len) && (t->fd >= 0)) {
		ssize_t n = read(t->fd, t->in, sizeof(t->in));
		if ((n == 0) || ((n < 0) && (errno != EAGAIN))) {
			close(t->fd);
			t->fd = -1;
		}
		t->in_len = (n > 0) ? (uint32_t)n : 0;
		t->in_pos = 0;
	}
	uint32_t n = 0;
	while ((n < max) && (t->in_pos < t->in_len))
		buf[n++] = t->in[t->in_pos++];
	return n;
}

static uint32_t st_write_room(void *ctx)
{
	(void)ctx;
	return 4096;
}

static void st_write(void *ctx, const void *buf, uint32_t n)
{
	stream_t *t = ctx;
	if ((t->fd >= 0) && n)
		send(t->fd, buf, n, 0);
}

static void st_flush(void *ctx)
{
	(void)ctx;
}

static void st_close(void *ctx)
{
	stream_t *t = ctx;
	if (t->fd >= 0)
		close(t->fd);
	t->fd = -1;
}

static void stream_accept(stream_t *t, int lfd)
{
	int fd = accept(lfd, NULL, NULL);
	if (fd < 0)
		return;
	if (t->fd >= 0)
		close(t->fd);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	t->fd = fd;
	t->in_len = t->in_pos = 0;
	t->reset(t->proto);
}

static scpi_raw_t s_raw;
static prologix_t s_px;
static void raw_reset(void *proto) { scpi_raw_reset(proto); }
static void px_reset(void *proto) { prologix_reset(proto); }
static stream_t s_streams[2] = {
	{ .fd = -1, .reset = raw_reset, .proto = &s_raw },
	{ .fd = -1, .reset = px_reset, .proto = &s_px },
};

/* the web page: a few clients on one listening socket, a new one replaces the oldest */
#define WEB_CONNS 3
static http_t   s_http[WEB_CONNS];
static uint32_t s_web_seq[WEB_CONNS], s_web_accepts;
static void web_reset(void *proto) { http_reset(proto); }
static stream_t s_web[WEB_CONNS] = {
	{ .fd = -1, .reset = web_reset, .proto = &s_http[0] },
	{ .fd = -1, .reset = web_reset, .proto = &s_http[1] },
	{ .fd = -1, .reset = web_reset, .proto = &s_http[2] },
};

static void web_accept(int lfd)
{
	int k = 0;
	for (int i = 0; i < WEB_CONNS; i++)
		if ((s_web[i].fd < 0) || ((s_web[k].fd >= 0) && (s_web_seq[i] < s_web_seq[k])))
			k = i;
	s_web_seq[k] = ++s_web_accepts;
	stream_accept(&s_web[k], lfd);
}

static void idle(void)
{
}

int main(int argc, char **argv)
{
	uint16_t pmap_port = VXI_PORTMAP_PORT, web_port = 8080;
	int opt;

	while ((opt = getopt(argc, argv, "p:w:")) != -1)
		if (opt == 'p')
			pmap_port = (uint16_t)atoi(optarg);
		else if (opt == 'w')
			web_port = (uint16_t)atoi(optarg);
	signal(SIGPIPE, SIG_IGN);

	memset(_settings_start, 0xFF, sizeof(_settings_start));
	settings_init();
	sim_reset();
	sim_dev_t *d = sim_add(5, -1);
	d->on_message = sim_scpi;
	d->status = 0;
	gpib_init();
	instr_init();
	instr_startup(idle);
	vxi_init();
	printf("instrument at GPIB %u: %s\n", gpib_addr, instr_id());

	int lfd[4] = { listen_on(SOCK_STREAM, pmap_port), listen_on(SOCK_STREAM, VXI_CORE_PORT),
	               listen_on(SOCK_STREAM, VXI_ABORT_PORT), listen_on(SOCK_DGRAM, pmap_port) };
	static const vxi_channel_t chans[3] = { VXI_CH_PORTMAP, VXI_CH_CORE, VXI_CH_ABORT };
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		s_conns[i].fd = -1;
	s_streams[0].lfd = listen_on(SOCK_STREAM, SOCKETS_RAW_PORT);
	s_streams[1].lfd = listen_on(SOCK_STREAM, SOCKETS_PROLOGIX_PORT);
	for (int i = 0; i < 2; i++)
		s_streams[i].port = (port_t){ st_read, st_write_room, st_write, st_flush, &s_streams[i], NULL };
	scpi_raw_init(&s_raw, &s_streams[0].port);
	prologix_init(&s_px, &s_streams[1].port, GPIB_OWNER_PX_NET);
	int web_lfd = listen_on(SOCK_STREAM, web_port);
	for (int i = 0; i < WEB_CONNS; i++) {
		s_web[i].port = (port_t){ st_read, st_write_room, st_write, st_flush, &s_web[i], st_close };
		http_init(&s_http[i], &s_web[i].port);
	}
	printf("VXI-11 on TCPIP::127.0.0.1::inst0::INSTR (portmapper %u, core %u, abort %u); raw SCPI on %u, Prologix on %u, "
	       "web page on http://127.0.0.1:%u/\n",
	       pmap_port, VXI_CORE_PORT, VXI_ABORT_PORT, SOCKETS_RAW_PORT, SOCKETS_PROLOGIX_PORT, web_port);
	fflush(stdout);

	for (;;) {
		struct pollfd pf[MAX_FDS];
		conn_t *owner[MAX_FDS];
		int n = 0;
		for (int i = 0; i < 4; i++)
			pf[n++] = (struct pollfd){ .fd = lfd[i], .events = POLLIN };
		for (int i = 0; i < 2; i++)
			pf[n++] = (struct pollfd){ .fd = s_streams[i].lfd, .events = POLLIN };
		pf[n++] = (struct pollfd){ .fd = web_lfd, .events = POLLIN };
		for (int i = 0; i < VXI_MAX_CONNS; i++)
			if (s_conns[i].fd >= 0) {
				uint32_t len;
				owner[n] = &s_conns[i];
				pf[n++] = (struct pollfd){ .fd = s_conns[i].fd,
				                           .events = (short)(POLLIN | (vxi_tx(s_conns[i].vc, &len) ? POLLOUT : 0)) };
			}
		poll(pf, (nfds_t)n, 1);

		for (int i = 0; i < 3; i++)
			if (pf[i].revents & POLLIN)
				accept_on(lfd[i], chans[i]);
		if (pf[3].revents & POLLIN) {
			uint8_t req[1024], resp[256];
			struct sockaddr_in from;
			socklen_t fl = sizeof(from);
			ssize_t r = recvfrom(lfd[3], req, sizeof(req), 0, (struct sockaddr *)&from, &fl);
			uint32_t m = (r > 0) ? vxi_udp(req, (uint32_t)r, resp, sizeof(resp)) : 0;
			if (m)
				sendto(lfd[3], resp, m, 0, (struct sockaddr *)&from, fl);
		}
		for (int i = 0; i < 2; i++)
			if (pf[4 + i].revents & POLLIN)
				stream_accept(&s_streams[i], s_streams[i].lfd);
		if (pf[6].revents & POLLIN)
			web_accept(web_lfd);
		for (int i = 7; i < n; i++)
			if (owner[i]->fd >= 0)
				service(owner[i], pf[i].revents);

		for (int k = 0; k < 200; k++) { /* simulated time runs on calls, not the clock */
			vxi_task();
			scpi_raw_run(&s_raw);
			prologix_run(&s_px);
			for (int i = 0; i < WEB_CONNS; i++)
				if (s_web[i].fd >= 0)
					http_run(&s_http[i]);
			gpib_task();
		}
		for (int i = 0; i < VXI_MAX_CONNS; i++)
			if (s_conns[i].fd >= 0)
				service(&s_conns[i], 0);
	}
}
