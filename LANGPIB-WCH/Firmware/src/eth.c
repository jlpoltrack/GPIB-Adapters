#include <stddef.h>
#include "eth.h"
#include "board.h"
#include "ch32v4x7_eth.h"

/* RM 29 throughout. Descriptor bits follow the field tables (29-5, 29-10, 29-12), not the bit strips (RM errata). */

#define PHY_ADDR  1u    /* the internal PHY's default address (ETH_PHY_CR.PHYADDR_EN = 0) */
#define RX_BUFS   12u   /* lwipopts.h: TCP_WND stays below this; 6 missed ~0.03 % of frames with five clients */
#define TX_BUFS   4u
#define BUF_SIZE  1536u /* one whole frame per buffer: a TX frame can't span descriptors (RM 29.1.10.6.1) */

#define PHY_CR_RSTN         (1u << 31)
#define PHY_STATUS1_MDI     (1u << 5) /* 1: MDI, 0: MDI-X */
#define PHY_STATUS1_POL_REV (1u << 4)
#define PHY_ANAR_10_100     0x01E1u   /* 100TX-FD, 100TX, 10T-FD, 10T, selector 802.3 */
#define PHY_IMASK_LED_EN    (1u << 9) /* page 7 INTERRUPT_MASK.led_enable */
#define FEATURE_ACT_LED     (1u << 0)

static ETH_DMADESCTypeDef s_rx_desc[RX_BUFS] __attribute__((aligned(4)));
static ETH_DMADESCTypeDef s_tx_desc[TX_BUFS] __attribute__((aligned(4)));
static uint8_t s_rx_buf[RX_BUFS][BUF_SIZE] __attribute__((aligned(4)));
static uint8_t s_tx_buf[TX_BUFS][BUF_SIZE] __attribute__((aligned(4)));
static ETH_DMADESCTypeDef *s_rx, *s_tx; /* next to read / next to fill */

static uint32_t    s_phy_id;
static eth_link_t  s_link;
static eth_stats_t s_stats;

/* the DMA must see the buffer before the OWN bit (and the CPU be done with it before handing it back) */
static inline void dma_fence(void)
{
	__asm volatile("fence" ::: "memory");
}

static bool wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t us)
{
	uint32_t start = board_cycles(), cycles = us * (SystemCoreClock / 1000000u);

	while (*reg & mask)
		if ((board_cycles() - start) >= cycles)
			return false;
	return true;
}

/* --- MDIO: HCLK 200 MHz / 102 = 1.96 MHz MDC, ~33 us a frame ------------------------------------------------- */

#define MIIAR(reg) ((PHY_ADDR << 11) | (((uint32_t)(reg) << 6) & ETH_MACMIIAR_MR) | ETH_MACMIIAR_CR_Div102)

/* MIIAR must not change while MB is set (RM 29.2.1.4): a stuck access fails the next one instead of overwriting it */
static uint16_t phy_read(unsigned reg)
{
	if (!wait_clear(&ETH->MACMIIAR, ETH_MACMIIAR_MB, 1000))
		return 0xFFFFu;
	ETH->MACMIIAR = MIIAR(reg) | ETH_MACMIIAR_MB;
	if (!wait_clear(&ETH->MACMIIAR, ETH_MACMIIAR_MB, 1000))
		return 0xFFFFu;
	return (uint16_t)ETH->MACMIIDR;
}

static void phy_write(unsigned reg, uint16_t val)
{
	if (!wait_clear(&ETH->MACMIIAR, ETH_MACMIIAR_MB, 1000))
		return;
	ETH->MACMIIDR = val;
	ETH->MACMIIAR = MIIAR(reg) | ETH_MACMIIAR_MW | ETH_MACMIIAR_MB;
	wait_clear(&ETH->MACMIIAR, ETH_MACMIIAR_MB, 1000);
}

/* page 0, or page 7 for INTERRUPT_MASK and LED_CTRL; everything here leaves page 0 selected */
static void phy_page(uint16_t page)
{
	phy_write(PHY_PAG_SEL, page);
}

/* --- Init ---------------------------------------------------------------------------------------------------- */

/* RM 33.2.2: bits 15:8 are the inverse of 7:0 on a programmed part, so an unprogrammed 0xFFFFFFFF doesn't count */
bool eth_act_led_supported(void)
{
	uint32_t fs = *(const volatile uint32_t *)FEATURE_SIGN;
	return ((((fs >> 8) ^ fs) & 0xFFu) == 0xFFu) && (fs & FEATURE_ACT_LED);
}

