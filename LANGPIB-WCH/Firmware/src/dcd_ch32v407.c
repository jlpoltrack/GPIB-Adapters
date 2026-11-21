/* TinyUSB device driver for the CH32V407's USBHS1 (RM 22), run as a full-speed device on PA11/PA12, plus the
 * board's USB clock and connect/disconnect. Register-level behaviour follows WCH's V407 examples (EVT USBHS
 * CH372Device, Dual_device usbhs1_dcp.c, IAP USB_UART) where the RM is silent. One packet per interrupt: no burst. */

#include <stddef.h>
#include <string.h>
#include "board.h"
#include "tusb.h"
#include "device/dcd.h"

#define EP_MAX  TUP_DCD_ENDPOINT_MAX /* EP0-EP7 */
#define EP0_MPS CFG_TUD_ENDPOINT0_SIZE
#define EP_MPS  64u                  /* largest full-speed bulk/interrupt packet */

/* the header's RCC_UTMI1ON is bit 31; RM 3.4.13 and WCH's RCC_UTMI1cmd() use bit 30 (bit 31 is UTMI2) */
#define RCC_UTMI1_ON (1u << 30)

/* RAM the controller's DMA reaches (RM 22: buffer addresses must be 4-byte aligned) */
#define DMA_RAM_START 0x20000000u
#define DMA_RAM_END   0x20020000u

/* The per-endpoint registers sit at a fixed stride (RM 22.2.1.23-35); check the header against the RM */
_Static_assert(offsetof(USBHSD_TypeDef, UEP0_DMA) == 0x20, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP1_RX_DMA) == 0x24, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP1_TX_DMA) == 0x40, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP0_MAX_LEN) == 0x5C, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP0_RX_LEN) == 0x7C, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP1_RX_LEN) == 0x80, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP0_TX_LEN) == 0x9C, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP0_TX_CTRL) == 0x9E, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP1_TX_LEN) == 0xA0, "USBHSD layout");
_Static_assert(offsetof(USBHSD_TypeDef, UEP_TX_ISO) == 0xBC, "USBHSD layout");

#define USBD USBHS1D

/* n = 1..7 for the DMA registers; the rest index from EP0 */
#define EP_TX_DMA(n)  (((volatile uint32_t *)&USBD->UEP1_TX_DMA)[(n) - 1])
#define EP_RX_DMA(n)  (((volatile uint32_t *)&USBD->UEP1_RX_DMA)[(n) - 1])
#define EP_MAX_LEN(n) (((volatile uint32_t *)&USBD->UEP0_MAX_LEN)[(n)])
#define EP_RX_LEN(n)  (((volatile uint16_t *)&USBD->UEP0_RX_LEN)[(n) * 2])
#define EP_TX_LEN(n)  (((volatile uint16_t *)&USBD->UEP0_TX_LEN)[(n) * 2])
#define EP_TX_CTRL(n) (((volatile uint8_t *)&USBD->UEP0_TX_CTRL)[(n) * 4])
#define EP_RX_CTRL(n) (((volatile uint8_t *)&USBD->UEP0_RX_CTRL)[(n) * 4])

typedef struct {
	uint8_t *buf;
	uint16_t total;  /* bytes asked for */
	uint16_t done;   /* bytes moved so far */
	uint16_t pkt;    /* IN: length of the packet in flight */
	uint16_t mps;
	bool active;
	bool bounce;     /* the packet in flight uses s_bounce */
} xfer_t;

static xfer_t s_xfer[EP_MAX][2];
static __attribute__((aligned(4))) uint8_t s_ep0_buf[EP0_MPS]; /* SETUP, IN and OUT share it (RM 22.2.1.22) */
static __attribute__((aligned(4))) uint8_t s_bounce[EP_MAX][2][EP_MPS];
static uint8_t s_ep0_tog[2];   /* EP0 has no auto-toggle: next DATAx per direction */
static uint8_t s_pending_addr; /* SET_ADDRESS: applied once its status stage is out */

static inline void compiler_barrier(void)
{
	__asm volatile("" ::: "memory");
}

/* buffers the DMA can use as they are; anything else goes through s_bounce */
static bool dma_ok(const uint8_t *p, uint16_t len)
{
	uint32_t a = (uint32_t)(uintptr_t)p;
	return !(a & 3u) && (a >= DMA_RAM_START) && ((a + len) <= DMA_RAM_END);
}

