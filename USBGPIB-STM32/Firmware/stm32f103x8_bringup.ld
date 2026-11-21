/* Bring-up image (bringup/): runs from 0x08000000 without dapboot. Same settings pages as the app. */

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 62K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_settings_start = 0x0800F800;

INCLUDE stm32f103x8_common.ld
