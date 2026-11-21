/* STM32F103C8 application behind the 8 KB dapboot bootloader.
 * The last two 1 KB pages hold the settings log (settings.c). */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 54K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_settings_start = 0x0800F800;

INCLUDE stm32f103x8_common.ld