/* Response bits only. DONE/NAK_ACT are write-0-to-clear (RM 22.2.1.31-35): writing back what was read keeps them,
 * so a task-context arm can't wipe a completion (or a SETUP) that just came in. */
static void tx_res(uint8_t n, uint8_t res)
{
	EP_TX_CTRL(n) = (EP_TX_CTRL(n) & ~USBHS_UEP_T_RES_MASK) | res;
}

static void rx_res(uint8_t n, uint8_t res)
{
	EP_RX_CTRL(n) = (EP_RX_CTRL(n) & ~USBHS_UEP_R_RES_MASK) | res;
}

/* DATA0 again on EP1-7: the toggle bits only take a write with auto-toggle off (WCH _set_endp_toggle) */
static void reset_toggle(uint8_t n, tusb_dir_t dir)
{
	uint16_t bit = (uint16_t)(1u << n);

	if (dir == TUSB_DIR_IN) {
		USBD->UEP_TX_TOG_AUTO &= (uint16_t)~bit;
		EP_TX_CTRL(n) = (EP_TX_CTRL(n) & ~USBHS_UEP_T_TOG_MASK) | USBHS_UEP_T_TOG_DATA0;
		USBD->UEP_TX_TOG_AUTO |= bit;
	} else {
		USBD->UEP_RX_TOG_AUTO &= (uint16_t)~bit;
		EP_RX_CTRL(n) = (EP_RX_CTRL(n) & ~USBHS_UEP_R_TOG_MASK) | USBHS_UEP_R_TOG_DATA0;
		USBD->UEP_RX_TOG_AUTO |= bit;
	}
}

static void ep0_arm_in(void)
{
	xfer_t *x = &s_xfer[0][TUSB_DIR_IN];
	uint16_t len = (uint16_t)TU_MIN(x->total - x->done, EP0_MPS);

	if (len)
		memcpy(s_ep0_buf, x->buf + x->done, len);
	x->pkt = len;
	USBD->UEP0_TX_LEN = len;
	compiler_barrier();
	USBD->UEP0_TX_CTRL = (USBD->UEP0_TX_CTRL & (USBHS_UEP_T_DONE | USBHS_UEP_T_NAK_ACT)) |
	                     (s_ep0_tog[TUSB_DIR_IN] ? USBHS_UEP_T_TOG_DATA1 : 0) | USBHS_UEP_T_RES_ACK;
}

static void ep0_arm_out(void)
{
	compiler_barrier();
	USBD->UEP0_RX_CTRL = (USBD->UEP0_RX_CTRL & (USBHS_UEP_R_DONE | USBHS_UEP_R_NAK_ACT)) |
	                     (s_ep0_tog[TUSB_DIR_OUT] ? USBHS_UEP_R_TOG_DATA1 : 0) | USBHS_UEP_R_RES_ACK;
}

static void epn_arm_in(uint8_t n)
{
	xfer_t *x = &s_xfer[n][TUSB_DIR_IN];
	uint16_t len = (uint16_t)TU_MIN(x->total - x->done, x->mps);
	uint8_t *p = x->buf + x->done;

	x->bounce = !len || !dma_ok(p, len);
	if (x->bounce) {
		if (len)
			memcpy(s_bounce[n][TUSB_DIR_IN], p, len);
		p = s_bounce[n][TUSB_DIR_IN];
	}
	x->pkt = len;
	EP_TX_DMA(n) = (uint32_t)(uintptr_t)p;
	EP_TX_LEN(n) = len;
	compiler_barrier();
	tx_res(n, USBHS_UEP_T_RES_ACK);
}

/* a short tail (less than a packet left) lands in s_bounce, so a full packet can't overrun the caller's buffer */
static void epn_arm_out(uint8_t n)
{
	xfer_t *x = &s_xfer[n][TUSB_DIR_OUT];
	uint8_t *p = x->buf + x->done;

	x->bounce = ((x->total - x->done) < x->mps) || !dma_ok(p, x->mps);
	EP_RX_DMA(n) = (uint32_t)(uintptr_t)(x->bounce ? s_bounce[n][TUSB_DIR_OUT] : p);
	compiler_barrier();
	rx_res(n, USBHS_UEP_R_RES_ACK);
}

