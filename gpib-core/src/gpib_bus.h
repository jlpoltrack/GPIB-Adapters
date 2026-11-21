#ifndef GPIB_BUS_H_
#define GPIB_BUS_H_

#include <stdint.h>
#include <stdbool.h>

/* GPIB controller operations as non-blocking jobs, shared by the front ends. gpib_task() runs them from the main
 * loop. An owner keeps the bus while it leaves an instrument addressed (a message split over several jobs); a
 * priority job (serial poll) may still run between the bytes of a write, or before a read's first byte.
 * Addresses: bits 0-4 primary, bits 5-7 secondary + 1 (0 = none). */

#define GPIB_NO_ADDR 0xFF

typedef enum { GPIB_OK, GPIB_PENDING, GPIB_TIMEOUT, GPIB_ABORTED, GPIB_NO_LISTENER } gpib_status_t; /* a zeroed job is not pending */
typedef enum { GPIB_PHASE_NONE, GPIB_PHASE_ADDRESS, GPIB_PHASE_COMMAND, GPIB_PHASE_WRITE, GPIB_PHASE_READ } gpib_phase_t;

typedef enum {
	GPIB_OP_WRITE,     /* len bytes from buf */
	GPIB_OP_READ,      /* up to len bytes into buf, ending on EOI (or term, with GPIB_TERM) */
	GPIB_OP_SPOLL,     /* serial poll: status byte into buf[0] (0 on failure) */
	GPIB_OP_SDC,       /* selective device clear */
	GPIB_OP_GET,       /* group execute trigger */
	GPIB_OP_GTL,       /* go to local */
	GPIB_OP_LLO,       /* local lockout, all instruments */
	GPIB_OP_UNADDRESS, /* UNL + UNT, ends a message cut short */
	GPIB_OP_LISTEN_ONLY, /* READ without addressing: capture an instrument in talk-only mode (plotter output) */
	GPIB_OP_TALK_ONLY,   /* WRITE without addressing: to listen-only devices (printers, plotters) */
} gpib_op_t;

/* job flags */
#define GPIB_EOI  0x01 /* WRITE: EOI with the last byte */
#define GPIB_END  0x02 /* WRITE: unaddress afterwards (READ unaddresses when the message ends) */
#define GPIB_TERM 0x04 /* READ: term also ends the message */
#define GPIB_IGNORE_EOI 0x08 /* READ: EOI doesn't end the message, but the job returns at it (eoi set) */
#define GPIB_SOFT_TMO   0x10 /* READ: timeout_ms is per byte and ends the message normally (timed_out set) */

enum { GPIB_OWNER_TMC, GPIB_OWNER_CDC, GPIB_OWNER_VXI, GPIB_OWNER_RAW, GPIB_OWNER_PX_NET, GPIB_OWNERS }; /* RAW: TCP 5025, PX_NET: Prologix on TCP 1234 */

typedef struct gpib_job {
	/* request */
	uint8_t  op, addr, flags, term;
	uint8_t *buf;
	uint16_t len;
	uint16_t timeout_ms;  /* from when the job starts; 0 = until aborted */
	/* result */
	volatile gpib_status_t status;
	uint16_t done;        /* bytes transferred */
	bool     end;         /* READ: the message ended */
	bool     eoi;         /* READ: the last byte came with EOI */
	bool     timed_out;   /* READ with GPIB_SOFT_TMO: ended by the timeout */
	/* private */
	uint8_t  owner, pc, nprog;
	bool     abort;
	uint16_t prog[12];
	uint32_t deadline;
} gpib_job_t;

typedef struct {
	gpib_status_t status;
	gpib_phase_t  phase;
	uint8_t       op, addr;
} gpib_error_t;

void gpib_init(void);
bool gpib_submit(gpib_job_t *job, uint8_t owner); /* false while that owner has a job queued */
bool gpib_submit_priority(gpib_job_t *job);       /* false while another priority job is queued */
void gpib_task(void);
void gpib_abort(gpib_job_t *job);                 /* ends the job now as GPIB_ABORTED; once finished, ends its message */
gpib_status_t gpib_run(gpib_job_t *job, uint8_t owner); /* blocking, for start-up */
uint8_t gpib_last_owner(void); /* the front end that last submitted a job (priority jobs don't count) */
void gpib_ifc(void);                              /* aborts every job, then interface clear */
void gpib_ren(bool asserted);
bool gpib_connected(void);
bool gpib_srq(void);
bool gpib_holds_bus(uint8_t owner);              /* that owner left an instrument addressed */
uint8_t gpib_find_device(void);                   /* blocking scan; GPIB_NO_ADDR if nothing answers */
int gpib_scan(uint8_t *found, uint8_t max);       /* blocking (~0.1 s): primary addresses 1..30 that listen; -1 if busy */
const gpib_error_t *gpib_last_error(void);
void gpib_set_trace(void (*fn)(const gpib_job_t *job)); /* called with every finished job */

#endif /* GPIB_BUS_H_ */
