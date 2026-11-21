# Firmware

The application runs a cooperative main loop with no RTOS: USB, then one step of the GPIB bus, then LED and watchdog.
Board-independent code is in [gpib-core](../../gpib-core). `Firmware/src/` holds
only what is specific to this board.

| Module | Job |
|---|---|
| `board` | clocks, pins, LED patterns, button, SysTick, unique ID, watchdog (about 4 s) |
| `gpib_phy` | byte handshake on the wire |
| `gpib_bus` | controller operations as non-blocking jobs, shared by both front ends |
| `usbtmc`, `instr` | USBTMC/USB488 front end; instrument auto-ID at start-up, `!` commands |
| `prologix` | `++` protocol on the serial port |
| `settings` | CRC-checked settings log in two flash pages |
| `trace` | optional bus log on J3 (115200 8N1, 3.3 V) |

## Flash layout

| Region | Address | Size |
|---|---|---|
| dapboot | 0x08000000 | 8 KB |
| Application | 0x08002000 | 54 KB |
| Settings log | 0x0800F800 | 2 KB |

Every F103C8 has 128 KB of flash, so the app has room to grow.

## Build options

| Option | Effect |
|---|---|
| `HSE_MHZ=8` | for a dev board with an 8 MHz crystal. Run `make clean` and `make -C ../bootloader/dapboot/src clean` first |
| `BENCH=1` | brings USB up with no instrument, for a board without J2. Never ships |
| `PROG=openocd` | flash with OpenOCD instead of pyOCD |
| `make bringup` | SWD-only test image with Prologix on RTT, no dapboot. `make flash-bringup` writes it, `tools/bringup.py` drives it |

The bring-up image and `usb_bench.py --modes` leave settings behind that `flash-all` keeps. Run
`pyocd erase --chip -t stm32f103c8` before flashing a release.

## Using both interfaces

Both interfaces are always enumerated, and the host picks one by opening it. Each has its own target address and
modes, and every job re-addresses, so neither disturbs the other. A job owns the bus until its message ends.

REN is bus-wide: it stays asserted while either side wants remote. USBTMC auto-polls SRQ only for its own instrument,
and only while it was the last side to use the bus; the serial port polls only on `++spoll`.

## Prologix commands

Controller mode, as in the Prologix manual: `++addr`, `++auto`, `++clr`, `++eoi`, `++eos`, `++eot_enable`, `++eot_char`,
`++ifc`, `++llo`, `++loc`, `++mode` (controller only), `++read [eoi|<char>]`, `++read_tmo_ms`, `++rst`, `++savecfg`,
`++spoll`, `++srq`, `++trg`, `++ver`, `++help`. `++addr` defaults to the instrument found at start-up, and setting it to that address keeps it following auto-ID (an instrument swap needs no new `++addr`). Secondary
addresses are limited to 96–102.

Extensions:

| Command | Purpose |
|---|---|
| `++autoid on\|off\|slow\|slower\|slowest` | read `*IDN?` at start-up for the USB serial number |
| `++idstring short\|normal` | short or full serial string |
| `++scan` | list responding addresses |
| `++lon 1` | listen-only capture, e.g. plotter output, until the next input |
| `++ton 0\|1` | talk-only: data lines go to listen-only devices with no addressing |
| `++err` | last bus error and the phase it happened in |
| `++trace on\|off` | bus log on J3 |
| `++usb tmc\|cdc\|both` | interfaces to enumerate; saved, and the adapter restarts |
| `++dfu` | restart into dapboot |

## Entering the bootloader

`dfu-util` (DFU_DETACH), `++dfu`, `!reset`, or SW1 while the app runs: each writes dapboot's magic to BKP->DR1 and
resets. Holding SW1 while plugging in also stays in dapboot, as does an invalid app, so a bad flash can't brick the
board. SWD is the recovery path, as the ROM bootloader's USART1 pins are SRQ and ATN.

The app carries the dapboot it was built with (`src/dapboot_image.c`). At boot it compares that copy with the
bootloader pages and rewrites them when they differ, so a DFU update of the app also updates dapboot, which DFU
itself can't reach. The rewrite takes well under a second; losing power during the last page (the vector table,
about 50 ms) leaves a board that only SWD can revive.

## Differences from UsbGpib

- USB ID 1209:0001, not 03EB:2065, so host tools that look for the old ID don't find it.
- Settings are not migrated from the v2 layout.
- Data settle before DAV is 2 µs (IEEE 488.1 T1).
- A write with no listener fails instead of going nowhere.
- EOI is asserted only after ATN is released.
- READ_STATUS_BYTE is always answered after its serial poll, which can cut in between the bytes of a transfer.
- A read that sets TermCharEnabled uses VISA's character for that read only; other reads use the `!term` setting.
- USBTMC IN transfers carry at most 1008 bytes, so they end on a short packet (pyvisa-py hangs otherwise).

## Known limitations

- dapboot hangs in its crystal wait if Y1 is dead, so the app's no-crystal LED never shows.
- GTL and REN_CONTROL wait until a read finishes its message, and every transfer re-asserts REN (as v2).
- SRQ from another instrument queues a USBTMC notification every 100 ms, which can push out others.
- Talk-only doesn't unaddress an instrument the same front end left addressed as talker.
- An instrument that answers neither `*IDN?` nor `ID?` delays enumeration about 3 s.
- Secondary addresses above 6 need a wider address type in `gpib_bus`.
