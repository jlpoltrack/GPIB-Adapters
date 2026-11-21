#ifndef VXI11_H_
#define VXI11_H_

/* VXI-11 server (portmapper, core and abort channels) as a transport-independent protocol layer. The network shim
 * (lwIP on the target, POSIX sockets on the host) opens a connection per accepted TCP socket, feeds it bytes with
 * vxi_rx() and sends what vxi_tx() returns. Calls that wait for the GPIB bus reply later, from vxi_task(). */

#include <stdint.h>
#include <stdbool.h>

#define VXI_PORTMAP_PORT 111
#define VXI_CORE_PORT    9010
#define VXI_ABORT_PORT   9011

#define VXI_MAX_CONNS 6    /* TCP connections over all three channels */
#define VXI_MAX_LINKS 4
#define VXI_MAX_DATA  1024 /* maxRecvSize, and the most one device_read returns */

typedef enum { VXI_CH_PORTMAP, VXI_CH_CORE, VXI_CH_ABORT } vxi_channel_t;
typedef struct vxi_conn vxi_conn_t;

void vxi_init(void);
vxi_conn_t *vxi_open(vxi_channel_t ch);  /* NULL when all connections are in use: refuse the socket */
uint32_t vxi_rx(vxi_conn_t *c, const uint8_t *data, uint32_t len); /* bytes taken; the rest waits for a reply */
const uint8_t *vxi_tx(vxi_conn_t *c, uint32_t *len);                /* reply bytes to send, or NULL */
void vxi_tx_done(vxi_conn_t *c, uint32_t n);                         /* n of them were sent */
bool vxi_failed(const vxi_conn_t *c);    /* garbage on the stream: close the socket (then vxi_close()) */
void vxi_close(vxi_conn_t *c);           /* socket gone: destroys its links, aborts its GPIB job */
void vxi_task(void);                     /* from the main loop */

/* Portmapper over UDP: one datagram in, the reply out. Returns the reply length, 0 for none. */
uint32_t vxi_udp(const uint8_t *req, uint32_t len, uint8_t *resp, uint32_t max);

#endif /* VXI11_H_ */
