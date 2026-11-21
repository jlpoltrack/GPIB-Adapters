#ifndef GPIB_PINS_H_
#define GPIB_PINS_H_

/* GPIB line access for gpib_phy.c. Every line is an FT pin in open-drain output mode: OUTDR=0 drives low, 1 releases
 * (SRQ is a floating input). Controls are spread over four ports; DIO1-8 sit on PB8-PB15 out of bit order (routing),
 * so data goes through two 256-entry tables (docs/plan.md, GPIB pin map). */

#include "ch32v4x7.h"

#define REN_PORT  GPIOA
#define REN_PIN   15
#define EOI_PORT  GPIOC
#define EOI_PIN   10
#define DAV_PORT  GPIOC
#define DAV_PIN   11
#define NRFD_PORT GPIOC
#define NRFD_PIN  12
#define NDAC_PORT GPIOD
#define NDAC_PIN  2
#define IFC_PORT  GPIOB
#define IFC_PIN   3
#define SRQ_PORT  GPIOB
#define SRQ_PIN   4  /* input only */
#define ATN_PORT  GPIOB
#define ATN_PIN   5  /* R6 100k pull-down: reads low with no bus (gpib_connected()) */

#define DATA_PORT  GPIOB
#define DATA_SHIFT 8 /* DIO on PB8-PB15 */
#define DATA_MASK  (0xFFu << DATA_SHIFT)

/* DIOn -> PB bit - 8: DIO1 PB13, DIO2 PB15, DIO3 PB10, DIO4 PB9, DIO5 PB12, DIO6 PB14, DIO7 PB11, DIO8 PB8 */
#define DIO_TO_PORT(d) ((((d) >> 0 & 1u) << 5) | (((d) >> 1 & 1u) << 7) | (((d) >> 2 & 1u) << 2) | \
                        (((d) >> 3 & 1u) << 1) | (((d) >> 4 & 1u) << 4) | (((d) >> 5 & 1u) << 6) | \
                        (((d) >> 6 & 1u) << 3) | (((d) >> 7 & 1u) << 0))
#define PORT_TO_DIO(p) ((((p) >> 5 & 1u) << 0) | (((p) >> 7 & 1u) << 1) | (((p) >> 2 & 1u) << 2) | \
                        (((p) >> 1 & 1u) << 3) | (((p) >> 4 & 1u) << 4) | (((p) >> 6 & 1u) << 5) | \
                        (((p) >> 3 & 1u) << 6) | (((p) >> 0 & 1u) << 7))

#ifdef GPIB_SIM
#include "gpib_sim_pins.h" /* SIM=1: gpib-core's simulated bus; the GPIB pins stay as reset left them */
#else

#define LINE_LOW(l)   (l##_PORT->BCR = 1u << l##_PIN)
#define LINE_HIGH(l)  (l##_PORT->BSHR = 1u << l##_PIN)
#define LINE_STATE(l) (l##_PORT->INDR & (1u << l##_PIN))
#define LINE_OUT(l)   (l##_PORT->OUTDR & (1u << l##_PIN))

#define ATN_LOW   LINE_LOW(ATN)
#define ATN_HIGH  LINE_HIGH(ATN)
#define NDAC_LOW  LINE_LOW(NDAC)
#define NDAC_HIGH LINE_HIGH(NDAC)
#define NRFD_LOW  LINE_LOW(NRFD)
#define NRFD_HIGH LINE_HIGH(NRFD)
#define DAV_LOW   LINE_LOW(DAV)
#define DAV_HIGH  LINE_HIGH(DAV)
#define EOI_LOW   LINE_LOW(EOI)
#define EOI_HIGH  LINE_HIGH(EOI)
#define REN_LOW   LINE_LOW(REN)
#define REN_HIGH  LINE_HIGH(REN)
#define IFC_LOW   LINE_LOW(IFC)
#define IFC_HIGH  LINE_HIGH(IFC)