/* FEATURE_SIGN bit 0 clear means LINK only, not no LEDs (RM 33.2.2), so the LEDs are always set up; WCH's PHY_LEDCfg()
 * skips them. LED_CTRL (page 7, 0x15) isn't in the RM: WCH's |= 3 << 5, kept for parts with ACT. */
static void led_init(void)
{
	AFIO->PCFR1 |= AFIO_PCFR1_ETHPHY_LED_REMAP;                 /* LED0 PD14 (green), LED1 PD15 (yellow) */
	GPIOD->CFGHR = (GPIOD->CFGHR & ~(0xFFu << 24)) | (0xBBu << 24); /* AF push-pull, 50 MHz */

	phy_page(PHY_REG_PAGE7);
	if (eth_act_led_supported())
		phy_write(PHY_LED_CTRL, phy_read(PHY_LED_CTRL) | (3u << 5));
	else
		board_log("ETH: FEATURE_SIGN 0x%08lx: link LEDs only\n", (unsigned long)*(const volatile uint32_t *)FEATURE_SIGN);
	phy_write(PHY_INTERRUPT_MASK, (uint16_t)((phy_read(PHY_INTERRUPT_MASK) & ~(1u << 5)) | PHY_IMASK_LED_EN));
	phy_page(PHY_REG_PAGE0);
}

/* chained descriptors (TCH / RCH), one BUF_SIZE buffer each, as WCH's ETH_DMA{Tx,Rx}DescChainInit() */
static void desc_init(void)
{
	for (unsigned i = 0; i < RX_BUFS; i++) {
		s_rx_desc[i].ControlBufferSize = ETH_DMARxDesc_RCH | BUF_SIZE;
		s_rx_desc[i].Buffer1Addr = (uint32_t)(uintptr_t)s_rx_buf[i];
		s_rx_desc[i].Buffer2NextDescAddr = (uint32_t)(uintptr_t)&s_rx_desc[(i + 1) % RX_BUFS];
		s_rx_desc[i].Status = ETH_DMARxDesc_OWN;
	}
	for (unsigned i = 0; i < TX_BUFS; i++) {
		s_tx_desc[i].Status = ETH_DMATxDesc_TCH;
		s_tx_desc[i].ControlBufferSize = 0;
		s_tx_desc[i].Buffer1Addr = (uint32_t)(uintptr_t)s_tx_buf[i];
		s_tx_desc[i].Buffer2NextDescAddr = (uint32_t)(uintptr_t)&s_tx_desc[(i + 1) % TX_BUFS];
	}
	s_rx = s_rx_desc;
	s_tx = s_tx_desc;
	dma_fence();
	ETH->DMARDLAR = (uint32_t)(uintptr_t)s_rx_desc;
	ETH->DMATDLAR = (uint32_t)(uintptr_t)s_tx_desc;
}

