#include <stdio.h>
#include <string.h>
#include "net.h"
#include "eth.h"
#include "board.h"
#include "vxi11_lwip.h"
#include "sockets_lwip.h"
#include "mdns_lwip.h"
#include "netcfg.h"
#include "settings.h"
#include "lwip/igmp.h"
#include "lwip/autoip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#define LINK_POLL_MS 500
#define TX_WAIT_US   2000 /* a full TX ring at 10 Mbit/s drains in about 5 ms; TCP retries what this drops */
#define RX_PER_POLL  8

static struct netif s_netif;
static uint8_t      s_mac[6];
static char         s_hostname[NETCFG_NAME_MAX + 1];
static uint32_t     s_phy_id, s_link_at;
static bool         s_services, s_dhcp;

u32_t sys_now(void)
{
	return board_millis();
}

static err_t linkoutput(struct netif *nif, struct pbuf *p)
{
	(void)nif;
	if (p->tot_len > ETH_FRAME_MAX)
		return ERR_BUF;
	uint8_t *buf = eth_tx_buf(TX_WAIT_US);
	if (!buf)
		return ERR_MEM;
	pbuf_copy_partial(p, buf, p->tot_len, 0);
	eth_tx_send(p->tot_len);
	return ERR_OK;
}

static err_t netif_setup(struct netif *nif)
{
	nif->name[0] = 'e';
	nif->name[1] = '0';
	nif->output = etharp_output;
	nif->linkoutput = linkoutput;
	nif->mtu = 1500;
	nif->hwaddr_len = 6;
	memcpy(nif->hwaddr, s_mac, 6);
	nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
	netif_set_hostname(nif, s_hostname);
	return ERR_OK;
}

void net_init(void)
{
	const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8u; /* ESIG_UNIID1-3 (RM 31) */

	/* locally administered; FNV-1a of the 64 real UID bits (word 3 reads erased), so every byte (and the hostname's
	 * two) depends on all of them: boards of one lot differ in only a few UID bytes */
	uint64_t h = 0xCBF29CE484222325ull;
	for (int w = 0; w < 2; w++)
		for (int i = 0; i < 4; i++)
			h = (h ^ (uint8_t)(uid[w] >> (8 * i))) * 0x100000001B3ull;
	s_mac[0] = 0x02;
	for (int i = 1; i < 6; i++, h >>= 8)
		s_mac[i] = (uint8_t)h;
	if (netcfg_name_valid(settings.net_name))
		strcpy(s_hostname, settings.net_name);
	else
		snprintf(s_hostname, sizeof(s_hostname), "langpib-%02x%02x", s_mac[4], s_mac[5]);

	eth_init(s_mac); /* logs what timed out */
	s_phy_id = eth_phy_id();

	/* settings.net_*: '!net' / '++net' (gpib-core netcfg.c), read once here */
	bool fixed = settings.net_mode == NET_MODE_STATIC;
	s_dhcp = !fixed;
	ip4_addr_t ip, mask, gw;
	IP4_ADDR(&ip, settings.net_ip[0], settings.net_ip[1], settings.net_ip[2], settings.net_ip[3]);
	IP4_ADDR(&mask, settings.net_mask[0], settings.net_mask[1], settings.net_mask[2], settings.net_mask[3]);
	IP4_ADDR(&gw, settings.net_gw[0], settings.net_gw[1], settings.net_gw[2], settings.net_gw[3]);

	lwip_init();
	netif_add(&s_netif, fixed ? &ip : IP4_ADDR_ANY4, fixed ? &mask : IP4_ADDR_ANY4, fixed ? &gw : IP4_ADDR_ANY4, NULL,
	          netif_setup, ethernet_input);
	netif_set_default(&s_netif);
	netif_set_up(&s_netif);
	if (!fixed)
		dhcp_start(&s_netif); /* AutoIP takes over if no server answers */
	mdns_lwip_init(&s_netif, s_hostname);
	board_log("net: %s, %s.local\n", fixed ? "static" : "dhcp", s_hostname);
}

void net_start_services(void)
{
	if (!s_services) {
		vxi11_lwip_init();
		sockets_lwip_init(); /* raw SCPI 5025, Prologix 1234 */
		mdns_lwip_services(&s_netif);
	}
	s_services = true;
}

void net_poll(void)
{
	const uint8_t *f;
	unsigned n;

	for (int i = 0; (i < RX_PER_POLL) && (n = eth_rx(&f)); i++) {
		struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
		if (!p) /* lwIP is full: the frame waits in the ring */
			break;
		pbuf_take(p, f, (u16_t)n);
		eth_rx_release();
		if (s_netif.input(p, &s_netif) != ERR_OK)
			pbuf_free(p);
	}

	uint32_t now = board_millis();
	if ((now - s_link_at) >= LINK_POLL_MS) {
		s_link_at = now;
		eth_link_t l;
		if (eth_link_poll(&l)) {
			if (l.up) {
				board_log("ETH link up: %s %s duplex, %s, polarity %s\n", l.fast ? "100M" : "10M",
				          l.full ? "full" : "half", l.mdix ? "MDI-X" : "MDI", l.pol_rev ? "reversed" : "normal");
				netif_set_link_up(&s_netif);
			} else {
				board_log("ETH link down\n");
				netif_set_link_down(&s_netif);
			}
		}
		static uint32_t logged_ip; /* DHCP, AutoIP or static: the address on the UART when it changes */
		uint32_t ip = ip4_addr_get_u32(netif_ip4_addr(&s_netif));
		if (ip != logged_ip) {
			logged_ip = ip;
			board_log("net: address %s (%s)\n", ip4addr_ntoa(netif_ip4_addr(&s_netif)),
			          autoip_supplied_address(&s_netif) ? "AutoIP" : s_dhcp ? "DHCP" : "static");
		}
	}
	sys_check_timeouts();
}

void net_task(void)
{
	net_poll();
	if (s_services) {
		vxi11_lwip_task();
		sockets_lwip_task();
	}
}

uint32_t net_phy_id(void)
{
	return s_phy_id;
}

uint32_t net_ip(void)
{
	return netif_is_up(&s_netif) ? ip4_addr_get_u32(netif_ip4_addr(&s_netif)) : 0;
}

static void ip_bytes(const ip4_addr_t *a, uint8_t b[4])
{
	b[0] = ip4_addr1(a);
	b[1] = ip4_addr2(a);
	b[2] = ip4_addr3(a);
	b[3] = ip4_addr4(a);
}

/* for '!net' / '++net' queries and the web page (gpib-core netcfg.h) */
bool board_net_status(netcfg_status_t *st)
{
	ip_bytes(netif_ip4_addr(&s_netif), st->ip);
	ip_bytes(netif_ip4_netmask(&s_netif), st->mask);
	ip_bytes(netif_ip4_gw(&s_netif), st->gw);
	memcpy(st->mac, s_mac, 6);
	st->hostname = s_hostname;
	st->dhcp = s_dhcp;
	return true;
}
