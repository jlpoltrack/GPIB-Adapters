/* A simulated SCPI instrument: answers *IDN?, *STB?, MEAS?, BLOCK? n, X..X? and echoes other queries */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpib_sim.h"
#include "sim_scpi.h"

static unsigned s_count;

void sim_scpi(sim_dev_t *d)
{
	static char msg[sizeof(d->in) + 1], ans[300]; /* static: 4.4 KB would overflow a board's stack */
	uint16_t n = d->in_len < sizeof(msg) - 1 ? d->in_len : sizeof(msg) - 1;

	memcpy(msg, d->in, n);
	d->in_len = 0;
	while (n && ((msg[n - 1] == '\n') || (msg[n - 1] == '\r')))
		n--;
	msg[n] = '\0';
	if (!strcmp(msg, "*IDN?")) {
		snprintf(ans, sizeof(ans), "LANGPIB,SIMULATED,0,0.1\n");
	} else if (!strcmp(msg, "*STB?")) {
		snprintf(ans, sizeof(ans), "%u\n", d->status);
	} else if (!strcmp(msg, "MEAS?")) {
		snprintf(ans, sizeof(ans), "+1.%03u000E+00\n", s_count++ % 100u); /* 1.000-1.099, no float printf */
	} else if (!strncmp(msg, "BLOCK? ", 7)) { /* BLOCK? n: n bytes of 'A'..'Z', for long reads */
		unsigned len = (unsigned)atoi(&msg[7]) % 255u;
		for (unsigned i = 0; i < len; i++)
			ans[i] = (char)('A' + i % 26);
		ans[len] = '\n';
		ans[len + 1] = '\0';
	} else if ((msg[0] == 'X') && n && (msg[n - 1] == '?')) { /* X...X?: its length, for long writes */
		snprintf(ans, sizeof(ans), "%u\n", n);
	} else if (n && (msg[n - 1] == '?')) {
		snprintf(ans, sizeof(ans), "%.200s\n", msg);
	} else {
		return;
	}
	sim_answer(d, ans, true);
}
