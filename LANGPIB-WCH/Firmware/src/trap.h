#ifndef TRAP_H_
#define TRAP_H_

#include <stdint.h>

#define TRAP_MAGIC 0x7EA9F417u

/* the last exception, kept across the watchdog reset it ends in */
typedef struct {
	uint32_t magic, cause, epc, tval;
} trap_record_t;

extern trap_record_t trap_last;

#endif /* TRAP_H_ */