#define DAV_STATE  LINE_STATE(DAV)
#define NDAC_STATE LINE_STATE(NDAC)
#define NRFD_STATE LINE_STATE(NRFD)
#define ATN_STATE  LINE_STATE(ATN)
#define EOI_STATE  LINE_STATE(EOI)
#define SRQ_STATE  LINE_STATE(SRQ)

/* true when ATN is released */
#define ATN_OUT_STATE (LINE_OUT(ATN) != 0)

#define GPIB_T4(M, i)   M(i), M((i) + 1), M((i) + 2), M((i) + 3)
#define GPIB_T16(M, i)  GPIB_T4(M, i), GPIB_T4(M, (i) + 4), GPIB_T4(M, (i) + 8), GPIB_T4(M, (i) + 12)
#define GPIB_T64(M, i)  GPIB_T16(M, i), GPIB_T16(M, (i) + 16), GPIB_T16(M, (i) + 32), GPIB_T16(M, (i) + 48)
#define GPIB_T256(M)    GPIB_T64(M, 0), GPIB_T64(M, 64), GPIB_T64(M, 128), GPIB_T64(M, 192)

static const uint8_t s_dio_to_port[256] = { GPIB_T256(DIO_TO_PORT) };
static const uint8_t s_port_to_dio[256] = { GPIB_T256(PORT_TO_DIO) };

/* GPIB data is negative logic: a 1 bit pulls the line low. One BSHR write: reset the 1s, set the 0s. */
#define DATA_WRITE(d) (DATA_PORT->BSHR = ((uint32_t)s_dio_to_port[(uint8_t)(d)] << (DATA_SHIFT + 16)) | \
                                         ((uint32_t)(uint8_t)~s_dio_to_port[(uint8_t)(d)] << DATA_SHIFT))
#define DATA_RELEASE  (DATA_PORT->BSHR = DATA_MASK)
#define DATA_READ     (s_port_to_dio[(uint8_t)~(DATA_PORT->INDR >> DATA_SHIFT)])

/* 4-bit CFGxR field: 0x6 open-drain output 2 MHz (the slowest edges: HS pins, a cable), 0x4 floating input */
#define GPIB_OD 0x6u

static inline void gpib_pin_cfg(GPIO_TypeDef *port, unsigned pin, uint32_t mode)
{
	volatile uint32_t *cfg = (pin < 8) ? &port->CFGLR : &port->CFGHR;
	unsigned sh = (pin & 7u) * 4u;
	*cfg = (*cfg & ~(0xFu << sh)) | (mode << sh);
}

/* Everything starts released; REN keeps its output level (low from reset), phy_init() then sets it. No pulls: the
 * bus has 5 V terminations, and ATN needs R6 alone to read low with no bus. */
static inline void gpib_pins_init(void)
{
	RCC->PB2PCENR |= RCC_IOPAEN | RCC_IOPBEN | RCC_IOPCEN | RCC_IOPDEN;

	DATA_RELEASE;
	EOI_HIGH;
	DAV_HIGH;
	NRFD_HIGH;
	NDAC_HIGH;
	IFC_HIGH;
	ATN_HIGH;
	for (unsigned pin = 8; pin < 16; pin++)
		gpib_pin_cfg(DATA_PORT, pin, GPIB_OD);
	gpib_pin_cfg(REN_PORT, REN_PIN, GPIB_OD);
	gpib_pin_cfg(EOI_PORT, EOI_PIN, GPIB_OD);
	gpib_pin_cfg(DAV_PORT, DAV_PIN, GPIB_OD);
	gpib_pin_cfg(NRFD_PORT, NRFD_PIN, GPIB_OD);
	gpib_pin_cfg(NDAC_PORT, NDAC_PIN, GPIB_OD);
	gpib_pin_cfg(IFC_PORT, IFC_PIN, GPIB_OD);
	gpib_pin_cfg(ATN_PORT, ATN_PIN, GPIB_OD);
	gpib_pin_cfg(SRQ_PORT, SRQ_PIN, 0x4u);
}

#endif /* GPIB_SIM */
#endif /* GPIB_PINS_H_ */
