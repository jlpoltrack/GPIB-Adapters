/* The one trap entry (mtvec mode 0): interrupts dispatch by number, exceptions park for the watchdog */

#include "board.h"
#include "trap.h"

trap_record_t trap_last __attribute__((section(".noinit")));

/* The bootloader runs no watchdog, so its faults reset at once; the app's board.c overrides this */
bool board_wdg_ok(void);
__attribute__((weak)) bool board_wdg_ok(void) { return false; }

void SysTick_Handler(void);
void USBHS1_IRQHandler(void);

/* the fault is kept for the next boot (main logs it); the watchdog resets in a few seconds, else a direct reset */
static void fault(uint32_t cause)
{
	uint32_t epc, tval;

	__asm volatile("csrr %0, mepc" : "=r"(epc));
	__asm volatile("csrr %0, mtval" : "=r"(tval));
	trap_last = (trap_record_t){ .magic = TRAP_MAGIC, .cause = cause, .epc = epc, .tval = tval };
	if (!board_wdg_ok()) /* no LSI, no watchdog: parking would hang for good */
		NVIC_SystemReset();
	for (;;) {
	}
}

__attribute__((interrupt, section(".trap"), aligned(4))) void trap_entry(void)
{
	uint32_t cause;

	__asm volatile("csrr %0, mcause" : "=r"(cause));
	if (!(cause & 0x80000000u))
		fault(cause);

	switch (cause & 0xFFFu) {
	case SysTick_IRQn:
		SysTick_Handler();
		break;
	case USBHS1_IRQn:
		USBHS1_IRQHandler();
		break;
	default: /* enabled by mistake: masking it keeps it from storming */
		NVIC_DisableIRQ((IRQn_Type)(cause & 0xFFFu));
		break;
	}
}
