#include "gpib_phy.h"
#include "gpib_pins.h"
#include "board.h"

/* Data valid to DAV. The AVR got ~0.7 us from loop overhead; at 72 MHz there would be almost none. */
#define T1_SETTLE_US 2 /* IEEE 488.1 T1 minimum for open-collector drivers */

static enum { IDLE, TX_WAIT_NRFD, TX_WAIT_NDAC, RX_WAIT_DAV_LOW, RX_WAIT_DAV_HIGH } s_state;
static uint8_t s_rx_dat;
static bool    s_rx_eoi;
static bool    s_ren = true;

void phy_init(void)
{
	gpib_pins_init();
	phy_ren(s_ren); /* recovery keeps the host's REN choice */
	s_state = IDLE;
	phy_ifc();
}

void phy_ifc(void)
{
	NRFD_HIGH;
	IFC_LOW;
	board_delay_us(120);
	IFC_HIGH;
	board_delay_ms(10);
}

void phy_ren(bool asserted)
{
	s_ren = asserted;
	if (asserted)
		REN_LOW;
	else
		REN_HIGH;
}

bool phy_atn_asserted(void)
{
	return !ATN_OUT_STATE;
}

/* The ATN timing was measured on a commercial GPIB adapter */
void phy_atn(bool asserted)
{
	if (asserted == phy_atn_asserted())
		return;
	board_delay_us(220);
	if (asserted)
		ATN_LOW;
	else
		ATN_HIGH;
	board_delay_us(70);
}

/* ATN can only read low with ATN released when no instrument is connected or powered */
bool phy_connected(void)
{
	if (phy_atn_asserted()) {
		board_delay_us(10);
		ATN_HIGH;
		board_delay_us(10);
	}
	return ATN_STATE != 0;
}

bool phy_srq(void)
{
	return !SRQ_STATE;
}

bool phy_probe_listener(void)
{
	ATN_HIGH;
	board_delay_ms(2);
	return (NDAC_STATE == 0) && (ATN_STATE != 0);
}

void phy_tx_start(uint8_t dat, bool eoi)
{
	DAV_HIGH;
	NRFD_HIGH;
	NDAC_HIGH;
	if (eoi)
		EOI_LOW;
	DATA_WRITE(dat);
	board_delay_us(T1_SETTLE_US);
	s_state = TX_WAIT_NRFD;
}

static void tx_end(void)
{
	DAV_HIGH;
	DATA_RELEASE;
	EOI_HIGH;
	s_state = IDLE;
}

phy_status_t phy_tx_poll(void)
{
	switch (s_state) {
	case TX_WAIT_NRFD:
		if (NRFD_STATE == 0)
			return PHY_BUSY;
		if (NDAC_STATE != 0) { /* NRFD and NDAC both high: nobody is listening */
			tx_end();
			return PHY_NO_LISTENER;
		}
		DAV_LOW;
		s_state = TX_WAIT_NDAC;
		/* fall through */
	case TX_WAIT_NDAC:
		if (NDAC_STATE == 0)
			return PHY_BUSY;
		tx_end();
		return PHY_DONE;
	default:
		return PHY_DONE;
	}
}

/* NDAC goes low before ATN is released, so a talker can't see its first byte as accepted */
void phy_rx_start(void)
{
	NDAC_LOW;
	phy_atn(false);
	NRFD_HIGH;
	s_state = RX_WAIT_DAV_LOW;
}

phy_status_t phy_rx_poll(uint8_t *dat, bool *eoi)
{
	switch (s_state) {
	case RX_WAIT_DAV_LOW:
		if (DAV_STATE != 0)
			return PHY_BUSY;
		NRFD_LOW;
		s_rx_dat = DATA_READ;
		s_rx_eoi = (EOI_STATE == 0);
		NDAC_HIGH;
		s_state = RX_WAIT_DAV_HIGH;
		/* fall through */
	case RX_WAIT_DAV_HIGH:
		if (DAV_STATE == 0)
			return PHY_BUSY;
		NDAC_LOW; /* with NRFD low: hold the talker off until the next byte */
		s_state = IDLE;
		*dat = s_rx_dat;
		*eoi = s_rx_eoi;
		return PHY_DONE;
	default:
		return PHY_DONE;
	}
}

bool phy_rx_cancel(void)
{
	if (s_state == RX_WAIT_DAV_HIGH)
		return false;
	if (s_state == RX_WAIT_DAV_LOW) {
		NRFD_LOW;
		board_delay_us(2); /* a talker that saw NRFD high just before may still be setting DAV */
		if (DAV_STATE == 0)
			return false; /* a byte is waiting: phy_rx_poll() takes it */
		s_state = IDLE;
	}
	return true;
}
