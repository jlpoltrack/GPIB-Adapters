/* LANGPIB-WCH application. APP_ORIGIN (Makefile): 0x08004100 behind the DFU bootloader and its 256-byte header
 * (bootloader/image.h), or 0x08000000 standalone. Code stays in the zero-wait first 512 KB; settings take its last 8 KB. */
ENTRY(_start)

MEMORY
{
  FLASH    (rx)  : ORIGIN = APP_ORIGIN, LENGTH = 0x0807E000 - APP_ORIGIN
  SETTINGS (r)   : ORIGIN = 0x0807E000, LENGTH = 8K   /* two 4 KB erase sectors */
  RAM      (rwx) : ORIGIN = 0x20000000, LENGTH = 128K - 256 /* fits either RAM_CODE_MOD setting */
  NOINIT   (rw)  : ORIGIN = 0x2001FF00, LENGTH = 256        /* the bootloader's RAM stays below this */
}

__stack_size = 8K; /* lwIP mDNS probe tie-breaking nests ~3.3 KB of frames (-fstack-usage), plus net path and USB IRQ */

SECTIONS
{
  .text : ALIGN(4)
  {
    KEEP(*(.init))              /* _start first */
    . = ALIGN(1024);            /* mtvec base must be 1 KB aligned (RM 9.2.3.6) */
    KEEP(*(.trap))
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
    . = ALIGN(4);
  } > FLASH

  .settings (NOLOAD) :
  {
    _settings_start = .;
  } > SETTINGS

  _sidata = LOADADDR(.data);
  .data : ALIGN(4)
  {
    _sdata = .;
    *(.data .data.*)
    . = ALIGN(8);
    PROVIDE(__global_pointer$ = . + 0x800);
    *(.sdata .sdata.*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss (NOLOAD) : ALIGN(4)
  {
    _sbss = .;
    *(.sbss .sbss.* .bss .bss.* COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  /* the network update's stage takes the flash above (bootloader/image.h STAGE_ADDR) */
  ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0x08041000, "the app grew into the network update stage")

  .noinit (NOLOAD) : ALIGN(4)   /* survives a system reset and the bootloader */
  {
    *(.noinit .noinit.*)
    . = ALIGN(4);
  } > NOINIT

  .stack (NOLOAD) : ALIGN(16)   /* no heap: nothing mallocs */
  {
    . += __stack_size;
    __stack_top = .;
    PROVIDE(end = .);           /* nano vsnprintf links malloc (for asprintf), never called here */
  } > RAM
}
