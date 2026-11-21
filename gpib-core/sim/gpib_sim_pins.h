#ifndef GPIB_SIM_PINS_H_
#define GPIB_SIM_PINS_H_

/* gpib_pins.h on the host: the controller's lines are bits of sim_ctrl, reads run the simulated instruments */

#include <stdint.h>

/* line levels, 1 = released (high) */
#define L_DIO  0x00FFu
#define L_EOI  (1u << 8)
#define L_DAV  (1u << 9)
#define L_NRFD (1u << 10)
#define L_NDAC (1u << 11)
#define L_IFC  (1u << 12)
#define L_SRQ  (1u << 13)
#define L_ATN  (1u << 14)
#define L_REN  (1u << 15)
#define L_ALL  0xFFFFu

extern uint32_t sim_ctrl;
uint32_t sim_bus(void);
void sim_pins_init(void);

#define ATN_LOW   (sim_ctrl &= ~L_ATN)
#define ATN_HIGH  (sim_ctrl |= L_ATN)
#define NDAC_LOW  (sim_ctrl &= ~L_NDAC)
#define NDAC_HIGH (sim_ctrl |= L_NDAC)
#define NRFD_LOW  (sim_ctrl &= ~L_NRFD)
#define NRFD_HIGH (sim_ctrl |= L_NRFD)
#define DAV_LOW   (sim_ctrl &= ~L_DAV)
#define DAV_HIGH  (sim_ctrl |= L_DAV)
#define EOI_LOW   (sim_ctrl &= ~L_EOI)
#define EOI_HIGH  (sim_ctrl |= L_EOI)
#define REN_LOW   (sim_ctrl &= ~L_REN)
#define REN_HIGH  (sim_ctrl |= L_REN)
#define IFC_LOW   (sim_ctrl &= ~L_IFC)
#define IFC_HIGH  (sim_ctrl |= L_IFC)

#define DAV_STATE  (sim_bus() & L_DAV)
#define NDAC_STATE (sim_bus() & L_NDAC)
#define NRFD_STATE (sim_bus() & L_NRFD)
#define ATN_STATE  (sim_bus() & L_ATN)
#define EOI_STATE  (sim_bus() & L_EOI)
#define SRQ_STATE  (sim_bus() & L_SRQ)

#define ATN_OUT_STATE ((sim_ctrl & L_ATN) != 0)

#define DATA_WRITE(d) (sim_ctrl = (sim_ctrl & ~L_DIO) | (uint8_t)~(d))
#define DATA_RELEASE  (sim_ctrl |= L_DIO)
#define DATA_READ     ((uint8_t)~sim_bus())

#define gpib_pins_init() sim_pins_init()

#endif /* GPIB_SIM_PINS_H_ */
