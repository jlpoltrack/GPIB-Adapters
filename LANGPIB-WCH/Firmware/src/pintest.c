/* GPIB pin self-test, boot only and only with no bus: finds stuck and bridged lines (U1's pins, the traces, J3's
 * joints) with the internal pull-ups (30-50k). R6 (100k) still lets ATN read high on its pull-up (FT VIH ~1.6 V). */

#include <stdio.h>
#include <string.h>
#include "board.h"
#include "gpib_pins.h"
#include "pintest.h"

#define CFG_FLOAT  0x4u
#define CFG_PULL   0x8u /* with OUTDR = 1: pull-up */
#define CFG_OD_OUT 0x6u /* open-drain output, 2 MHz, as gpib_pins.h */
#define SETTLE_US  200u /* 50k x ~1 nF of cable is 50 us */

typedef struct {
	const char   *name;
	GPIO_TypeDef *port;
	uint8_t       pin;
} line_t;

#define DIO_PIN(n) (DATA_SHIFT + __builtin_ctz(DIO_TO_PORT(1u << ((n) - 1))))

static const line_t s_lines[] = {
	{ "DIO1", DATA_PORT, DIO_PIN(1) }, { "DIO2", DATA_PORT, DIO_PIN(2) }, { "DIO3", DATA_PORT, DIO_PIN(3) },
	{ "DIO4", DATA_PORT, DIO_PIN(4) }, { "DIO5", DATA_PORT, DIO_PIN(5) }, { "DIO6", DATA_PORT, DIO_PIN(6) },
	{ "DIO7", DATA_PORT, DIO_PIN(7) }, { "DIO8", DATA_PORT, DIO_PIN(8) }, { "EOI", EOI_PORT, EOI_PIN },
	{ "DAV", DAV_PORT, DAV_PIN },      { "NRFD", NRFD_PORT, NRFD_PIN },  { "NDAC", NDAC_PORT, NDAC_PIN },
	{ "IFC", IFC_PORT, IFC_PIN },      { "SRQ", SRQ_PORT, SRQ_PIN },     { "ATN", ATN_PORT, ATN_PIN },
	{ "REN", REN_PORT, REN_PIN },
};
#define NLINES (sizeof(s_lines) / sizeof(s_lines[0]))
#define ATN_IDX 14u

static void cfg(const line_t *l, uint32_t mode)
{
	volatile uint32_t *r = (l->pin < 8) ? &l->port->CFGLR : &l->port->CFGHR;
	unsigned sh = (l->pin & 7u) * 4u;
	*r = (*r & ~(0xFu << sh)) | (mode << sh);
}

static uint32_t cfg_get(const line_t *l)
{
	volatile uint32_t *r = (l->pin < 8) ? &l->port->CFGLR : &l->port->CFGHR;
	return (*r >> ((l->pin & 7u) * 4u)) & 0xFu;
}

/* each line's mode and output bit as they were; bit writes only, PA8 (LED) shares port A with REN */
static void restore(const uint8_t *mode, uint32_t out)
{
	for (unsigned i = 0; i < NLINES; i++) {
		const line_t *l = &s_lines[i];
		if (out & (1u << i))
			l->port->BSHR = 1u << l->pin;
		else
			l->port->BCR = 1u << l->pin;
		cfg(l, mode[i]);
	}
}

static bool high(const line_t *l)
{
	return (l->port->INDR >> l->pin) & 1u;
}

/* bit i set: line i reads low */
static uint32_t lows(void)
{
	uint32_t m = 0;
	board_delay_us(SETTLE_US);
	for (unsigned i = 0; i < NLINES; i++)
		if (!high(&s_lines[i]))
			m |= 1u << i;
	return m;
}

static void add(char *buf, size_t len, const char *fmt, const char *a, const char *b)
{
	size_t n = strlen(buf);
	if (n < len)
		snprintf(buf + n, len - n, fmt, a, b);
}

bool gpib_pintest(void)
{
	uint8_t mode[NLINES];
	uint32_t out = 0;
	char faults[150] = ""; /* with its newline it must fit board_log()'s 160 */
	bool ok = true;

	RCC->PB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_IOPDEN;
	for (unsigned i = 0; i < NLINES; i++) {
		mode[i] = (uint8_t)cfg_get(&s_lines[i]);
		if ((s_lines[i].port->OUTDR >> s_lines[i].pin) & 1u)
			out |= 1u << i;
	}

	/* ATN with R6 alone, floating: low. High here means a powered bus is there, so leave the lines alone. */
	cfg(&s_lines[ATN_IDX], CFG_FLOAT);
	board_delay_us(SETTLE_US);
	if (high(&s_lines[ATN_IDX])) {
		restore(mode, out);
		board_log("GPIB pins: not tested (ATN high: bus present, or R6 open)\n");
		return true;
	}

	for (unsigned i = 0; i < NLINES; i++) {
		s_lines[i].port->BSHR = 1u << s_lines[i].pin;
		cfg(&s_lines[i], CFG_PULL);
	}
	uint32_t base = lows();
	for (unsigned i = 0; i < NLINES; i++)
		if (base & (1u << i)) {
			add(faults, sizeof(faults), " %s%s", s_lines[i].name, " stuck low;");
			ok = false;
		}

	/* one line low at a time: only it may read low. Not with ATN held low against the pull-up: that can be another
	 * controller on a live bus, where a low pulse on IFC or DAV is a command to every instrument. */
	for (unsigned i = 0; (i < NLINES) && !(base & (1u << ATN_IDX)); i++) {
		if (base & (1u << i))
			continue;
		const line_t *l = &s_lines[i];
		l->port->BCR = 1u << l->pin;
		cfg(l, CFG_OD_OUT);
		uint32_t m = lows() & ~base;
		l->port->BSHR = 1u << l->pin;
		cfg(l, CFG_PULL);
		if (!(m & (1u << i))) {
			add(faults, sizeof(faults), " %s%s", l->name, " stuck high;");
			ok = false;
		}
		for (unsigned j = i + 1; j < NLINES; j++)
			if (m & (1u << j)) {
				add(faults, sizeof(faults), " %s-%s bridged;", l->name, s_lines[j].name);
				ok = false;
			}
	}

	restore(mode, out); /* gpib_pins_init() then starts from the reset state, as without the test */
	if (ok) {
		board_log("GPIB pins: ok (16 lines, no stuck or bridged line; ATN low via R6)\n");
	} else { /* two calls, one line: together they are longer than board_log() takes */
		board_log("GPIB pins: FAULT (or an instrument switched off on the plug):");
		board_log("%s\n", faults);
	}
	return ok;
}
