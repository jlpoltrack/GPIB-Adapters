# Hardware

Based on xyphro's UsbGpib REV2 (ATmega32U4) with the MCU swapped for an STM32F103C8T6. Schematic: `Hardware/USBGPIB-STM32.pdf`.
The GPIB lines are defined in `Firmware/src/gpib_pins.h`.

## Block diagram

```
USB-C J1 ── VBUS (+5V) ─► XC6206 LDO (U2) ─► 3V3 ─► MCU, LED
  │ D+/D− ─► ESD (U3) ─► 22 Ω (R6/R7) ─► PA12/PA11, fixed 1K5 (R8) from PA12 to 3V3
  │ CC1/CC2 ─► 5K1 each to GND (R1/R2)
STM32F103C8T6 (U1) ◄── 16 MHz crystal (Y1)
  ├─ DIO1-8 on PB3/4/6-11, control on PB12-15, PA8-10, PA15 ──► GPIB connector J2 (direct, open-drain, no drivers)
  ├─ PC13 ► status LED D2 (active low)   ├─ PA1 ◄ button SW1 to GND
  ├─ PA13/PA14, NRST ─► SWD header J4 (GND / SWCLK / SWDIO / NRST)
  └─ PA2/PA3 ─► UART2 header J3 (GND / TX / RX)
```

## Pinout (LQFP-48)

| Pin | STM32 name | Net / function | GPIB pin | Notes |
|---|---|---|---|---|
| 1 | VBAT | 3V3 | | 100 nF (C11); must not float (BKP->DR1 DFU handoff) |
| 2 | PC13 | LED | | active low: 3V3 → 330 Ω (R5) → green LED (D2) → pin |
| 3 | PC14 | free | | OSC32_IN, leave NC |
| 4 | PC15 | free | | OSC32_OUT, leave NC |
| 5 | PD0 / OSC_IN | OSC_IN | | 16 MHz crystal |
| 6 | PD1 / OSC_OUT | OSC_OUT | | 16 MHz crystal |
| 7 | NRST | NRST | | 100 nF (C5) to GND; J4 pin 4 |
| 8 | VSSA | GND | | |
| 9 | VDDA | 3V3 | | 100 nF + 1 µF (C12, C13) |
| 10 | PA0 | free | | not FT |
| 11 | PA1 | BUTTON | | SW1 to GND, internal pull-up (app and dapboot) |
| 12 | PA2 | UART2_TX | | J3 pin 2; not FT, 3.3 V only |
| 13 | PA3 | UART2_RX | | J3 pin 3; not FT, 3.3 V only |
| 14–17 | PA4–PA7 | free | | not FT |
| 18 | PB0 | free | | not FT; candidate for a 2nd LED |
| 19 | PB1 | free | | not FT |
| 20 | PB2 / BOOT1 | BOOT1 | | 5K1 (R4) to GND |
| 21 | PB10 | DIO3 | 3 | FT |
| 22 | PB11 | DIO4 | 4 | FT |
| 23 | VSS | GND | | |
| 24 | VDD | 3V3 | | 100 nF (C8) |
| 25 | PB12 | EOI | 5 | FT |
| 26 | PB13 | DAV | 6 | FT |
| 27 | PB14 | NRFD | 7 | FT |
| 28 | PB15 | NDAC | 8 | FT |
| 29 | PA8 | IFC | 9 | FT |
| 30 | PA9 | SRQ | 10 | FT, input only; also USART1_TX of the ROM bootloader |
| 31 | PA10 | ATN | 11 | FT; also USART1_RX of the ROM bootloader |
| 32 | PA11 | USB_DM | | 22 Ω (R7) to the connector |
| 33 | PA12 | USB_DP | | 22 Ω (R6) to the connector; 1K5 (R8) to 3V3 |
| 34 | PA13 | SWDIO | | J4 pin 3 |
| 35 | VSS | GND | | |
| 36 | VDD | 3V3 | | 100 nF (C9) |
| 37 | PA14 | SWCLK | | J4 pin 2 |
| 38 | PA15 | REN | 17 | FT; JTDI until firmware disables JTAG |
| 39 | PB3 | DIO8 | 16 | FT; JTDO/SWO, so no SWO trace |
| 40 | PB4 | DIO7 | 15 | FT; NJTRST until firmware disables JTAG |
| 41 | PB5 | free | | not FT |
| 42 | PB6 | DIO6 | 14 | FT |
| 43 | PB7 | DIO2 | 2 | FT |
| 44 | BOOT0 | BOOT0 | | 5K1 (R3) to GND |
| 45 | PB8 | DIO5 | 13 | FT |
| 46 | PB9 | DIO1 | 1 | FT |
| 47 | VSS | GND | | |
| 48 | VDD | 3V3 | | 100 nF (C10) |

GPIB connector pins 12 (shield) and 18–24 go to GND. J2 pads are numbered as IEEE-488: 1–12 on one row, 13–24 on the other. Pin 25 (SHELL) is the two mounting holes: plated GND pads with a 1 mm exposed ring on both sides, tied solid to the pours, so the connector's feet ground its shell.
Pin numbers checked against DS5319 (STM32F103x8/B) Table 5.

FT pins take VDD + 4 V (4 V with the board unpowered), but only with the pin's internal pull off. Every FT pin is in use except PB2 (BOOT1), so a new 5 V-facing signal needs a non-FT pin plus protection.