/* WCH's ETH_Configuration() + ETH_Init(), minus the ETH interrupt. Needs the 25 MHz HSE (ETH PLL reference). */
bool eth_init(const uint8_t mac[6])
{
	bool ok = true;

	RCC->CTLR |= RCC_ETHPLLON;
	for (unsigned i = 0; (i < 300) && !(RCC->CTLR & RCC_ETHPLLRDY); i++)
		board_delay_us(10);
	if (!(RCC->CTLR & RCC_ETHPLLRDY)) {
		board_log("ETH: ETH PLL not locked\n");
		ok = false;
	}

	RCC->HBPCENR |= RCC_ETHMACEN;
	RCC->HBRSTR |= RCC_ETHMACRST;
	RCC->HBRSTR &= ~RCC_ETHMACRST;

	/* PHY out of power-down, then out of reset. PHY_CR reads as 0, so WCH's &= ~PD then |= RSTN is exactly this. */
	ETH->PHY_CR = 0;
	board_delay_us(10);
	ETH->PHY_CR = PHY_CR_RSTN;

	ETH->DMABMR |= ETH_DMABMR_SR;
	if (!wait_clear(&ETH->DMABMR, ETH_DMABMR_SR, 100000)) {
		board_log("ETH: DMA software reset stuck\n");
		ok = false;
	}
	ETH->PHY_CR = PHY_CR_RSTN; /* SR resets "all MAC subsystems" (RM 29.2.4.1): PHY_CR may be one, and reads as 0 */

	/* WCH's settings: watchdog and jabber on, 96-bit IFG, no checksum offload, no pad/CRC strip; perfect address
	 * filter plus broadcast, and all multicast (PAM: mDNS), no flow control; TX store-and-forward; error
	 * frames (FEF: all but runts) and undersized good-CRC frames (FUGF) forwarded, eth_rx drops them */
	ETH->MACMIIAR = ETH_MACMIIAR_CR_Div102;
	ETH->MACCR = 0;
	ETH->MACFFR = ETH_MACFFR_PAM;
	ETH->MACHTHR = 0;
	ETH->MACHTLR = 0;
	ETH->MACFCR = 0;
	ETH->MACVLANTR = 0;
	ETH->DMAOMR = ETH_DMAOMR_TSF | ETH_DMAOMR_FEF | ETH_DMAOMR_FUGF;
	ETH->DMAIER = 0;
	ETH->MACA0HR = ((uint32_t)mac[5] << 8) | mac[4];
	ETH->MACA0LR = mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16) | ((uint32_t)mac[3] << 24);
	ETH->MMCTIMR = ETH_MMCTIMR_TGFM; /* WCH masks these MMC counter interrupts */
	ETH->MMCRIMR = ETH_MMCRIMR_RGUFM | ETH_MMCRIMR_RFCEM;

	/* WCH doesn't wait for the BCR reset to self-clear; the LED writes below would race it */
	phy_write(PHY_BCR, PHY_Reset);
	for (unsigned i = 0; (i < 100) && (phy_read(PHY_BCR) & PHY_Reset); i++)
		board_delay_us(1000);
	if (phy_read(PHY_BCR) & PHY_Reset) {
		board_log("ETH: PHY reset stuck (MDIO?)\n");
		ok = false;
	}
	phy_page(PHY_REG_PAGE0);
	s_phy_id = ((uint32_t)phy_read(PHY_PHYIDR1) << 16) | phy_read(PHY_PHYIDR2);

	led_init();

	/* 10/100 full/half advertised. Auto-MDIX and polarity correction are the PHY's defaults and nothing here
	 * touches them: this board swaps the pairs and the polarity at the jack, so it depends on both. */
	phy_write(PHY_ANAR, PHY_ANAR_10_100);
	phy_write(PHY_BCR, PHY_AutoNegotiation | PHY_Restart_AutoNegotiation);

	desc_init();
	return ok;
}

uint32_t eth_phy_id(void)
{
	return s_phy_id;
}

/* --- Link ---------------------------------------------------------------------------------------------------- */

/* WCH's ETH_LinkUpCfg(): duplex and speed into MACCR, then ETH_Start(). MACCR.FES is RO in the RM (29.2.4.1, and
 * the Chinese original), so its write is presumably ignored; WCH writes it, and so do we. DM is RW and matters. */
static void mac_start(const eth_link_t *l)
{
	uint32_t cr = ETH->MACCR & ~(ETH_MACCR_FES | ETH_MACCR_DM | (1u << 15));

	if (l->fast)
		cr |= ETH_MACCR_FES;
	if (l->full)
		cr |= ETH_MACCR_DM;
	ETH->MACCR = cr | ETH_MACCR_TE;
	ETH->DMAOMR |= ETH_DMAOMR_FTF; /* the RM doesn't say FTF self-clears; WCH's library polls it as if it did */
	if (!wait_clear(&ETH->DMAOMR, ETH_DMAOMR_FTF, 1000)) {
		ETH->DMAOMR &= ~ETH_DMAOMR_FTF;
		board_log("ETH: TX FIFO flush didn't clear\n");
	}
	ETH->MACCR |= ETH_MACCR_RE;
	ETH->DMAOMR |= ETH_DMAOMR_ST | ETH_DMAOMR_SR;
}

/* After WCH's ETH_PHYLink(): up once BSR shows a link and, if the partner negotiates (ANER bit 0; WCH tests ANLPAR,
 * which reads its selector 0x0001 anyway), negotiation is done. The link bit latches low (IEEE 802.3 22.2.4.2.13). */