static void ep0_reset(void)
{
	s_xfer[0][TUSB_DIR_OUT] = (xfer_t){ .mps = EP0_MPS };
	s_xfer[0][TUSB_DIR_IN]  = (xfer_t){ .mps = EP0_MPS };
	s_ep0_tog[TUSB_DIR_OUT] = s_ep0_tog[TUSB_DIR_IN] = 1;
	USBD->UEP0_DMA     = (uint32_t)(uintptr_t)s_ep0_buf;
	USBD->UEP0_MAX_LEN = EP0_MPS;
	USBD->UEP0_TX_LEN  = 0;
	USBD->UEP0_TX_CTRL = USBHS_UEP_T_RES_NAK;
	USBD->UEP0_RX_CTRL = USBHS_UEP_R_RES_ACK; /* as WCH's Endp_Init; SETUPs are taken whatever the response */
}

/* EP1-7 off, NAKing, toggles back to DATA0 */
static void epn_reset_all(void)
{
	USBD->UEP_TX_EN = USBHS_UEP0_T_EN;
	USBD->UEP_RX_EN = USBHS_UEP0_R_EN;
	USBD->UEP_TX_TOG_AUTO = 0;
	USBD->UEP_RX_TOG_AUTO = 0;
	USBD->UEP_TX_ISO = 0;
	USBD->UEP_RX_ISO = 0;
	for (uint8_t n = 1; n < EP_MAX; n++) {
		s_xfer[n][TUSB_DIR_OUT] = (xfer_t){ 0 };
		s_xfer[n][TUSB_DIR_IN]  = (xfer_t){ 0 };
		EP_TX_LEN(n)  = 0;
		EP_TX_CTRL(n) = USBHS_UEP_T_RES_NAK;
		EP_RX_CTRL(n) = USBHS_UEP_R_RES_NAK;
	}
}

/*------------------------------------------------------------------------------------------------------------*/
/* Interrupt */

static void handle_setup(void)
{
	uint8_t setup[8];

	memcpy(setup, s_ep0_buf, sizeof(setup));
	/* a SETUP cancels whatever EP0 had queued; data and status stages start at DATA1 */
	s_xfer[0][TUSB_DIR_OUT].active = s_xfer[0][TUSB_DIR_IN].active = false;
	s_pending_addr = 0; /* a SET_ADDRESS whose status stage never finished */
	s_ep0_tog[TUSB_DIR_OUT] = s_ep0_tog[TUSB_DIR_IN] = 1;
	USBD->UEP0_TX_CTRL = USBHS_UEP_T_TOG_DATA1 | USBHS_UEP_T_RES_NAK;
	USBD->UEP0_RX_CTRL = USBHS_UEP_R_TOG_DATA1 | USBHS_UEP_R_RES_NAK; /* also clears R_DONE */
	dcd_event_setup_received(0, setup, true);
}

static void handle_in(uint8_t n)
{
	xfer_t *x = &s_xfer[n][TUSB_DIR_IN];

	/* NAK and clear T_DONE in one write, as WCH's examples do after an IN */
	EP_TX_CTRL(n) = (EP_TX_CTRL(n) & ~(USBHS_UEP_T_RES_MASK | USBHS_UEP_T_DONE)) | USBHS_UEP_T_RES_NAK;
	if (n == 0) {
		s_ep0_tog[TUSB_DIR_IN] ^= 1;
		if (s_pending_addr && !x->total) { /* SET_ADDRESS status ZLP is out: WCH sets DEV_AD here too */
			USBD->DEV_AD = s_pending_addr;
			s_pending_addr = 0;
		}
	}
	if (!x->active)
		return;
	x->done += x->pkt;
	if (x->done < x->total) {
		if (n == 0)
			ep0_arm_in();
		else
			epn_arm_in(n);
		return;
	}
	x->active = false;
	dcd_event_xfer_complete(0, (uint8_t)(n | TUSB_DIR_IN_MASK), x->done, XFER_RESULT_SUCCESS, true);
}

