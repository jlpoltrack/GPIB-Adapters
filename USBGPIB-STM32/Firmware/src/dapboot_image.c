/* dapboot.bin as built with this app, in rodata: main.c rewrites the bootloader pages from it when they differ */
#include <stdint.h>

__asm__(".section .rodata.dapboot_image, \"a\"\n"
        ".balign 4\n"
        ".global dapboot_image, dapboot_image_end\n"
        "dapboot_image:\n"
        ".incbin \"" DAPBOOT_BIN "\"\n"
        ".balign 2\n" /* flash is written in half-words; a pad byte is 0 and compares equal after the write */
        "dapboot_image_end:\n");
