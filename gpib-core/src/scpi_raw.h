#ifndef SCPI_RAW_H_
#define SCPI_RAW_H_

/* Raw SCPI socket (TCP 5025, as LXI instruments have): each line goes to the instrument (gpib_addr) as sent, with EOI
 * on its last byte; a line with a '?' then reads the reply, ending on EOI or the read termination ('!term'), and sends
 * it back, keeping the bus from the write to the end of the reply. Transport-independent: a port_t (sockets_lwip.c,
 * host/). */

#include <stdbool.h>
#include <stdint.h>
#include "gpib_bus.h"
#include "port.h"

#define SCPI_RAW_LINE_MAX 256

typedef struct scpi_raw {
	const port_t *port;
	uint8_t       state;
	bool          query, sent_part;
	gpib_job_t    job;
	uint8_t       line[SCPI_RAW_LINE_MAX];
	uint16_t      len;
	uint8_t       rbuf[256];
} scpi_raw_t;

void scpi_raw_init(scpi_raw_t *r, const port_t *port);
void scpi_raw_run(scpi_raw_t *r);   /* from the main loop */
void scpi_raw_reset(scpi_raw_t *r); /* a new client: drop its unfinished line and GPIB job */

#endif /* SCPI_RAW_H_ */
