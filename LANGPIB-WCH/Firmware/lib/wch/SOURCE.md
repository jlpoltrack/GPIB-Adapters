`ch32v4x7.h`, `core_riscv.h`, `ch32v4x7_usb.h` and `ch32v4x7_eth.h` are unmodified copies from WCH's official repo
https://github.com/openwch/ch32v407_ch32v467 (commit 17394a1, `EVT/EXAM/SRC`): register definitions only, none of
WCH's peripheral library. `ch32v4x7_conf.h` and `system_ch32v4x7.h` are ours, standing in for WCH's.
WCH notice: "This software (modified or not) and binary are used for microcontroller manufactured by Nanjing Qinheng
Microelectronics."

Known header bug: `RCC_UTMI1ON` / `RCC_UTMI2ON` are swapped (RM 3.4.13 and WCH's `RCC_UTMI1cmd()`: UTMI1 is CFGR2 bit 30);
`src/dcd_ch32v407.c` defines its own.
