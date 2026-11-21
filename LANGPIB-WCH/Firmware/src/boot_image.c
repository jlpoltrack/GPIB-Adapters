/* The DFU bootloader (bootloader/build/boot.bin) as built with this app, in rodata: main.c rewrites sectors 0-3 from
 * it when they differ, so a DFU update of the app carries a new bootloader along (USBGPIB-STM32's dapboot trick). */

__asm__(".section .rodata.boot_image, \"a\"\n"
        ".balign 4\n"
        ".global boot_image, boot_image_end\n"
        "boot_image:\n"
        ".incbin \"" BOOT_BIN "\"\n"
        ".balign 4, 0\n" /* board_flash_write() takes whole words; the pad bytes are written and compare equal */
        "boot_image_end:\n");
