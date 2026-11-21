#ifndef LWIPOPTS_H_
#define LWIPOPTS_H_

/* lwIP 2.2, raw API from the main loop only (no interrupts touch it) */

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (16 * 1024)
#define PBUF_POOL_SIZE              16
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_TCP_PCB            16  /* VXI-11 6, 5025 1, 1234 1, HTTP 3, plus closing ones (TIME_WAIT reused) */
#define MEMP_NUM_TCP_PCB_LISTEN     6   /* portmapper, VXI-11 core and abort, 5025, 1234, 80 */
#define MEMP_NUM_UDP_PCB            5   /* DHCP, portmapper, mDNS, spare */
#define MEMP_NUM_TCP_SEG            32

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_DNS                    0
#define LWIP_IGMP                   1   /* mDNS joins 224.0.0.251 */

#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 1
#define LWIP_DHCP_AUTOIP_COOP       1
#define LWIP_DHCP_AUTOIP_COOP_TRIES 3   /* about 10 s of DHCP before a link-local address */
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_EXT_STATUS_CALLBACK 1 /* mDNS re-announces on address changes */
#define LWIP_NUM_NETIF_CLIENT_DATA  1

/* mDNS: <hostname>.local, _vxi-11, _scpi-raw, _lxi and _http (gpib-core mdns_lwip.c); responder only */
#define LWIP_MDNS_RESPONDER         1
#define MDNS_MAX_SERVICES           4   /* _vxi-11, _scpi-raw, _lxi, _http */
#define LWIP_MDNS_SEARCH            0
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 6)

/* The window keeps a peer's burst inside the ETH RX descriptor ring (eth.c), whatever the main loop is doing */
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_STATS                  0
#define LWIP_NETIF_TX_SINGLE_PBUF   0

#endif /* LWIPOPTS_H_ */