static void handle_out(uint8_t n)
{
	xfer_t *x = &s_xfer[n][TUSB_DIR_OUT];
	uint8_t ctrl = EP_RX_CTRL(n);

	/* a repeat of the last packet (the host missed our ACK): drop it, keep receiving (WCH usbhs1_dcp.c) */
	if (!(ctrl & USBHS_UEP_R_TOG_MATCH)) {
		EP_RX_CTRL(n) = (ctrl & ~(USBHS_UEP_R_RES_MASK | USBHS_UEP_R_DONE)) |
		                (x->active ? USBHS_UEP_R_RES_ACK : USBHS_UEP_R_RES_NAK);
		return;
	}
	uint16_t len = EP_RX_LEN(n);
	EP_RX_CTRL(n) = (ctrl & ~(USBHS_UEP_R_RES_MASK | USBHS_UEP_R_DONE)) | USBHS_UEP_R_RES_NAK;
	if (n == 0)
		s_ep0_tog[TUSB_DIR_OUT] ^= 1;
	if (!x->active)
		return;

	uint16_t take = (uint16_t)TU_MIN(len, x->total - x->done);
	if (take && !n)
		memcpy(x->buf + x->done, s_ep0_buf, take);
	else if (take && x->bounce)
		memcpy(x->buf + x->done, s_bounce[n][TUSB_DIR_OUT], take);
	x->done += take;
	if ((len == x->mps) && (x->done < x->total)) {
		if (n == 0)
			ep0_arm_out();
		else
			epn_arm_out(n);
		return;
	}
	x->active = false;
	dcd_event_xfer_complete(0, n, x->done, XFER_RESULT_SUCCESS, true);
}

void dcd_int_handler(uint8_t rhport)
{
	(void)rhport;

	/* one event per pass, in WCH's order: transfer, bus reset, suspend. Capped: a flag that won't clear
	 * re-raises the interrupt instead of hanging here. */
	for (int i = 0; i < 16; i++) {
		uint8_t flags = USBD->INT_FG & USBD->INT_EN;

		if (flags & USBHS_UDIF_RTX_ACT) {
			uint8_t st = USBD->INT_ST, n = st & USBHS_UDIS_EP_ID_MASK;

			if (st & USBHS_UDIS_EP_DIR)
				handle_in(n);
			else if (!n && (USBD->UEP0_RX_CTRL & USBHS_UEP_R_SETUP_IS))
				handle_setup();
			else
				handle_out(n);
		} else if (flags & USBHS_UDIF_BUS_RST) {
			USBD->DEV_AD = 0;
			s_pending_addr = 0;
			epn_reset_all();
			ep0_reset();
			dcd_event_bus_reset(0, TUSB_SPEED_FULL, true);
			USBD->INT_FG = USBHS_UDIF_BUS_RST;
		} else if (flags & USBHS_UDIF_SUSPEND) {
			/* the flag marks both edges; MIS_ST says which (WCH) */
			USBD->INT_FG = USBHS_UDIF_SUSPEND;
			dcd_event_bus_signal(0, (USBD->MIS_ST & USBHS_UDMS_SUSPEND) ? DCD_EVENT_SUSPEND : DCD_EVENT_RESUME,
			                     true);
		} else if (flags & USBHS_UDIF_RX_SOF) {
			USBD->INT_FG = USBHS_UDIF_RX_SOF;
			dcd_event_sof(0, USBD->FRAME_NO & USBHS_UD_FRAME_NO, true);
		} else if (flags) {
			USBD->INT_FG = flags; /* nothing else is enabled; RTX_ACT is read-only and handled above */
		} else {
			break;
		}
	}
}

void USBHS1_IRQHandler(void)
{
	tud_int_handler(0);
}

/*------------------------------------------------------------------------------------------------------------*/
/* Controller */

/* WCH's USBHS_Device_Init order: link reset with the PHY running, interrupts, endpoints, speed, then enable.
 * No LPM_EN (WCH sets it): the BOS has no USB 2.0 extension, so LPM stays off. */
bool dcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init)
{
	(void)rh_init;

	memset(s_xfer, 0, sizeof(s_xfer));
	s_pending_addr = 0;
	USBD->CONTROL = USBHS_UD_RST_LINK | USBHS_UD_PHY_SUSPENDM;
	USBD->INT_EN = USBHS_UDIE_BUS_RST | USBHS_UDIE_SUSPEND | USBHS_UDIE_TRANSFER;
	USBD->DEV_AD = 0;
	USBD->UEP_AF_MODE = 0;
	USBD->UEP_TX_BURST = 0;
	USBD->UEP_RX_BURST = 0;
	USBD->UEP_RX_RES_MODE = 0;
	epn_reset_all();
	ep0_reset();
	USBD->BASE_MODE = USBHS_UD_SPEED_FULL;
	USBD->CONTROL = USBHS_UD_DMA_EN | USBHS_UD_PHY_SUSPENDM;
	dcd_connect(rhport);
	return true;
}

void dcd_int_enable(uint8_t rhport)
{
	(void)rhport;
	NVIC_EnableIRQ(USBHS1_IRQn);
}

