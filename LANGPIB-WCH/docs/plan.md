# LANGPIB-WCH: plan

The LANGPIB GPIB plug-on dongle on the WCH **CH32V407RET6** (LQFP64, RISC-V 200 MHz, 10/100 MAC **and PHY on chip**, USB-HS PHY used
at full speed). Hardware only for now; firmware later on gpib-core. MCU reference: `../../Reference-WCH` (hardware reference design,
Ethernet README, bootloader and programmer designs).

## Requirements (user, 2026-11-03)

- Form: 57.15 x 46.2 mm, plugs onto the instrument's GPIB socket, RJ45 and USB-C side by side on
  the right edge, all parts on top, cad-tools enclosure. ~~2 layers~~ **4 layers** (user, 2026-11-04: still under the
  order limit; In1 GND, In2 +3V3).
- ~~Dual RJ45 footprint (SMD default + THT backup), tab up~~: **replaced** (user, 2026-11-04) by the THT **HanRun
  HR911105A MagJack**, JLC-fitted, tab down: no SMD MagJack is stocked at JLC, and the
  transformer + line-side isolation took about 19 x 18 mm. USB-C and RJ45 stay on the same (right) edge.
- ~~PoE-ready~~: **dropped** (user, 2026-11-03, after the BOM estimate): PoE-ready cost 10 mm of height and isolation
  spacing for ~$7/board of parts nobody may fit. Board back to 57.15 x 46.2 mm, USB bus-powered only.
- **5 assembled boards** in the first order (JLC, under $59).

## Ethernet front end (2026-11-04)

**HanRun HR911105A** (C12074, $1.02 @100, 47k, extended, through-hole fitted by JLC: $4.73 per 5-board order): 1CT:1CT
magnetics, 75R / 1n 2 kV Bob Smith and green / yellow LEDs inside; tab down. As WCH's own EVT board.

| Item | Choice |
|---|---|
| MDI | TP / TN -> jack 6 / 3, RP / RN -> 2 / 1: pairs and polarity swapped so they reach the jack on F.Cu without crossing; the PHY's Auto-MDIX and polarity detection correct both (DS note 11) |
| Centre taps | Jack 5 (TX pair) and 4 (RX pair): 100n to GND each, no 3V3 bias, no 49.9R (WCH EVT) |
| LEDs | PD15 (ETH_LED1) -> 470R -> yellow 12, PD14 (ETH_LED0) -> 470R -> green 9 (swapped for routing; modes are firmware). Needs `AFIO_PCFR1.ETHPHY_LED_RM` |

History: the first routing (2026-11-03) used a plain SMD jack (Ckmtw R-RJ45M08P-C000) with a THT backup on the same spot
(R-RJ45R08P-C000, no LED holes) and a Pulse HX1188NLT with a discrete Bob Smith, a 0.9 mm line-side clearance rule and a
via column to the backup's holes. Parts and footprints are in kicad-tools `docs/parts.md` and `lib/gen_langpib_wch_fp.py`.

## PoE: dropped on this board

Studied and dropped: SDAPO DP9900LP-5V (C5380108, $6.52) + 2 x MB6S + SMAJ58A + OR diode. JLC-assembled it doubled the
5-board order ($46 -> $95). A PoE respin would need the 100n 100 V DC blocks back in the Bob Smith legs.

2026-11-04 (user): for PoE use an isolated splitter, **Adafruit 4552**; DIY splitter dropped. Onboard PoE with the
HY931147C PoE MagJack and a TPS23753A flyback stage is parked as research: [poe.md](poe.md).

## MCU and support (from `../../Reference-WCH/hardware/reference-design-ch32v407ret6.md` §7)

- U1 CH32V407RET6 (C55068250, $1.18, 440 in stock).
- Decoupling: 100n at VBAT (tied to VDD, no RTC battery), VDDA, VDD 18 / 49 / 64; 100n + 10u 0603 at main VDD 32; 2.2u
  0603 + 100n at VDDK 27.
- Y1 25 MHz YXC X322525MOB4SI (C9006, basic) + 2 x 15p; pads 2 / 4 to GND. No LSE (PC14 / PC15 free).
- NRST: 22n to GND; to the debug header. No BOOT pins on the LQFP64: own DFU bootloader, **DFU button** to GND (internal pull-up).
- USB-C (block `usbc-y495-srv05`): CC 5k1, SRV05-4; USBHS1 at full speed on PA11 / PA12, direct (no series R, pull-up inside).
  VBUS is +5V directly (bus-powered only: no OR diode, no VBUS sense).
- 3V3: AMS1117-3.3 (block `ldo-ams1117-3v3`), ~150-200 mA, from VBUS.
- **SDI debug header 1x7** (2.54 mm, DNP): 1 GND, 2 SWIO PA13, 3 SWCLK PA14, 4 NRST, 5 USART1 RX PA10, 6 USART1 TX PA9, 7 GND
  (matches the RP2350-Zero probe, `../../Reference-WCH/docs/programmer-design.md`).
- Status LED (block `status-leds`, one LED).

## GPIB pin map (pin *set*; the order is set after placement, `docs/workflow.md` pin-map review)

FT (5 V tolerant) pins on the RET6, after SDI (PA13 / PA14), USART1 (PA9 / PA10) and the PHY LEDs (PD14 / PD15): PB3, PB4,
PB5, PB8-PB15, PA8, PA15, PC10, PC11, PC12, PD2, PD8 = 18 for GPIB's 16.

- **DIO1-8 = PB8-PB15** in bit order (one `GPIOB->INDR >> 8` read), the same idea as port D on the STM32.
- Control lines: 8 of PB3, PB4, PB5, PA8, PA15, PC10, PC11, PC12, PD2, PD8 (two spare). ATN keeps its 100k pull-down.
- DFU button on PD8 and status LED on PA8 (user, 2026-11-04: the two spare FT pins sit on U1's west side, facing the
  top-left corner where SW1 / D1 are); PA1 / PA2 spare.

## Extended parts (assembled) and cost

CH32V407RET6, USB-C Y495, HR911105A: 3 types. Hand-fitted / DNP: 1x7 header, GPIB plug.
Estimate (jlc_cost.py, 2026-11-04, 5 boards, 4 layers): **$53.33** ($10.67 / board), incl. $4.73 for JLC's through-hole fitting.

## Open

- Done (board 1, 2026-11-21): the USBHS1 internal D+ pull-up works (the D+ net has only J1, U1 and U2's ESD array, and
  the board enumerates); `FEATURE_SIGN` = 0xE33900FF, bit 0 = 1 (ACT LED supported, both jack LEDs work).
- 