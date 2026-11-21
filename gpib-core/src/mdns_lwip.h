#ifndef MDNS_LWIP_H_
#define MDNS_LWIP_H_

/* mDNS / DNS-SD on lwIP's responder (LWIP_MDNS_RESPONDER, which needs LWIP_IGMP and multicast through the MAC
 * filter): <hostname>.local from start-up, then the LXI-style services once the instrument is known. Without
 * LWIP_MDNS_RESPONDER these do nothing. */

struct netif;

void mdns_lwip_init(struct netif *nif, const char *hostname);
void mdns_lwip_services(struct netif *nif); /* _vxi-11 (111), _scpi-raw (5025), _lxi and _http (80), after instr_id() */

#endif /* MDNS_LWIP_H_ */
