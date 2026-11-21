/* Forced into dapboot's build: replaces its deprecated 8 MHz clock call with the current API and this board's crystal. */
#pragma once
#include <libopencm3/stm32/rcc.h>
#define rcc_clock_setup_in_hse_8mhz_out_72mhz() rcc_clock_setup_pll(&rcc_hse_configs[DAPBOOT_HSE_CONFIG])
