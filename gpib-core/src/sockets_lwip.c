#include <string.h>
#include "sockets_lwip.h"
#include "scpi_raw.h"
#include "prologix.h"
#include "http.h"
#include "lwip/tcp.h"

/* One listening port and its client. Input stays in the pbuf chain until the protocol reads it, and the window opens
 * as it does (tcp_recved), so TCP holds a client off while the bus is busy. With no client, output is discarded. */
typedef struct {
	struct tcp_pcb *pcb;
	struct pbuf    *in;
	port_t          port;
	void          (*reset)(void *proto);
	void           *proto;
	uint32_t        seq; /* accept order: a full listener replaces its oldest client */
} tcp_port_t;

typedef struct {
	tcp_port_t *slot;
	uint8_t     n;
} listener_t;

static uint32_t tp_read(void *ctx, uint8_t *buf, uint32_t max)
{
	tcp_port_t *t = ctx;

	if (!t->in || !max)
		return 0;
	u16_t n = pbuf_copy_partial(t->in, buf, (u16_t)((max < 0xFFFFu) ? max : 0xFFFFu), 0);
	t->in = pbuf_free_header(t->in, n);
	if (t->pcb)
		tcp_recved(t->pcb, n);
	return n;
}

static uint32_t tp_write_room(void *ctx)
{
	tcp_port_t *t = ctx;

	if (!t->pcb)
		return 0xFFFFu;
	return (tcp_sndqueuelen(t->pcb) < TCP_SND_QUEUELEN - 1) ? tcp_sndbuf(t->pcb) : 0;
}

static void tp_free(tcp_port_t *t)
{
	if (t->in)
		pbuf_free(t->in);
	t->in = NULL;
	t->pcb = NULL;
}

/* Bytes lwIP can't take (queue or memory full) would leave a hole in the stream: reset the connection instead,
 * so the client sees an error, not corrupted data. Callers check write_room() first; this is the backstop. */
static void tp_write(void *ctx, const void *buf, uint32_t n)
{
	tcp_port_t *t = ctx;

	if (!t->pcb || !n)
		return;
	if ((n > tp_write_room(ctx)) || (tcp_write(t->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK)) {
		struct tcp_pcb *pcb = t->pcb;
		tcp_arg(pcb, NULL);
		tcp_recv(pcb, NULL);
		tcp_err(pcb, NULL);
		tcp_abort(pcb);
		tp_free(t);
	}
}

static void tp_flush(void *ctx)
{
	tcp_port_t *t = ctx;

	if (t->pcb)
		tcp_output(t->pcb);
}

/* ERR_ABRT if it had to abort: a callback for this pcb must then return that to lwIP */
static err_t tp_drop(tcp_port_t *t)
{
	struct tcp_pcb *pcb = t->pcb;
	err_t ret = ERR_OK;

	tcp_arg(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_err(pcb, NULL);
	if (tcp_close(pcb) != ERR_OK) {
		tcp_abort(pcb);
		ret = ERR_ABRT;
	}
	tp_free(t);
	return ret;
}

/* close once the queued output is out: what the client sent and nobody read is acknowledged first, or lwIP
 * would answer the close with a reset and the client could lose the response */
static void tp_close(void *ctx)
{
	tcp_port_t *t = ctx;

	if (!t->pcb)
		return;
	if (t->in)
		tcp_recved(t->pcb, t->in->tot_len);
	tp_drop(t);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	tcp_port_t *t = arg;

	if (!t || (t->pcb != pcb)) {
		if (p)
			pbuf_free(p);
		return ERR_OK;
	}
	if (!p || (err != ERR_OK)) { /* closed by the client */
		if (p)
			pbuf_free(p);
		return tp_drop(t);
	}
	if (t->in)
		pbuf_cat(t->in, p);
	else
		t->in = p;
	return ERR_OK;
}

/* lwIP has freed the pcb already */
static void on_err(void *arg, err_t err)
{
	(void)err;
	if (arg)
		tp_free(arg);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	static uint32_t accepts;
	listener_t *l = arg;
	tcp_port_t *t = NULL;

	if ((err != ERR_OK) || !pcb)
		return ERR_VAL;
	for (uint8_t i = 0; (i < l->n) && !t; i++)
		if (!l->slot[i].pcb)
			t = &l->slot[i];
	if (!t) {
		t = &l->slot[0];
		for (uint8_t i = 1; i < l->n; i++)
			if ((int32_t)(l->slot[i].seq - t->seq) < 0)
				t = &l->slot[i];
		tp_drop(t); /* another pcb: its ERR_ABRT isn't ours to return */
	}
	t->seq = ++accepts;
	t->pcb = pcb;
	t->reset(t->proto);
	tcp_arg(pcb, t);
	tcp_recv(pcb, on_recv);
	tcp_err(pcb, on_err);
	tcp_nagle_disable(pcb); /* replies are small and a client waits for each */
	ip_set_option(pcb, SOF_KEEPALIVE);
	return ERR_OK;
}

static void listen_on(listener_t *l, uint16_t port)
{
	for (uint8_t i = 0; i < l->n; i++)
		l->slot[i].port = (port_t){ tp_read, tp_write_room, tp_write, tp_flush, &l->slot[i], tp_close };
	struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
	if (!pcb || (tcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK))
		return;
	pcb = tcp_listen_with_backlog(pcb, l->n);
	tcp_arg(pcb, l);
	tcp_accept(pcb, on_accept);
}

static tcp_port_t s_raw_port, s_px_port, s_http_port[SOCKETS_HTTP_CONNS];
static listener_t s_raw_l = { &s_raw_port, 1 }, s_px_l = { &s_px_port, 1 }, s_http_l = { s_http_port, SOCKETS_HTTP_CONNS };
static scpi_raw_t s_raw;
static prologix_t s_px;
static http_t     s_http[SOCKETS_HTTP_CONNS];

static void raw_reset(void *proto) { scpi_raw_reset(proto); }
static void px_reset(void *proto) { prologix_reset(proto); }
static void http_reset_(void *proto) { http_reset(proto); }

void sockets_lwip_init(void)
{
	s_raw_port = (tcp_port_t){ .reset = raw_reset, .proto = &s_raw };
	s_px_port = (tcp_port_t){ .reset = px_reset, .proto = &s_px };
	for (int i = 0; i < SOCKETS_HTTP_CONNS; i++)
		s_http_port[i] = (tcp_port_t){ .reset = http_reset_, .proto = &s_http[i] };
	listen_on(&s_raw_l, SOCKETS_RAW_PORT);
	listen_on(&s_px_l, SOCKETS_PROLOGIX_PORT);
	listen_on(&s_http_l, HTTP_PORT);
	scpi_raw_init(&s_raw, &s_raw_port.port);
	prologix_init(&s_px, &s_px_port.port, GPIB_OWNER_PX_NET);
	for (int i = 0; i < SOCKETS_HTTP_CONNS; i++)
		http_init(&s_http[i], &s_http_port[i].port);
}

void sockets_lwip_task(void)
{
	scpi_raw_run(&s_raw);
	prologix_run(&s_px);
	for (int i = 0; i < SOCKETS_HTTP_CONNS; i++)
		if (s_http_port[i].pcb)
			http_run(&s_http[i]);
}
