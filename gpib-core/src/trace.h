#ifndef TRACE_H_
#define TRACE_H_

#include <stdbool.h>

/* Bus trace on the board's UART (board_trace_putc()): one line per finished GPIB job */

void trace_init(void);
void trace_enable(bool on);
bool trace_enabled(void);
void trace_task(void); /* from the main loop: feeds the UART */

#endif /* TRACE_H_ */
