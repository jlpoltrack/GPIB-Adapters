#ifndef VXI11_LWIP_H_
#define VXI11_LWIP_H_

/* vxi11.c on lwIP's raw API (NO_SYS): portmapper on TCP and UDP 111, core and abort channels. Call from the same
 * context as the rest of lwIP. */

void vxi11_lwip_init(void); /* after lwip_init(); calls vxi_init() */
void vxi11_lwip_task(void); /* from the main loop: vxi_task(), then every connection's input and output */

#endif /* VXI11_LWIP_H_ */
