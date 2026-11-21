#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

/* DFU bootloader: DFU mode only, polled (no interrupts), through the app's USBHS driver (../src/dcd_ch32v407.c) */
#define CFG_TUSB_MCU              OPT_MCU_NONE
#define CFG_TUSB_OS               OPT_OS_NONE
#define TUP_DCD_ENDPOINT_MAX      8
#define CFG_TUSB_DEBUG            0

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))

#define CFG_TUD_DFU               1
#define CFG_TUD_DFU_XFER_BUFSIZE  4096 /* one flash sector per block (image.h DFU_BLOCK) */

#endif /* TUSB_CONFIG_H_ */
