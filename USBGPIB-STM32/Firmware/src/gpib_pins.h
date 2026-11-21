#ifndef GPIB_PINS_H_
#define GPIB_PINS_H_

/* GPIB line access for gpib_phy.c, replacing the AVR pin macros.
 * All GPIB lines are 5V-tolerant pins in open-drain mode: ODR=0 drives low, ODR=1 releases. */

#include "stm32f1xx.h"

/* Control lines (open-drain outputs, readable via IDR). Map chosen for a crossing-free layout (docs/hardware.md). */
#define EOI_PORT  GPIOB
#define EOI_PIN   12
#define DAV_PORT  GPIOB
#define DAV_PIN   13
#define NRFD_PORT GPIOB
#define NRFD_PIN  14
#define NDAC_PORT GPIOB
#define NDAC_PIN  15
#define IFC_PORT  GPIOA
#define IFC_PIN   8
#define SRQ_PORT  GPIOA
#define SRQ_PIN   9   /* input only */
#define ATN_PORT  GPIOA
#define ATN_PIN   10
#define REN_PORT  GPIOA
#define REN_PIN   15  /* JTDI: needs JTAG disabled (SWD kept) */

/* DIO1..DIO8 on GPIOB, not in bit order: PB9, PB7, PB10, PB11, PB8, PB6, PB4 (NJTRST), PB3 (JTDO) */
#define DATA_PORT GPIOB
#define DATA_MASK 0x0FD8u

#define LINE_LOW(p, n)   ((p)->BRR  = (1u << (n)))
#define LINE_HIGH(p, n)  ((p)->BSRR = (1u << (n)))
#define LINE_STATE(p, n) ((p)->IDR & (1u << (n)))
#define LINE_OUT(p, n)   ((p)->ODR & (1u << (n)))

#define ATN_LOW   LINE_LOW(ATN_PORT, ATN_PIN)
#define ATN_HIGH  LINE_HIGH(ATN_PORT, ATN_PIN)
#define NDAC_LOW  LINE_LOW(NDAC_PORT, NDAC_PIN)
#define NDAC_HIGH LINE_HIGH(NDAC_PORT, NDAC_PIN)
#define NRFD_LOW  LINE_LOW(NRFD_PORT, NRFD_PIN)
#define NRFD_HIGH LINE_HIGH(NRFD_PORT, NRFD_PIN)
#define DAV_LOW   LINE_LOW(DAV_PORT, DAV_PIN)
#define DAV_HIGH  LINE_HIGH(DAV_PORT, DAV_PIN)
#define EOI_LOW   LINE_LOW(EOI_PORT, EOI_PIN)
#define EOI_HIGH  LINE_HIGH(EOI_PORT, EOI_PIN)
#define REN_LOW   LINE_LOW(REN_PORT, REN_PIN)
#define REN_HIGH  LINE_HIGH(REN_PORT, REN_PIN)
#define IFC_LOW   LINE_LOW(IFC_PORT, IFC_PIN)
#define IFC_HIGH  LINE_HIGH(IFC_PORT, IFC_PIN)

#define DAV_STATE  LINE_STATE(DAV_PORT, DAV_PIN)
#define NDAC_STATE LINE_STATE(NDAC_PORT, NDAC_PIN)
#define NRFD_STATE LINE_STATE(NRFD_PORT, NRFD_PIN)
#define ATN_STATE  LINE_STATE(ATN_PORT, ATN_PIN)
#define EOI_STATE  LINE_STATE(EOI_PORT, EOI_PIN)
#define SRQ_STATE  LINE_STATE(SRQ_PORT, SRQ_PIN)

/* true when ATN is released; replaces the AVR's (DDRF & (1<<6)) == 0 test */
#define ATN_OUT_STATE (LINE_OUT(ATN_PORT, ATN_PIN) != 0)

/* DIO bit i -> PB pin; one IDR read / BSRR write, a few shifts each (cheap at 72 MHz) */
static inline uint32_t data_spread(uint8_t d)
{
	return ((d & 0x01u) << 9) | ((d & 0x02u) << 6) | ((d & 0x04u) << 8) | ((d & 0x08u) << 8) |
	       ((d & 0x10u) << 4) | ((d & 0x20u) << 1) | ((d & 0x40u) >> 2) | ((d & 0x80u) >> 4);
}
static inline uint8_t data_gather(uint32_t r)
{
	return (uint8_t)(((r >> 9) & 0x01u) | ((r >> 6) & 0x02u) | ((r >> 8) & 0x04u) | ((r >> 8) & 0x08u) |
	                 ((r >> 4) & 0x10u) | ((r >> 1) & 0x20u) | ((r << 2) & 0x40u) | ((r << 4) & 0x80u));
}

/* GPIB data is negative logic: a 1 bit pulls the line low. Replaces DDRD = dat / ~PIND. */
#define DATA_WRITE(d) (DATA_PORT->BSRR = (data_spread(d) << 16) | data_spread((uint8_t)~(d)))
#define DATA_RELEASE  (DATA_PORT->BSRR = DATA_MASK)
#define DATA_READ     data_gather(~DATA_PORT->IDR)

/* Replaces the DDR/PORT setup at the top of gpib_init(). Everything starts released; REN keeps its output level
 * (low from reset, as the AVR drives it), phy_init() then sets it. */
static inline void gpib_pins_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_AFIOEN | RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;
	AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE; /* frees PA15/PB3/PB4 */

	GPIOA->BSRR = (1u << IFC_PIN) | (1u << SRQ_PIN) | (1u << ATN_PIN);
	GPIOB->BSRR = DATA_MASK | (1u << EOI_PIN) | (1u << DAV_PIN) | (1u << NRFD_PIN) | (1u << NDAC_PIN);

	/* 0x7 = open-drain 50MHz, 0x4 = floating input */
	GPIOA->CRH = (GPIOA->CRH & 0x0FFFF000u) | 0x70000747u; /* PA8 OD, PA9 input, PA10 OD, PA15 OD */
	GPIOB->CRL = (GPIOB->CRL & 0x00F00FFFu) | 0x77077000u; /* PB3,4,6,7 OD */
	GPIOB->CRH = 0x77777777u;                              /* PB8..15 */
}

#endif /* GPIB_PINS_H_ */