void dcd_int_disable(uint8_t rhport)
{
	(void)rhport;
	NVIC_DisableIRQ(USBHS1_IRQn);
}

/* the status ZLP goes now, the address once it's acknowledged (handle_in) */
void dcd_set_address(uint8_t rhport, uint8_t dev_addr)
{
	s_pending_addr = dev_addr & USBHS_UD_DEV_ADDR;
	dcd_edpt_xfer(rhport, TUSB_DIR_IN_MASK, NULL, 0, false);
}

void dcd_remote_wakeup(uint8_t rhport)
{
	(void)rhport;
	USBD->WAKE_CTRL = USBHS_UD_REMOTE_WKUP; /* self-clearing */
}

/* RB_UD_DEV_EN switches the D+ pull-up (WCH usbhs1_dcp.c _open/_close; the RM names no separate bit) */
void dcd_connect(uint8_t rhport)
{
	(void)rhport;
	USBD->CONTROL |= USBHS_UD_DEV_EN;
}

void dcd_disconnect(uint8_t rhport)
{
	(void)rhport;
	USBD->CONTROL &= (uint8_t)~USBHS_UD_DEV_EN;
}

void dcd_sof_enable(uint8_t rhport, bool en)
{
	(void)rhport;
	if (en)
		USBD->INT_EN |= USBHS_UDIE_SOF_ACT;
	else
		USBD->INT_EN &= (uint8_t)~USBHS_UDIE_SOF_ACT;
}

/*------------------------------------------------------------------------------------------------------------*/
/* Endpoints */

bool dcd_edpt_open(uint8_t rhport, const tusb_desc_endpoint_t *desc_ep)
{
	(void)rhport;
	uint8_t n = tu_edpt_number(desc_ep->bEndpointAddress);
	tusb_dir_t dir = tu_edpt_dir(desc_ep->bEndpointAddress);
	uint16_t mps = tu_edpt_packet_size(desc_ep);
	uint16_t bit = (uint16_t)(1u << n);

	TU_ASSERT(n < EP_MAX);
	if (n == 0)
		return true;
	TU_ASSERT(desc_ep->bmAttributes.xfer != TUSB_XFER_ISOCHRONOUS); /* not needed, not done */
	TU_ASSERT(mps && (mps <= EP_MPS));

	s_xfer[n][dir] = (xfer_t){ .mps = mps };
	/* one MAX_LEN per endpoint number: the larger of its two directions */
	EP_MAX_LEN(n) = TU_MAX(s_xfer[n][TUSB_DIR_IN].mps, s_xfer[n][TUSB_DIR_OUT].mps);
	if (dir == TUSB_DIR_IN) {
		EP_TX_LEN(n) = 0;
		EP_TX_DMA(n) = (uint32_t)(uintptr_t)s_bounce[n][TUSB_DIR_IN];
		tx_res(n, USBHS_UEP_T_RES_NAK);
		reset_toggle(n, dir);
		USBD->UEP_TX_EN |= bit;
	} else {
		EP_RX_DMA(n) = (uint32_t)(uintptr_t)s_bounce[n][TUSB_DIR_OUT];
		rx_res(n, USBHS_UEP_R_RES_NAK);
		reset_toggle(n, dir);
		USBD->UEP_RX_EN |= bit;
	}
	return true;
}

void dcd_edpt_close(uint8_t rhport, uint8_t ep_addr)
{
	(void)rhport;
	uint8_t n = tu_edpt_number(ep_addr);
	tusb_dir_t dir = tu_edpt_dir(ep_addr);
	uint16_t bit = (uint16_t)(1u << n);

	if (!n || (n >= EP_MAX))
		return;
	s_xfer[n][dir] = (xfer_t){ 0 };
	if (dir == TUSB_DIR_IN) {
		USBD->UEP_TX_EN &= (uint16_t)~bit;
		USBD->UEP_TX_TOG_AUTO &= (uint16_t)~bit;
		EP_TX_CTRL(n) = USBHS_UEP_T_RES_NAK;
	} else {
		USBD->UEP_RX_EN &= (uint16_t)~bit;
		USBD->UEP_RX_TOG_AUTO &= (uint16_t)~bit;
		EP_RX_CTRL(n) = USBHS_UEP_R_RES_NAK;
	}
}

void dcd_edpt_close_all(uint8_t rhport)
{
	(void)rhport;
	epn_reset_all();
}

