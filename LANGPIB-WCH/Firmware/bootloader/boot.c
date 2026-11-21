/* LANGPIB-WCH DFU bootloader (WCH repo docs/bootloader-design.md). Before touching clocks: SW1 or a DFU request stays
 * in DFU; a staged network update is installed (stage.c); then a valid app starts on untouched hardware, else DFU. */

#include <string.h>
#include "ch32v4x7.h"
#include "board.h"
#include "clock.h"
#include "flash.h"
#include "image.h"
#include "stage.h"
#include "tusb.h"

#define BOOT_REQUEST  0xDF11u /* BKP DATAR1, from the app's board_enter_bootloader() */
#define LED_PIN       8       /* PA8, D1 active low */
#define DFU_PIN       8       /* PD8, SW1 to GND */
#define MANIFEST_RESET_MS 3000u /* dfu-util sleeps 1 s on dfuMANIFEST before its last GETSTATUS: 1 s raced it */
#define DETACH_RESET_MS   50u

void SysTick_Handler(void) {} /* trap.c's dispatch target; nothing enables it here */

static uint32_t s_ms, s_ms_cycles;
static uint32_t s_reset_at;
static bool     s_reset_pending;

/* mcycle counts HCLK: a ms counter for TinyUSB and the timers, updated from the main loop */
static uint32_t cycles(void)
{
	uint32_t c;
	__asm volatile("csrr %0, mcycle" : "=r"(c));
	return c;
}

static void delay_us(uint32_t us)
{
	uint32_t t0 = cycles(), n = us * (SystemCoreClock / 1000000u);
	while ((cycles() - t0) < n) {
	}
}

static void tick(void)
{
	uint32_t per_ms = SystemCoreClock / 1000u, d = (cycles() - s_ms_cycles) / per_ms;
	s_ms += d;
	s_ms_cycles += d * per_ms;
}

uint32_t tusb_time_millis_api(void)
{
	return s_ms;
}

/* BKP DATAR1 survives the app's system reset (not a power cut: VBAT is VDD); read and cleared, clocks put back */
static uint32_t boot_request(void)
{
	uint32_t en = RCC->PB1PCENR;
	RCC->PB1PCENR = en | RCC_PWREN | RCC_BKPEN;
	uint32_t req = BKP->DATAR1;
	if ((req == BOOT_REQUEST) || (req == STAGE_REQUEST)) {
		PWR->CTLR |= PWR_CTLR_DBP;
		BKP->DATAR1 = 0;
		PWR->CTLR &= ~PWR_CTLR_DBP;
	}
	RCC->PB1PCENR = en;
	return req;
}

/* PD8 with its pull-up: low on five reads 1 ms apart. GPIOD is put back as reset left it. */
static bool button_held(void)
{
	uint32_t en = RCC->PB2PCENR, cfg = GPIOD->CFGHR, out = GPIOD->OUTDR;
	unsigned low = 0;

	RCC->PB2PCENR = en | RCC_IOPDEN;
	GPIOD->OUTDR = out | (1u << DFU_PIN);
	GPIOD->CFGHR = (cfg & ~(0xFu << ((DFU_PIN - 8) * 4))) | (0x8u << ((DFU_PIN - 8) * 4));
	for (int i = 0; i < 5; i++) {
		delay_us(1000);
		low += !(GPIOD->INDR & (1u << DFU_PIN));
	}
	GPIOD->CFGHR = cfg;
	GPIOD->OUTDR = out;
	RCC->PB2PCENR = en;
	return low == 5;
}

static void __attribute__((noreturn)) start_app(void)
{
	((void (*)(void))APP_START)(); /* its _start sets sp, gp and mtvec */
	for (;;) {
	}
}

static void __attribute__((noreturn)) system_reset(void)
{
	board_usb_disconnect();
	delay_us(10000); /* the host sees the detach before the app (or this) enumerates again */
	NVIC_SystemReset();
	for (;;) {
	}
}

/*------------------------------------------------------------------------------------------------------------*/
/* DFU: block n is the 4 KB sector at APP_HDR_ADDR + n * 4 KB. The header page is kept in RAM and written last, once
 * the image's CRC checks out, so an interrupted download leaves no valid app and the next boot stays here. */

static uint32_t s_hdr[FLASH_PAGE / 4];
static uint32_t s_page[FLASH_PAGE / 4];
static bool     s_have_hdr;
static uint32_t s_next_block, s_end;