bool eth_link_poll(eth_link_t *link)
{
	eth_link_t l = { 0 };
	uint16_t bsr = phy_read(PHY_BSR);

	if ((bsr != 0xFFFFu) && (bsr & PHY_Linked_Status)) {
		uint16_t bcr = phy_read(PHY_BCR), aner = phy_read(PHY_ANER);
		l.up = (bcr != 0xFFFFu) && (aner != 0xFFFFu) &&
		       (!(bcr & PHY_AutoNegotiation) || !(aner & 1u) || (bsr & PHY_AutoNego_Complete));
	}
	uint16_t st = l.up ? phy_read(PHY_STATUS) : 0xFFFFu;
	l.up = l.up && (st != 0xFFFFu); /* an MDIO timeout decodes as 100M full duplex: not a link */
	if (l.up) {
		l.fast = (st & PHY_SPEED_100M_MODE) != 0;
		l.full = (st & PHY_FULL_DUPLEX_MODE) != 0;
		l.mdix = !(st & PHY_STATUS1_MDI);
		l.pol_rev = (st & PHY_STATUS1_POL_REV) != 0;
	}

	uint32_t mf = ETH->DMAMFBOCR; /* clears on read */
	s_stats.rx_missed += (mf & 0xFFFFu) + ((mf >> 17) & 0x7FFu);

	bool changed = (l.up != s_link.up) || (l.up && ((l.fast != s_link.fast) || (l.full != s_link.full)));
	if (changed && l.up)
		mac_start(&l);
	s_link = l;
	*link = l;
	return changed;
}

/* --- Frames -------------------------------------------------------------------------------------------------- */

uint8_t *eth_tx_buf(uint32_t wait_us)
{
	uint32_t start = board_cycles(), cycles = wait_us * (SystemCoreClock / 1000000u);

	while (s_tx->Status & ETH_DMATxDesc_OWN) {
		if ((board_cycles() - start) >= cycles) {
			s_stats.tx_full++;
			return NULL;
		}
	}
	return (uint8_t *)(uintptr_t)s_tx->Buffer1Addr;
}

/* WCH's MACRAW_Tx(): one descriptor per frame, FS | LS, then clear TBUS and poll-demand the TX DMA */
void eth_tx_send(unsigned len)
{
	/* the frame this descriptor carried last time; ES leaves out the half-duplex carrier and collision errors */
	if (s_tx->Status & (ETH_DMATxDesc_ES | ETH_DMATxDesc_NC | ETH_DMATxDesc_LCA | ETH_DMATxDesc_LCO | ETH_DMATxDesc_EC))
		s_stats.tx_err++;
	s_tx->ControlBufferSize = len & ETH_DMATxDesc_TBS1;
	dma_fence();
	s_tx->Status = ETH_DMATxDesc_OWN | ETH_DMATxDesc_TCH | ETH_DMATxDesc_FS | ETH_DMATxDesc_LS;
	dma_fence();
	s_tx = (ETH_DMADESCTypeDef *)(uintptr_t)s_tx->Buffer2NextDescAddr;
	ETH->DMASR = ETH_DMASR_TBUS;
	ETH->DMATPDR = 0;
	s_stats.tx++;
}

/* back to the DMA, and resume it if it ran out of descriptors (WCH's ETH_HandleRxPkt()) */
static void rx_next(void)
{
	dma_fence();
	s_rx->Status = ETH_DMARxDesc_OWN;
	dma_fence();
	s_rx = (ETH_DMADESCTypeDef *)(uintptr_t)s_rx->Buffer2NextDescAddr;
	if (ETH->DMASR & ETH_DMASR_RBUS) {
		ETH->DMASR = ETH_DMASR_RBUS;
		ETH->DMARPDR = 0;
	}
}

unsigned eth_rx(const uint8_t **frame)
{
	for (unsigned i = 0; i < RX_BUFS; i++) {
		uint32_t st = s_rx->Status;
		if (st & ETH_DMARxDesc_OWN)
			return 0;
		unsigned len = (st & ETH_DMARxDesc_FL) >> ETH_DMARXDESC_FRAME_LENGTHSHIFT; /* with the 4-byte FCS */
		bool whole = (st & ETH_DMARxDesc_FS) && (st & ETH_DMARxDesc_LS);
		if (!(st & ETH_DMARxDesc_ES) && whole && (len > 4) && ((len - 4) <= ETH_FRAME_MAX)) {
			*frame = (const uint8_t *)(uintptr_t)s_rx->Buffer1Addr;
			return len - 4;
		}
		s_stats.rx_err++;
		rx_next();
	}
	return 0;
}

void eth_rx_release(void)
{
	s_stats.rx++;
	rx_next();
}

const eth_stats_t *eth_stats(void)
{
	return &s_stats;
}
