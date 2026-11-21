/* LANGPIB-WCH DFU bootloader: sectors 0-3 of flash. RAM stays below the app's .noinit (0x2001FF00), so the app's
 * trap record survives a pass through here. */
ENTRY(_start)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 16K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

__stack_size = 2K;

SECTIONS
{
  .text : ALIGN(4)
  {
    KEEP(*(.init))
    . = ALIGN(1024);            /* mtvec base must be 1 KB aligned (RM 9.2.3.6) */
    KEEP(*(.trap))
    *(.text .text.*)
    *(.rodata .rodata.* .srodata .srodata.*)
    . = ALIGN(4);
  } > FLASH

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
    *(.sbss .sbss.* .bss .bss.* COMMON .noinit .noinit.*)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  .stack (NOLOAD) : ALIGN(16)
  {
    . += __stack_size;
    __stack_top = .;
    PROVIDE(end = .);
  } > RAM
}
