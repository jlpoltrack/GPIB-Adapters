/* LANGPIB-WCH main loop: instrument start-up, then USB (USBTMC + Prologix CDC), Ethernet (VXI-11) and GPIB from one
 * loop. No RTOS; USB and SysTick are the only interrupts. */

#include "board.h"
#include "settings.h"
#include "instr.h"
#include "gpib_bus.h"
#include "usbtmc.h"
#include "prologix.h"
#include "trace.h"
#include "tusb.h"
#include "net.h"
#include "eth.h"
#include "trap.h"
#include "pintest.h"
#include "update.h"
#include <string.h>
#ifdef GPIB_SIM
#include "gpib_sim.h"
#include "sim_scpi.h"
#endif

extern const uint8_t boot_image[], boot_image_end[];
extern void _start(void);

#define BOOT_BASE   0x08000000u
#define BOOT_SECTOR 4096u

static volatile bool s_dfu_detach;
static uint32_t s_dfu_detach_at;

/* DFU_DETACH on the DFU runtime interface; the reset waits until the request's status stage is out */
void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
	s_dfu_detach_at = board_millis() + 50;
	s_dfu_detach = true;
}

/* during instrument start-up: the network keeps up (DHCP, ARP), VXI-11 isn't listening yet */
/* main-loop passes/s, the longest pass and the Ethernet counters, on the UART every minute (headroom: loaded vs idle) */
static void loop_stats(uint32_t pass_start)
{
	static uint32_t passes, window_start, longest;
	uint32_t now = board_millis();

	passes++;
	if (now - pass_start > longest)
		longest = now - pass_start;
	if (now - window_start >= 60000u) {
		const eth_stats_t *e = eth_stats();

		board_log("loop: %lu passes/s, longest %lu ms; eth rx %lu err %lu missed %lu, tx %lu err %lu full %lu\n",
		          (unsigned long)(passes / ((now - window_start) / 1000u)), (unsigned long)longest,
		          (unsigned long)e->rx, (unsigned long)e->rx_err, (unsigned long)e->rx_missed, (unsigned long)e->tx,
		          (unsigned long)e->tx_err, (unsigned long)e->tx_full);
		passes = longest = 0;
		window_start = now;
	}
}

static void idle(void)
{
	board_watchdog_feed();
	net_poll();
}

/* Rewrites the bootloader's sectors from the embedded copy when they differ. Sector 0 (its _start) goes last: only a
 * power loss in that one erase and write leaves a board that needs the SDI probe. */
static void bootloader_update(void)
{
	const uint8_t *flash = (const uint8_t *)BOOT_BASE;
	unsigned len = (unsigned)(boot_image_end - boot_image);

	if ((uintptr_t)_start < BOOT_BASE + 4u * BOOT_SECTOR) /* a standalone build lives there itself */
		return;
	if (memcmp(flash, boot_image, len) == 0)
		return;
	board_log("bootloader: updating %u bytes\n", len);
	for (unsigned off = (len - 1u) & ~(BOOT_SECTOR - 1u);; off -= BOOT_SECTOR) {
		unsigned n = ((len - off) < BOOT_SECTOR) ? (len - off) : BOOT_SECTOR;
		board_watchdog_feed();
		board_flash_erase(flash + off);
		board_flash_write(flash + off, boot_image + off, n);
		if (off == 0)
			break;
	}
	board_log("bootloader: %s\n", (memcmp(flash, boot_image, len) == 0) ? "updated" : "VERIFY FAILED");
}

static void log_boot(void)
{
	char uid[BOARD_SERIAL_LEN + 1];

	board_get_uid_hex(uid);
	board_log("\nLANGPIB-WCH: reset 0x%08lx, HSE %s, %lu MHz, UID %s, ESIG flash %u KB, RNG %s, watchdog %s\n",
	          (unsigned long)board_reset_cause(), board_hse_ok() ? "ok" : "FAILED",
	          (unsigned long)(SystemCoreClock / 1000000u), uid, *(volatile uint16_t *)0x1FFFF7E0u,
	          board_rng_ok() ? "ok" : "none", board_wdg_ok() ? "on" : "OFF (no LSI)");
	board_log("chip ID 0x%08lx, FEATURE_SIGN 0x%08lx\n", (unsigned long)*(volatile uint32_t *)0x1FFFF704u,
	          (unsigned long)*(volatile uint32_t *)0x1FFFF7D0u); /* WCH remaps the PHY LEDs only on IDs x2/x5 */
	if (trap_last.magic == TRAP_MAGIC) {
		board_log("last trap: mcause 0x%08lx mepc 0x%08lx mtval 0x%08lx\n", (unsigned long)trap_last.cause,
		          (unsigned long)trap_last.epc, (unsigned long)trap_last.tval);
		trap_last.magic = 0;
	}
}

int main(void)
{
	board_init();
	log_boot();
	bootloader_update();
	settings_init();
	trace_init();
	gpib_pintest();
#ifdef GPIB_SIM /* SIM=1: a simulated SCPI instrument at GPIB 5 instead of the plug */
	sim_reset();
	sim_add(5, -1)->on_message = sim_scpi;
	board_log("GPIB: SIMULATED instrument at 5, GPIB pins unused\n");
#endif
	instr_init();
	gpib_init();

	/* no 25 MHz crystal: neither USB nor Ethernet can run, so say so on the LED and stay here */
	if (!board_hse_ok()) {
		board_led_mode(LED_NO_CLOCK);
		for (;;)
			board_watchdog_feed();
	}

	net_init();
	board_log("PHY id 0x%08lx\n", (unsigned long)net_phy_id());

	if (!instr_startup(idle)) { /* bus unplugged while searching: start over */
		board_delay_ms(500);
		board_reset();
	}

	net_start_services();
	board_usb_connect();
	if (!(RCC->HBPCENR & RCC_USBHS1EN))
		board_log("USB: PLL didn't lock, USB off\n");
	tusb_rhport_init_t const dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
	tusb_init(0, &dev_init);

	for (;;) {
		uint32_t pass_start = board_millis();

		board_watchdog_feed();
		tud_task();
		usbtmc_task();
		prologix_task();
		gpib_task();
		trace_task();
		settings_task();
		net_task();
		update_task();
		loop_stats(pass_start);

		if (s_dfu_detach && ((int32_t)(board_millis() - s_dfu_detach_at) >= 0))
			board_enter_bootloader();

		if (!gpib_connected()) { /* unplugged: detach from USB and reset; it reconnects once plugged in again */
			board_led_mode(LED_OFF);
			board_usb_disconnect();
			board_delay_ms(500);
			board_reset();
		}
	}
}

uint32_t tusb_time_millis_api(void)
{
	return board_millis();
}
