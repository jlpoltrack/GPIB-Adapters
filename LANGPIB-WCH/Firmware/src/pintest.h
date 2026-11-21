#ifndef PINTEST_H_
#define PINTEST_H_

#include <stdbool.h>

/* Before gpib_init(): stuck and bridged GPIB lines, on the UART log. Skipped (true) when a powered bus holds ATN high. */
bool gpib_pintest(void);

#endif /* PINTEST_H_ */