uint32_t tud_dfu_get_timeout_cb(uint8_t alt, uint8_t state)
{
	(void)alt;
	return (state == DFU_DNBUSY) ? 20u : (state == DFU_MANIFEST) ? 100u : 0u;
}

void tud_dfu_download_cb(uint8_t alt, uint16_t block, const uint8_t *data, uint16_t len)
{
	uint32_t addr = image_block_addr(block, len);
	bool ok;

	(void)alt;
	if (block == 0) { /* a new download: also cancels the reset a finished one left pending */
		stage_clear(); /* else a staged network update could later be copied over this download */
		s_reset_pending = false;
		s_have_hdr = false;
		s_next_block = 0;
		s_end = 0;
	}
	if (!addr || (block != s_next_block)) {
		tud_dfu_finish_flashing(DFU_STATUS_ERR_ADDRESS);
		return;
	}

	flash_unlock();
	ok = flash_erase_sector(addr);
	for (uint32_t off = 0; ok && (off < len); off += FLASH_PAGE) {
		uint32_t n = ((len - off) < FLASH_PAGE) ? (len - off) : FLASH_PAGE;
		memset(s_page, 0xFF, sizeof(s_page));
		memcpy(s_page, data + off, n);
		if (addr + off == APP_HDR_ADDR) {
			memcpy(s_hdr, s_page, sizeof(s_hdr));
			s_have_hdr = n == FLASH_PAGE;
		} else {
			ok = flash_program_page(addr + off, s_page);
		}
	}
	flash_lock();

	s_next_block++;
	s_end = addr + len;
	tud_dfu_finish_flashing(ok ? DFU_STATUS_OK : DFU_STATUS_ERR_PROG);
}

void tud_dfu_manifest_cb(uint8_t alt)
{
	const app_header_t *hdr = (const app_header_t *)s_hdr;
	bool ok = s_have_hdr && (s_end > APP_START) && (hdr->length <= s_end - APP_START) &&
	          image_valid(hdr, (const uint8_t *)APP_START);

	(void)alt;
	if (ok) {
		flash_unlock();
		ok = flash_program_page(APP_HDR_ADDR, s_hdr);
		flash_lock();
	}
	s_have_hdr = false;
	tud_dfu_finish_flashing(ok ? DFU_STATUS_OK : DFU_STATUS_ERR_FIRMWARE);
	if (ok) {
		s_reset_at = s_ms + MANIFEST_RESET_MS;
		s_reset_pending = true;
	}
}

void tud_dfu_detach_cb(void)
{
	s_reset_at = s_ms + DETACH_RESET_MS;
	s_reset_pending = true;
}

/*------------------------------------------------------------------------------------------------------------*/

static void __attribute__((noreturn)) dfu_mode(void)
{
	bool hse = clock_init();

	RCC->PB2PCENR |= RCC_IOPAEN;
	GPIOA->CFGHR = (GPIOA->CFGHR & ~(0xFu << ((LED_PIN - 8) * 4))) | (0x1u << ((LED_PIN - 8) * 4));
	s_ms_cycles = cycles();

	/* USB needs the crystal: without it, blink fast and wait for a power cycle */
	if (hse) {
		board_usb_connect();
		tusb_rhport_init_t const dev_init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
		tusb_init(0, &dev_init);
	}

	for (;;) {
		tick();
		if (hse) {
			tud_int_handler(0); /* polled: MIE stays off in the bootloader */
			tud_task();
		}
		/* double blink each second: no app pattern looks like it (theirs are single, 2 Hz or slower); no crystal: 10 Hz */
		uint32_t ph = s_ms % 1000u;
		bool on = hse ? ((ph < 100u) || ((ph >= 250u) && (ph < 350u))) : ((s_ms % 100u) < 50u);
		GPIOA->BSHR = on ? (1u << (LED_PIN + 16)) : (1u << LED_PIN);

		if (s_reset_pending && ((int32_t)(s_ms - s_reset_at) >= 0))
			system_reset();
	}
}

int main(void)
{
	uint32_t req = boot_request();
	bool stay = (req == BOOT_REQUEST);
	stay |= button_held();
	if (!stay && stage_boot(req == STAGE_REQUEST))
		start_app();
	dfu_mode();
}
