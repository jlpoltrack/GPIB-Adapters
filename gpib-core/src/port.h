#ifndef PORT_H_
#define PORT_H_

#include <stdint.h>

/* A byte stream for a front end (Prologix, raw SCPI, HTTP): the CDC-ACM port (cdc_port.c), a TCP connection
 * (sockets_lwip.c, host/), or a fake in the host tests. write() takes what fits; write_room() says how much that is.
 * close() ends a connection once its output is out (HTTP); NULL where there's nothing to close. */
typedef struct port {
	uint32_t (*read)(void *ctx, uint8_t *buf, uint32_t max);
	uint32_t (*write_room)(void *ctx);
	void (*write)(void *ctx, const void *buf, uint32_t n);
	void (*flush)(void *ctx);
	void *ctx;
	void (*close)(void *ctx);
} port_t;

extern const port_t cdc_port; /* cdc_port.c: TinyUSB's CDC interface 0 */

#endif /* PORT_H_ */
