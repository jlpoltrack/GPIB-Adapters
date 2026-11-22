#ifndef NET_H_
#define NET_H_

/* Ethernet: the on-chip MAC and 10/100 PHY (eth.c), lwIP with DHCP then AutoIP, VXI-11 */

#include <stdbool.h>
#include <stdint.h>

void     net_init(void);           /* PHY, MAC, lwIP, DHCP */
void     net_start_services(void); /* VXI-11, raw SCPI, Prologix: once the instrument is found */
void     net_task(void);           /* net_poll(), the web page, and the instrument ports once started */
uint32_t net_phy_id(void);
uint32_t net_ip(void);             /* 0 until it has an address */

#endif /* NET_H_ */
