#ifndef INSTR_H_
#define INSTR_H_

#include <stdint.h>
#include <stdbool.h>

/* The instrument behind the adapter, shared by the front ends (USBTMC, Prologix, VXI-11): its address and ID
 * string from auto-ID, the default read termination, and the '!' configuration commands. */

#define INSTR_ID_MAX 50 /* characters, as the USB serial string */

extern uint8_t gpib_addr; /* found by auto-ID (fallback 1), or set by '!addr' */

void instr_init(void);
/* Blocking start-up: waits for the bus, finds the instrument and reads its ID. idle() runs every 10 ms or so
 * (watchdog, later the network). Returns false if the bus went away while searching: the caller resets. */
bool instr_startup(void (*idle)(void));
const char *instr_id(void);
bool        instr_ready(void); /* instr_startup() has found the instrument */

char instr_get_readterm(void); /* '\n', '\r', or 0 for EOI only */
void instr_set_readterm(char term);

/* A '!' command, without the '!'. Returns the length of the response put in resp (0 for none). */
uint8_t instr_command(const uint8_t *cmd, uint16_t len, uint8_t *resp, uint8_t max);
void set_internal_response(uint8_t *pdat, uint8_t len); /* for miniparser.c's handlers */

#endif /* INSTR_H_ */
