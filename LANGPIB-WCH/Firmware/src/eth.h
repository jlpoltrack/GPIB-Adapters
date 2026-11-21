#ifndef ETH_H_
#define ETH_H_

/* The CH32V407's on-chip 10/100 MAC and PHY (RM 29), polled: no ETH interrupt. Chained DMA descriptors, one frame
 * per buffer, frames copied in and out. Register-level port of WCH's eth_driver.c (EVT MAC_RAW). */

#include <stdbool.h>
#include <stdint.h>

#define ETH_FRAME_MAX 1514u /* without the FCS: the MAC adds it on TX and eth_rx() strips it */

typedef struct {
	bool up;
	bool fast;    /* 100M, else 10M */
	bool full;    /* full duplex */
	bool mdix;    /* PHY_STATUS1: MDI-X (pairs are swapped at the jack; which way it resolves depends on the partner) */
	bool pol_rev; /* PHY_STATUS1: polarity reversed (also swapped at the jack; probably only reported at 10M) */
} eth_link_t;

typedef struct {
	uint32_t rx, rx_err, rx_missed; /* rx_missed: DMA missed-frame counters (ring full or FIFO overflow) */
	uint32_t tx, tx_err, tx_full;   /* tx_full: no free TX descriptor */
} eth_stats_t;

bool     eth_init(const uint8_t mac[6]); /* clocks, MAC/DMA, PHY reset and auto-negotiation, LEDs; false on a timeout */
uint32_t eth_phy_id(void);               /* PHYIDR1 << 16 | PHYIDR2 */
bool     eth_act_led_supported(void);    /* FEATURE_SIGN bit 0: the LEDs show LINK and ACT, else LINK only */

/* Reads the PHY (1 MDIO read with no link, 4 with one: ~0.13 ms); on link up sets the MAC's speed and duplex and
 * starts it. Returns true when the state changed. */
bool eth_link_poll(eth_link_t *link);

/* TX: a free buffer of ETH_FRAME_MAX bytes (waiting up to wait_us for the DMA) or NULL, then hand len bytes of
 * it to the DMA */
uint8_t *eth_tx_buf(uint32_t wait_us);
void     eth_tx_send(unsigned len);

/* RX: length of the next good frame (0: none), valid in *frame until eth_rx_release() */
unsigned eth_rx(const uint8_t **frame);
void     eth_rx_release(void);

const eth_stats_t *eth_stats(void);

#endif /* ETH_H_ */
