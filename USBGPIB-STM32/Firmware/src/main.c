/* USBGPIB-STM32 main loop: instrument start-up, then USB (USBTMC + Prologix CDC) and GPIB from one loop */

#include "board.h"
#include "settings.h"
#include "instr.h"
#include "gpib_bus.h"
#include "usbtmc.h"
#include "prologix.h"
#include "trace.h"
#include "tusb.h"
#include <string.h>

extern const uint8_t dapboot_image[], dapboot_image_end[]; /* src/dapboot_image.c */

static volatile bool s_dfu_detach;
static uint32_t s_dfu_detach_at;

/* DFU_DETACH on the DFU runtime interface; the jump waits until the request's status stage is out */
void tud_dfu_runtime_reboot_to_dfu_cb(void)
{
	s_dfu_detach_at = board_millis() + 50;
	s_dfu_detach = true;
}

/* the button, or a DFU_DETACH: into dapboot */
static void check_bootloader_entry(void)
{
	board_watchdog_feed();
	if (BTN_PRESSED || (s_dfu_detach && ((int32_t)(board_millis() - s_dfu_detach_at) >= 0)))
		board_enter_bootloader();
}

/* Rewrites the bootloader pages from the embedded dapboot.bin when they differ, so a DFU update of the app carries a
 * new dapboot along. Page 0 (its vector table) goes last: only a power loss in that one erase and write needs SWD. */
static void bootloader_update(void)
{
	const uint8_t *flash = (const uint8_t *)FLASH_BASE;
	unsigned len = dapboot_image_end - dapboot_image;

	if (memcmp(flash, dapboot_image, len) == 0)
		return;
	board_watchdog_feed();
	for (unsigned off = (len - 1) & ~(BOARD_FLASH_PAGE_SIZE - 1);; off -= BOARD_FLASH_PAGE_SIZE) {
		unsigned n = len - off < BOARD_FLASH_PAGE_SIZE ? len - off : BOARD_FLASH_PAGE_SIZE;
		board_flash_erase(flash + off);
		board_flash_write(flash + off, dapboot_image + off, n);
		if (off == 0)
			break;
	}
	board_watchdog_feed();
}

int main(void)
{
	board_init();
	bootloader_update();
	settings_init();
	trace_init();
	instr_init();
	gpib_init();

	/* no 16 MHz crystal: USB can't run on the HSI, so say so on the LED and stay here */
	if (!board_hse_ok()) {
		board_led_mode(LED_NO_CLOCK);
		for (;;)
			check_bootloader_entry();
	}

#ifdef BENCH_NO_BUS /* make BENCH=1: USB comes up with no instrument, for a board without J2 */
	board_led_mode(LED_ON);
#else
	if (!instr_startup(check_bootloader_entry)) { /* bus unplugged while searching: start over */
		board_delay_ms(500);
		board_reset();
	}
#endif

	board_usb_connect();
	tusb_rhport_init_t const dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
	tusb_init(0, &dev_init);

	for (;;) {
		board_watchdog_feed();
		tud_task();
		usbtmc_task();
		prologix_task();
		gpib_task();
		trace_task();
		check_bootloader_entry();

#ifndef BENCH_NO_BUS
		if (!gpib_connected()) { /* unplugged: detach from USB and reset; it reconnects once plugged in again */
			board_led_mode(LED_OFF);
			board_usb_disconnect();
			board_delay_ms(500);
			board_reset();
		}
#endif

		settings_task();
	}
}

uint32_t tusb_time_millis_api(void)
{
	return board_millis();
}

void USB_HP_CAN1_TX_IRQHandler(void)
{
	tud_int_handler(0);
}

void USB_LP_CAN1_RX0_IRQHandler(void)
{
	tud_int_handler(0);
}
