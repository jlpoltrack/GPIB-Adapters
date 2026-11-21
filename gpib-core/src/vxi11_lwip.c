#include <string.h>
#include "vxi11.h"
#include "vxi11_lwip.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

/* Input stays in the pbuf chain until vxi_rx() takes it, and the window opens only as it does (tcp_recved), so a
 * client waiting on a GPIB reply is held off by TCP rather than by buffering here. */
typedef struct {
	struct tcp_pcb *pcb;
	vxi_conn_t     *vc;
	struct pbuf    *in;
} conn_t;

static conn_t          s_conns[VXI_MAX_CONNS];
static struct udp_pcb *s_udp;

static void conn_free(conn_t *k)
{
	if (k->in)
		pbuf_free(k->in);
	vxi_close(k->vc);
	*k = (conn_t){ 0 };
}

/* ERR_ABRT if it had to abort: a callback must then return that to lwIP */
static err_t conn_drop(conn_t *k)
{
	struct tcp_pcb *pcb = k->pcb;
	err_t ret = ERR_OK;

	tcp_arg(pcb, NULL);
	tcp_recv(pcb, NULL);
	tcp_sent(pcb, NULL);
	tcp_err(pcb, NULL);
	tcp_poll(pcb, NULL, 0);
	if (tcp_close(pcb) != ERR_OK) {
		tcp_abort(pcb);
		ret = ERR_ABRT;
	}
	conn_free(k);
	return ret;
}

static err_t service(conn_t *k)
{
	while (k->in) {
		uint16_t len = k->in->len;
		if (!len) { /* an empty pbuf in the chain */
			struct pbuf *next = k->in->next;
			if (next)
				pbuf_ref(next);
			pbuf_free(k->in);
			k->in = next;
			continue;
		}
		uint32_t used = vxi_rx(k->vc, k->in->payload, len);
		if (used) {
			k->in = pbuf_free_header(k->in, (u16_t)used);
			tcp_recved(k->pcb, (u16_t)used);
		}
		if (used < len)
			break;
	}

	uint32_t len;
	const uint8_t *p;
	bool wrote = false;
	while ((p = vxi_tx(k->vc, &len)) != NULL) {
		u16_t room = tcp_sndbuf(k->pcb);
		uint32_t n = (len < room) ? len : room;
		if (!n || (tcp_write(k->pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY) != ERR_OK))
			break;
		vxi_tx_done(k->vc, n);
		wrote = true;
	}
	if (wrote)
		tcp_output(k->pcb);

	if (vxi_failed(k->vc))
		return conn_drop(k);
	return ERR_OK;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
	conn_t *k = arg;
	(void)pcb;

	if (!k) {
		if (p)
			pbuf_free(p);
		return ERR_OK;
	}
	if (!p || (err != ERR_OK)) { /* closed by the client */
		if (p)
			pbuf_free(p);
		return conn_drop(k);
	}
	if (k->in)
		pbuf_cat(k->in, p);
	else
		k->in = p;
	return service(k);
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
	(void)pcb;
	(void)len;
	return arg ? service(arg) : ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
	(void)pcb;
	return arg ? service(arg) : ERR_OK;
}

/* lwIP has freed the pcb already */
static void on_err(void *arg, err_t err)
{
	(void)err;
	if (arg)
		conn_free(arg);
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
	if ((err != ERR_OK) || !pcb)
		return ERR_VAL;
	conn_t *k = NULL;
	for (int i = 0; !k && (i < VXI_MAX_CONNS); i++)
		if (!s_conns[i].pcb)
			k = &s_conns[i];
	vxi_conn_t *vc = k ? vxi_open((vxi_channel_t)(uintptr_t)arg) : NULL;
	if (!vc) {
		tcp_abort(pcb);
		return ERR_ABRT;
	}
	*k = (conn_t){ .pcb = pcb, .vc = vc };
	tcp_arg(pcb, k);
	tcp_recv(pcb, on_recv);
	tcp_sent(pcb, on_sent);
	tcp_err(pcb, on_err);
	tcp_poll(pcb, on_poll, 2);
	tcp_nagle_disable(pcb); /* replies are small and a client waits for each */
	return ERR_OK;
}

static void listen_on(uint16_t port, vxi_channel_t ch)
{
	struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
	if (!pcb || (tcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK))
		return;
	pcb = tcp_listen_with_backlog(pcb, 2);
	tcp_arg(pcb, (void *)(uintptr_t)ch);
	tcp_accept(pcb, on_accept);
}

/* Portmapper GETPORT over UDP: VISA finders and lxi discover broadcast it */
static void on_udp(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
	static uint8_t req[1024], resp[256];
	(void)arg;

	u16_t n = pbuf_copy_partial(p, req, sizeof(req), 0);
	pbuf_free(p);
	uint32_t m = vxi_udp(req, n, resp, sizeof(resp));
	if (!m)
		return;
	struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)m, PBUF_RAM);
	if (!q)
		return;
	memcpy(q->payload, resp, m);
	udp_sendto(pcb, q, addr, port);
	pbuf_free(q);
}

void vxi11_lwip_init(void)
{
	vxi_init();
	listen_on(VXI_PORTMAP_PORT, VXI_CH_PORTMAP);
	listen_on(VXI_CORE_PORT, VXI_CH_CORE);
	listen_on(VXI_ABORT_PORT, VXI_CH_ABORT);
	s_udp = udp_new_ip_type(IPADDR_TYPE_V4);
	if (s_udp && (udp_bind(s_udp, IP4_ADDR_ANY, VXI_PORTMAP_PORT) == ERR_OK))
		udp_recv(s_udp, on_udp, NULL);
}

void vxi11_lwip_task(void)
{
	vxi_task();
	for (int i = 0; i < VXI_MAX_CONNS; i++)
		if (s_conns[i].pcb)
			service(&s_conns[i]);
}
