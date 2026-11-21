#ifndef GPIB_PHY_H_
#define GPIB_PHY_H_

#include <stdint.h>
#include <stdbool.h>

/* Byte-level IEEE 488 handshake. A byte is started, then polled until done; nothing here blocks except the
 * microsecond settle delays and IFC. Signal order and delays follow the v2 (AVR) firmware. */

typedef enum { PHY_BUSY, PHY_DONE, PHY_NO_LISTENER } phy_status_t;

void phy_init(void);  /* all lines released except REN (asserted), then IFC */
void phy_ifc(void);
void phy_ren(bool asserted);
void phy_atn(bool asserted); /* with the settle delays around an ATN change */
bool phy_atn_asserted(void);
bool phy_connected(void);    /* releases ATN to sense the bus: only between transactions */
bool phy_srq(void);
bool phy_probe_listener(void); /* releases ATN, then: does an addressed listener hold NDAC low? */

void phy_tx_start(uint8_t dat, bool eoi); /* set ATN first; EOI only with ATN released */
phy_status_t phy_tx_poll(void);

void phy_rx_start(void);  /* releases ATN */
phy_status_t phy_rx_poll(uint8_t *dat, bool *eoi);
bool phy_rx_cancel(void); /* stop waiting for a byte; false once one is being accepted */

#endif /* GPIB_PHY_H_ */
