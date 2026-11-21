#ifndef PROLOGIX_H_
#define PROLOGIX_H_

/* Prologix GPIB-USB / GPIB-ETHERNET (controller mode) protocol on a byte stream, plus this adapter's ++ extensions.
 * One instance per port: the CDC port (prologix_task()) and TCP 1234 (sockets_lwip.c). The ++ settings are shared. */

#include <stdbool.h>
#include <stdint.h>
#include "gpib_bus.h"
#include "port.h"

#define PROLOGIX_LINE_MAX 256

typedef struct prologix {
	const port_t *port;
	uint8_t       owner; /* GPIB_OWNER_* for its jobs */
	uint8_t       state, kind;
	gpib_job_t    job;
	uint8_t       line[PROLOGIX_LINE_MAX + 2]; /* + the EOS terminator */
	uint16_t      len;
	bool          esc, cmd, any, sent_part, final;
	uint8_t       plus;
	bool          savecfg;
	bool          ton; /* data lines go out talk-only, without addressing */
	uint8_t       rbuf[64];
	uint8_t       rflags, rterm;
	uint8_t       trg[15];
	uint8_t       ntrg, itrg;
	uint8_t       status;
} prologix_t;

void prologix_init(prologix_t *px, const port_t *port, uint8_t owner);
void prologix_run(prologix_t *px);   /* from the main loop */
void prologix_reset(prologix_t *px); /* a new client on the port: drop its unfinished line and GPIB job */
void prologix_task(void);            /* the CDC port's instance (cdc_port.c) */

#endif /* PROLOGIX_H_ */
