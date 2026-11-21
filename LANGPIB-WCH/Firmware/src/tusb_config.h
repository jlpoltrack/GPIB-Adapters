#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

/* TinyUSB has no port for the V407's USBHS (its WCH driver is the V307's, a different register map): dcd_ch32v407.c
 * is ours. USBHS1 runs as a full-speed device. */
#define CFG_TUSB_MCU              OPT_MCU_NONE
#define CFG_TUSB_OS               OPT_OS_NONE
#define TUP_DCD_ENDPOINT_MAX      8 /* EP0-EP7 (RM 22) */
#define CFG_TUSB_DEBUG            0

#define CFG_TUD_ENABLED           1
#define CFG_TUD_MAX_SPEED         OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE    64

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))

#define CFG_TUD_USBTMC                0 /* custom class driver: gpib-core usbtmc.c */
#define CFG_TUD_CDC                   1
#define CFG_TUD_DFU_RUNTIME           1

#define CFG_TUD_CDC_RX_BUFSIZE        256
#define CFG_TUD_CDC_TX_BUFSIZE        256
#define CFG_TUD_CDC_EP_BUFSIZE        64

#endif /* TUSB_CONFIG_H_ */