bool dcd_edpt_xfer(uint8_t rhport, uint8_t ep_addr, uint8_t *buffer, uint16_t total_bytes, bool is_isr)
{
	(void)rhport;
	(void)is_isr;
	uint8_t n = tu_edpt_number(ep_addr);
	tusb_dir_t dir = tu_edpt_dir(ep_addr);
	xfer_t *x = &s_xfer[n][dir];

	TU_ASSERT(n < EP_MAX);
	x->buf = buffer;
	x->total = total_bytes;
	x->done = 0;
	x->active = true;
	if (n == 0) {
		if (dir == TUSB_DIR_IN)
			ep0_arm_in();
		else
			ep0_arm_out();
	} else {
		if (dir == TUSB_DIR_IN)
			epn_arm_in(n);
		else
			epn_arm_out(n);
	}
	return true;
}

void dcd_edpt_stall(uint8_t rhport, uint8_t ep_addr)
{
	(void)rhport;
	uint8_t n = tu_edpt_number(ep_addr);

	if (tu_edpt_dir(ep_addr) == TUSB_DIR_IN)
		tx_res(n, USBHS_UEP_T_RES_STALL);
	else
		rx_res(n, USBHS_UEP_R_RES_STALL);
}

/* DATA0 again; a transfer queued while halted is re-armed */
void dcd_edpt_clear_stall(uint8_t rhport, uint8_t ep_addr)
{
	(void)rhport;
	uint8_t n = tu_edpt_number(ep_addr);
	tusb_dir_t dir = tu_edpt_dir(ep_addr);

	if (!n)
		return; /* EP0's stall ends at the next SETUP */
	if (dir == TUSB_DIR_IN) {
		tx_res(n, USBHS_UEP_T_RES_NAK);
		reset_toggle(n, dir);
		if (s_xfer[n][dir].active)
			epn_arm_in(n);
	} else {
		rx_res(n, USBHS_UEP_R_RES_NAK);
		reset_toggle(n, dir);
		if (s_xfer[n][dir].active)
			epn_arm_out(n);
	}
}

/*------------------------------------------------------------------------------------------------------------*/
/* Board */

/* WCH's USBHS_RCC_Init: USBHS PLL from the 25 MHz HSE (it clocks the PHY at full speed too), UTMI1 clock, module
 * clock. Plus a module reset for clean registers. PA11/PA12 stay floating inputs; the PHY takes them over. */
void board_usb_connect(void)
{
	RCC->HBPCENR &= ~RCC_USBHS1EN;
	if (!(RCC->CTLR & RCC_USBHSPLLRDY)) {
		RCC->CTLR &= ~RCC_USBHSPLLON; /* source and reference only take a write with the PLL off */
		RCC->CFGR2 = (RCC->CFGR2 & ~(RCC_USBHSPLLSRC | RCC_USBHSPLLCLK)) | RCC_USBHSPLLSRC_HSE | RCC_USBHSPLLCLK_25M;
		RCC->CTLR |= RCC_USBHSPLLON;
		/* bounded (tens of ms, no SysTick in the bootloader): no lock leaves USB off, Ethernet still runs */
		for (uint32_t n = 2000000; !(RCC->CTLR & RCC_USBHSPLLRDY); n--) {
			if (!n)
				return;
		}
	}
	RCC->CFGR2 |= RCC_UTMI1_ON;

	RCC->PB2PCENR |= RCC_IOPAEN;
	GPIOA->CFGHR = (GPIOA->CFGHR & ~((0xFu << 12) | (0xFu << 16))) | (0x4u << 12) | (0x4u << 16);

	RCC->HBRSTR |= RCC_USBHS1RST;
	RCC->HBRSTR &= ~RCC_USBHS1RST;
	RCC->HBPCENR |= RCC_USBHS1EN;
}

/* pull-up off first so the host sees the detach, then the controller, its clocks and the PLL (WCH IAP's order) */
void board_usb_disconnect(void)
{
	NVIC_DisableIRQ(USBHS1_IRQn);
	if (RCC->HBPCENR & RCC_USBHS1EN)
		USBD->CONTROL = USBHS_UD_RST_SIE | USBHS_UD_RST_LINK;
	RCC->HBRSTR |= RCC_USBHS1RST;
	RCC->HBRSTR &= ~RCC_USBHS1RST;
	RCC->HBPCENR &= ~RCC_USBHS1EN;
	RCC->CFGR2 &= ~RCC_UTMI1_ON;
	RCC->CTLR &= ~RCC_USBHSPLLON;
}