## Firmware-imposed requirements

1. **D+ pull-up is fixed, not switched.** The F103 has no internal pull-up. `board.c` (and dapboot) keep USB detached by driving
   PA12 low over the permanent 1K5 (R8, ≈2.2 mA while held). R8 sits on the **MCU side** of the 22 Ω series resistor.
2. **PC13 can only sink about 3 mA.** R5 = 330 Ω with the green KT-0805G (Vf 2.6–3.1 V) gives about 1–2 mA.
   Check on a board: the drop across R5 must stay under 0.99 V (3 mA, DS5319 pin table note 5). It looks very bright at that current.
   A blue or white LED would be barely lit at 3.3 V.
3. **No pull-ups and no series resistors on the 16 GPIB lines.** A series R raises VOL when sinking the instruments'
   terminations. All 16 lines are on FT (5 V-tolerant) pins as mapped. The one exception is **R9, 100k from ATN to GND**
   (REV2's R5): with no instrument on the bus ATN reads low, which is how `phy_connected()` sees a disconnect.
4. **VBAT (pin 1) goes to 3V3.** The `!reset` → DFU handoff uses BKP->DR1, which survives a system reset while VDD is up. No coin cell.
5. **BOOT0 and PB2 (BOOT1) pulled low with 5K1.** BOOT0 = 0 always boots from flash. BOOT1 only matters when BOOT0 = 1,
   but PB2 must not float.
6. **dapboot uses the same LED and button.** `make bootloader` builds dapboot's generic `STM32F103` target with the LED on PC13
   (open-drain) and the DFU button on PA1 (active low, internal pull-up). Hold SW1 while plugging in to stay in DFU.
7. JTAG-default pins PA15/PB3/PB4 (REN/DIO8/DIO7) have weak internal pulls until firmware disables JTAG.
   They are harmless, but don't add strong external pulls to them.
8. **GPIB pin map follows the layout**, not port order: every line escapes U1 without crossings (bottom/right pins on top copper,
   top pins through vias under U1 on bottom copper). DIO1-8 are all on GPIOB (PB9, PB7, PB10, PB11, PB8, PB6, PB4, PB3), so a
   byte is still one IDR read / one BSRR write plus a few shifts (`gpib_pins.h`).

## Assembly

D2 (C2297 on KiCad's stock `LED_0805_2012Metric`) was assembled reversed on the first prototype and lit once turned round.
The design now carries `FT Rotation Offset = 180`, so `positions.csv` has it at 0, and the cathode goes on the pad nearer U1.

## Design review

1. **SWD is the only way in at first.** A blank chip has no dapboot, so `make flash-all` over J4 is the first step.
   J4 is GND / SWCLK / SWDIO / NRST: the probe and the board each need their own power. NRST makes
   connect-under-reset possible, so firmware that disturbs SWD can always be erased (`pyocd erase --chip -M under-reset`).
2. **The ROM bootloader is not a practical recovery path.** The F103 system bootloader only speaks USART1 on PA9/PA10,
   which are SRQ/ATN (GPIB pins 10/11). SWD is the real fallback, so BOOT0 has only its pull-down.
3. **Drive strength limits the bus size.** The STM32 sinks 8 mA per pin at VOL ≤ 0.4 V; the ATmega32U4 sank 20 mA.
   Each instrument terminates every line with 3 kΩ to 5 V and 6.2 kΩ to GND, which is about 1.5 mA at 0.4 V.
   That allows about 5 instruments on the bus (REV2 handled about 13). The total for 16 lines at 8 mA is 128 mA,
   inside the chip's 150 mA ΣIVSS limit. Fine for the usual one or two instruments.
4. **Unpowered adapter on a powered bus is safe.** Instrument terminations idle the lines at ≈3.4 V. FT pins tolerate VDD + 4 V
   (4 V with VDD = 0) and have no clamp to VDD, so nothing back-powers the board.
5. **LDO: XC6206P332MR-G** (U2, LCSC C5446). 200 mA with ≈0.25 V dropout, stable with ceramic caps. Its input is VBUS
   directly (≈5 V; no series diode). Load on 3V3: MCU at 72 MHz with USB ≤ 50 mA, plus the LED and the D+ hold ≈ 5 mA.
   GPIB sink current returns to the instruments' 5 V, not to 3V3. That is ¼ of 200 mA, ≈0.1 W.
6. **Crystal: YXC X322516MLB4SI** (Y1, LCSC C13738 basic, 3225-4P, 16 MHz, CL = 9 pF, ESR ≤ 50 Ω). Firmware and dapboot
   divide it by 2 (PLLXTPRE) and multiply by 9: 72 MHz SYSCLK, USB 72 / 1.5 = 48 MHz. Per AN2867,
   gm_crit = 4·ESR·(2πf)²·(C0 + CL)² ≈ 0.3 mA/V at 50 Ω, C0 ≈ 3 pF. The F1 HSE has gm ≥ 25 mA/V, so the gain margin is ≈85
   (≥ 5 is required). Load caps C6/C7 are 2·(CL − Cstray) ≈ 10 pF each (C32949). USB full speed only needs ±2500 ppm.
7. **Genuine silicon.** GD32/CKS32/APM32 "F103" parts differ in flash timing, USB and option bytes. Buy ST from LCSC/JLC.
   The firmware has only been checked against ST's register behaviour. Every F103C8 has 128 KB of flash; the layout uses 64 KB.

