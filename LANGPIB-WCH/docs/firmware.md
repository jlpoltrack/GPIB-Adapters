# LANGPIB-WCH: firmware

Port of the LANGPIB firmware to the CH32V407RET6. Goals, VXI-11 scope and bus sharing are in LANGPIB-STM32's
firmware notes and hold here; the structure follows LANGPIB-RP2350's (lwIP,
`net.c`, polled Ethernet). This file covers what differs. Chip facts: the Reference-WCH repo (`../../Reference-WCH`: RM translation in
`docs/rm/`, `ethernet/README.md`, `docs/bootloader-design.md`).

**Status (2026-11-21):** board 1 brought up on the `SIM=1` build, then J3 fitted: the real build runs an HP 3456A and
an HP 66332A over USB and Ethernet (see "Still to verify").
Verified on silicon (`tools/bringup.py`): SDI flashing over 2-wire and 1-wire (RP2350-Zero probe), 25 MHz crystal and
200 MHz, RNG, watchdog, UART log, GPIB pin self-test (16 lines ok), PHY id 0x73719011; USB: enumeration, CDC Prologix,
USBTMC (PyVISA-py), reads to 254 B and writes to 2000 B through the simulated instrument; Ethernet: 100M full duplex
(`MDI, polarity reversed`), AutoIP, mDNS, both jack LEDs, web page and LXI XML, VXI-11 (`vxi11_check.py`, all
checks), TCP 5025 and 1234 (`sockets_check.py`); DFU: update over USB with no button (runtime detach), two downloads
back to back, the app rewriting the bootloader; settings across `++rst` (`++net name`). D1 lights. With J3: real
instruments on every transport. Not yet run: DHCP from a server (tested direct to a Mac only), SW1 entry into DFU,
10M link.
Chip: ID 0x46720012, FEATURE_SIGN 0xE33900FF (bit 0 = 1: ACT LED), 576 KB flash, RAM_CODE_MOD 0 (200 KB RAM); UID word 3
reads 0xE339E339 (erased), so only 64 UID bits are real.

Fixed at bring-up: the IWDG setup hung (PVU/RVU only clear on the LSI clock, which nothing had started: the LSI is now
enabled first, `watchdog on|OFF` in the boot line); the 4 KB stack was too small (gpib-core's simulator overflowed it,
and lwIP's mDNS probe tie-breaking nests ~3.3 KB of frames: now 8 KB); the web page was cut short (gpib-core `http.c`
counted bytes lwIP's full send queue had dropped; a short TCP write now resets the connection instead); `dfu-util`
lost the bootloader to its own 1 s reset (now 3 s, cancelled by a new download).

## Layout and building

`Firmware/src/`: `startup.S`, `trap.c` (the one trap entry), `board.c` + `board_hw.h` (gpib-core's board layer),
`clock.c` (200 MHz, shared with the bootloader), `gpib_pins.h`, `dcd_ch32v407.c` (TinyUSB device driver for the V407's
USBHS), `eth.c` (MAC, DMA, on-chip PHY), `net.c` (lwIP netif, DHCP then AutoIP, VXI-11), `boot_image.c` (the embedded
bootloader), `update.c` (firmware update over Ethernet), `main.c`, configs (`tusb_config.h`, `lwipopts.h`, `arch/cc.h`, `app_config.h`). `Firmware/bootloader/`:
the DFU bootloader (and `stage.c`, the network update's stage, built into both). `Firmware/tools/mkdfu.py`: app
header and DFU suffix.
`Firmware/lib/`: submodules TinyUSB 0.21.0, lwIP STABLE-2_2_1 (gpib-core is `../gpib-core` at the repo root); `wch/`: WCH's register headers only
(`SOURCE.md`), no peripheral library.

```
cd Firmware
make                    # bootloader first, then build/langpib.elf (0x08004100), .dfu, -sdi.hex (header + app)
make flash              # SDI, RP2350-Zero probe (PROBE=/dev/cu.usbmodemXXXX): bootloader + header + app
make production         # a new board: erase, bootloader + app, boot check, docs/production-log.csv
make dfu                # USB: dfu-util detaches the app and downloads build/langpib.dfu
make netdfu HOST=ip PASSWORD=pw   # Ethernet: the same .dfu by HTTP POST (see "Firmware update over Ethernet")
make test               # gpib-core's host tests, the bootloader's image and update-stage tests
make SIM=1 [flash]      # build-sim/: gpib-core's simulated instrument at GPIB 5 instead of the plug
make bringup [SIM=1] [STAGE=flash|log|usb|usbtmc|net] [HOST=ip]   # tools/bringup.py: the bring-up checks below
```

**No GPIB plug (SIM=1):** without a bus ATN reads low, so the normal firmware waits for an instrument and never
starts USB or the network services. `make SIM=1` swaps the GPIB lines for gpib-core's simulated SCPI instrument
(`gpib-core/sim/`: `*IDN?` = `LANGPIB,SIMULATED,0,0.1`, `MEAS?`, `BLOCK? n`, echoes other queries) and leaves the
GPIB pins untouched. It enumerates as "LAN GPIB adapter (simulated GPIB)", logs `GPIB: SIMULATED instrument`, and
`make production` refuses it. Everything but the GPIB lines can be brought up this way: USB, Ethernet, VXI-11, 5025,
1234, the web page, mDNS, settings and DFU.

**GPIB pin self-test** (`src/pintest.c`, both builds): at boot, only if ATN reads low with R6 alone (no powered bus),
each line gets the internal pull-up (30-50k; ATN still reads high against R6's 100k), then each line is pulled low in
turn. The log says `GPIB pins: ok` or lists stuck-low, stuck-high and bridged pairs, which finds U1 and J3 solder
faults with nothing plugged in. An instrument switched off on the plug can also hold lines low. If ATN stays low
against the pull-up (a short, or another controller holding it on a live bus), no line is driven and ATN is reported
stuck low. The lines are put back as reset left them; `production.py` fails a board that logs a fault or
`watchdog OFF`.

**Toolchain:** xPack `riscv-none-elf-gcc` 15.2.0-1 in `~/opt` (the Makefile finds it; `CROSS=` overrides). Homebrew's
`riscv64-elf-gcc` has no C library, which gpib-core, TinyUSB and lwIP need. Code is plain `rv32imac` (newlib-nano's
multilib); the core also has Zba/Zbb/Zbs and vector, unused.

## CH32V407 specifics

- **Clocks:** WCH's 400 / 200 MHz setup: 25 MHz HSE x16 = 400 MHz SYSCLK, HCLK = APB1 = APB2 = 200 MHz, flash clock
  HCLK / 2 (100 MHz max). Without the crystal the PLL runs from the 20 MHz HSI; USB and Ethernet need the HSE, so the LED
  shows `LED_NO_CLOCK` and the firmware stops there.
- **Interrupts:** SysTick (1 ms, LED patterns) and USBHS1 only. One trap entry (mtvec mode 0, 1 KB aligned) dispatches
  on mcause; hardware stacking and nesting stay off, so handlers are plain C. An exception parks the CPU and is kept in
  `.noinit` (the last 256 B of RAM, which the bootloader doesn't use); the watchdog resets and the next boot logs
  mcause / mepc / mtval on the UART.
- **Flash:** bootloader in sectors 0-3, app header page at 0x08004000, app from 0x08004100, the network update's
  stage from 0x08041000, settings log in the last two 4 KB sectors of the 512 KB zero-wait area (0x0807E000). Standard
  mode: 4 KB
  sector erase, 16-bit programming; the CPU stalls meanwhile. **Erased flash reads 0xE339E339**, not 0xFFFFFFFF:
  gpib-core takes `BOARD_FLASH_ERASED` from `board_hw.h`.
- **GPIB:** every line FT, open-drain output at 2 MHz (CFG 0x6, the slowest edges), SRQ floating input; no pulls (ATN's R6 must read low with no
  bus). DIO1-8 are on PB8-PB15 out of bit order: one `GPIOB->INDR` read and a 256-entry table in, one `BSHR` write and
  a table out.
- **RAM:** about three-quarters is network buffers: lwIP's pbuf pool 24.5 KB and heap 16 KB, VXI-11's connections
  19 KB, Ethernet DMA 24 KB. The linker scripts take 128 KB, which holds under either `RAM_CODE_MOD` option-byte
  setting (RM 32.6: 1 = 576 KB code + 136 KB RAM, 0 = 512 KB code + 200 KB RAM). If RAM runs short, programming
  `RAM_CODE_MOD` = 0 over SDI gives 200 KB; code already stays in the first 512 KB.
- **Watchdog:** IWDG, LSI / 64, 2500 counts, about 4 s; stopped while a debugger halts the core (DBGMCU_CR is CSR 0x7C0).
  The LSI must be started first (RCC_RSTSCKR.LSION): without it the IWDG never takes its prescaler and reload.
- **UART:** USART1 TX on J4 (PA9, 115200 8N1): gpib-core's bus trace and a blocking bring-up log (`board_log()`), which
  prints the reset cause, HSE state, clock, UID, flash size, PHY ID and any trap from before the reset.
- **LED:** D1 sinks into PA8 (active low). On: ready. 2 Hz: no GPIB bus. Long on, short off (1.1 s): bus, no
  instrument. 10 Hz: no crystal. Double blink each second: the bootloader (DFU). **DFU button:** SW1 on PD8, internal pull-up, read by the bootloader.

## USB

TinyUSB has no port for this USBHS (its WCH driver is the V307's, a different register map), so `dcd_ch32v407.c` is
ours, from RM 22 and WCH's V407 examples (`USBHS/Device/CH372Device`, `IAP/USB_UART`). USBHS1 runs as a full-speed
device on PA11 / PA12; composite USBTMC + CDC (Prologix) + DFU runtime as on the siblings.

- **Clocks:** USBHS PLL from the 25 MHz HSE (assumed needed at full speed too), UTMI1 gate (CFGR2 bit 30: WCH's header
  has UTMI1 / UTMI2 swapped), then the module. The D+ pull-up is the device-enable bit, as in WCH's code.
- **Transfers:** one packet per interrupt; EP1-7 toggles in hardware, EP0's in software. The DMA goes straight to the
  caller's buffer when it is 4-byte aligned in SRAM and a full packet fits, else through a 64-byte bounce buffer. The
  device address is set in the interrupt when SET_ADDRESS's status stage completes.
- **Taken from WCH, not the RM:** EP0 accepts a SETUP while NAK or STALL; a completion sets NAK and clears its flag in
  one write; an OUT packet with the wrong toggle is dropped and the endpoint re-armed.
- **Risk:** if the hardware doesn't NAK while a completion flag is pending, a packet could be lost or doubled (WCH's
  examples share it). Not seen so far: 22 KB USBTMC reads and 3005-byte writes to an HP 66332A, and thousands of short
  transfers, all intact (2026-11-21); an analyser capture would still settle it.

## Ethernet

On-chip MAC and 10/100 PHY (`eth.c`, a register-level port of WCH's `eth_driver.c`), polled from the main loop like
the RP2350's: no ETH interrupt. `net.c` as on the RP2350: MAC 02: plus 5 bytes of an FNV-1a hash of the 64 real UID
bits (a plain XOR fold gave every board of one chip lot the same last two bytes, so the same hostname), hostname
`langpib-XXXX`, DHCP then AutoIP, VXI-11.

- **Init:** ETH PLL (RCC_CTLR 22 / 23), MAC clock and reset, DMA reset, MDC = HCLK / 102. PHY reset, then 10/100
  half and full advertised, auto-negotiation. Auto-MDIX and polarity correction are never touched: they must stay on
  (the MDI pairs and polarity are swapped at the jack), and the link log should say `MDI-X, polarity reversed`.
- **Buffers:** 12 RX (6 missed frames in bursts from five clients) and 4 TX of 1536 B, one frame each; lwIP copies in and out, software checksums (WCH leaves the
  offload off). Up to 8 frames a poll; a full ring drops frames into `eth_stats()->rx_missed`.
- **Link:** polled every 500 ms (WCH's rule: link bit, and auto-negotiation done if the partner negotiates); speed and
  duplex go to the MAC, which is restarted on every link-up.
- **LEDs:** PD14 / PD15 with `AFIO_PCFR1.ETHPHY_LED_RM` and page 7 `led_enable`, always. `FEATURE_SIGN` bit 0 = 0
  means LINK only, not no LEDs (RM 33.2.2): WCH's driver skips the LEDs then, we only skip its ACT tweak to the
  undocumented LED_CTRL. WCH also remaps only for chip IDs x2 / x5. Chip ID and FEATURE_SIGN are logged at boot.
- **Taken from WCH, not the RM:** MACCR.FES (speed) is written though the RM marks it read-only (probably ignored); the
  PHY reset is a pulse; LED_CTRL.
- **Checked against RM 29 / 33 (2026-11-04, review of every register and descriptor bit):** no blocking bug. Fixed from
  it: the LEDs as above; PHY_CR's reset release written again after the MAC soft reset (which may reset it, and it
  reads as 0); "partner doesn't negotiate" from ANER bit 0 (ANLPAR never reads 0); an MDIO access never starts while
  one is busy, and a timed-out status read is no link; a TX FIFO flush that doesn't self-clear is cleared and logged;
  `tx_err` counts carrier and collision errors too. MACFFR.PAM passes all multicast, for mDNS.
- **Only hardware can tell:** whether the MAC soft reset resets PHY_CR; whether FTF self-clears; whether FES follows
  the PHY's speed (and 10M works); LINK-only LEDs on a FEATURE_SIGN bit 0 = 0 part; Auto-MDIX and polarity correction
  on after reset (log against a fixed-MDI partner at 10 and 100M); the PHY's MDIO wake-up time after reset (10 us now).
  Board 1 (2026-11-21): Auto-MDIX and polarity correction are on after reset (100M against the Mac, `MDI, polarity
  reversed`), the 10 us wake-up is enough (PHY ID read every boot), and no `ETH:` flush error has been logged. Its part
  has bit 0 = 1, so the LINK-only case waits for a bit 0 = 0 chip.

## Network settings and mDNS

- **Settings** (gpib-core `netcfg.c`), the same command as `!net` on USBTMC and `++net` on Prologix (USB or TCP 1234):
  `net` (query: `dhcp; ip 192.168.1.23; name langpib-1a2b; update off`), `net dhcp` (the default: DHCP, AutoIP
  169.254.x.x if no server answers), `net static <ip> <mask> [<gateway>]`, `net name <host>` / `net name default`,
  `net password <pw>` / `net password off` (firmware update over Ethernet; from USB only). Saved at once,
  applied by `net_init()` at the next reset (`++rst` or a power cycle), so changing the address from the network
  doesn't cut the connection doing it. Invalid values are ignored, like other setters', and show in the query.
- **Recovery:** if a static address is wrong, `++net dhcp` over the USB CDC port, then `++rst`.
- **mDNS** (lwIP's responder, gpib-core `mdns_lwip.c`): `<hostname>.local` from start-up (default `langpib-XXXX`, the
  last two MAC bytes; the same name goes to DHCP), then once the instrument is found `_vxi-11._tcp` (111),
  `_scpi-raw._tcp` (5025), `_lxi._tcp` and `_http._tcp` (80) as `<instrument ID> (<hostname>)`; `_lxi` carries the
  TXT keys LXI asks for (Manufacturer, Model, SerialNumber, FirmwareVersion). Re-announced when the address changes.
  Needs IGMP and MACFFR.PAM. Check from the Mac: `dns-sd -B _vxi-11._tcp`, `dns-sd -B _lxi._tcp`,
  `ping langpib-XXXX.local`, PyVISA `TCPIP::langpib-XXXX.local::INSTR`.
- **Web page** (gpib-core `http.c`, TCP 80, three connections, a new one replaces the oldest):
  - `/`: the instrument (GPIB address, `*IDN?`, VISA strings), the network (address, mask, gateway, MAC), serial and
    firmware, and a form for the network settings (`POST /net`, the same `net` command; invalid values are refused
    with a message, nothing changes), and the firmware update form (`POST /update`, below) once a password is set.
  - `/lxi/identification`: the LXI identification XML (schema 1.0), for Keysight Connection Expert, NI MAX and the
    like. It validates against the LXI Consortium's XSD. `LXIVersion` says 1.6, but the adapter isn't LXI-certified
    and doesn't do the rest of LXI (LAN reset button, LCI, IPv6).
  - No authentication for the settings, like ports 1234 and 5025, which can change the same; only the firmware
    update needs its password. The page is rendered again for
    each TCP window rather than buffered, so it costs no RAM per connection beyond the request (~400 B).
  - On the Mac: `make host` in gpib-core, `build/host/vxi11_host` serves the page on `http://127.0.0.1:8080/`.

## Raw SCPI (TCP 5025) and Prologix (TCP 1234)

From gpib-core (`scpi_raw.c`, `prologix.c`, `sockets_lwip.c`), started with VXI-11 once the instrument is found. One
client each; a new connection replaces the old one, so a client that vanished can't lock the port.

- **5025:** each line goes to the instrument as sent (EOI on its last byte); a line with a `?` then reads the reply
  (EOI, or the `!term` read termination) and sends it back. A query keeps the bus from its write to the end of its reply,
  so no other client's command comes in between. No `!` commands, as on VXI-11. PyVISA:
  `TCPIP::<ip>::5025::SOCKET` with `read_termination='\n'`.
- **1234:** the Prologix GPIB-ETHERNET protocol, the same `++` commands as the USB CDC port, which runs alongside. The
  `++` settings are shared with the CDC port and saved like it.
- **Bus sharing:** USBTMC, CDC Prologix, VXI-11, 5025 and 1234 each own their jobs on the one bus (gpib-core's round
  robin); VXI-11 locks don't stop the other front ends.
- **Tested on the host:** gpib-core's `make test` (protocol, simulated bus) and `tools/sockets_check.py` against
  `vxi11_host` (raw queries, a client replacing a stale one, PyVISA SOCKET, Prologix, both ports at once). On a board:
  `tools/sockets_check.py <ip>`.

## DFU bootloader

The LQFP64 has no BOOT pins, so USB updates go through our own bootloader in sectors 0-3 (`Firmware/bootloader/`),
after the WCH repo's `docs/bootloader-design.md`.

- **From the running app** (an instrument found, so USB is up): `make dfu` (dfu-util's DFU_DETACH), `++dfu` on the
  USB CDC port, `!reset` over USBTMC (a `!` command: INDICATOR_PULSE first). `++dfu` on TCP 1234 is refused (`dfu:
  from USB only`): anyone on the LAN could otherwise leave the adapter in DFU until a power cycle. With no
  instrument the app never starts USB: SW1 at plug-in or the probe.
- **Entry:** before any clock or USB setup it reads BKP DATAR1 (0xDF11 from the app's `board_enter_bootloader()`,
  0x5354 from `board_install_update()`, cleared) and SW1 (PD8, five reads 1 ms apart). Neither DFU request: it installs
  a staged network update if there is one to install (below), then checks the app's header (magic, version, length,
  CRC-32 over the image). All good: it jumps to 0x08004100 with the peripherals as reset left them. Otherwise DFU, with
  D1 double-blinking once a second, unlike any app pattern (10 Hz if the crystal is dead: no USB then).
- **DFU:** TinyUSB's DFU class over the app's USBHS driver, polled (no interrupts). Same VID:PID as the app, bcdDevice
  0xDF10, MS OS 2.0 WinUSB, download only (no upload, so no cloning). 4 KB blocks: block n is the sector at
  0x08004000 + n x 4 KB, erased then written in fast 256-byte pages and read back. Block 0's first page is the header:
  kept in RAM, written at manifestation only if the CRC over the written image matches. A cut-off download leaves no
  header, so the next boot stays in DFU. After a good download it resets after 1 s (or 50 ms after DFU_DETACH).
  Block 0 also erases a network update's stage, so a stale one can't later be copied over the download.
- **Safety:** the address checks (`image_block_addr()`, and again in `flash.c`) keep it inside 0x08004000-0x0807DFFF:
  it can't erase itself or the settings. Host tests (`make test`) cover the CRC, the header against `mkdfu.py`'s output
  and every block number. A USB download may fill the whole area; only network updates are limited to half of it.
- **Embedded in the app** (USBGPIB-STM32's dapboot trick): the app carries `boot.bin` (`src/boot_image.c`, `.incbin`)
  and at boot rewrites sectors 0-3 when they differ, sector 0 last, so a DFU update of the app updates the bootloader
  too. So the design's WRPR on sectors 0-3 is not used. A power cut in the sector-0 rewrite (one 4 KB erase and
  write) leaves a board only the SDI probe can revive; the log says `bootloader: updating` / `updated`.
- **Image:** `tools/mkdfu.py` puts the 256-byte header (`bootloader/image.h`) in front of `langpib.bin` and the DFU
  suffix after it; `-sdi.hex` is header + image for the probe.
- **USB IDs:** still pid.codes' test PID 1209:0001, as on the siblings (design question B2).

## Firmware update over Ethernet

For adapters with no computer on their USB port (powered from a charger or a PoE splitter). The same `.dfu` as for
USB DFU goes up by HTTP POST; the app checks it into a stage in unused flash and the bootloader installs it
(`src/update.c`, `bootloader/stage.c`, gpib-core `http.c` and `netcfg.c`).

- **Turning it on:** off until an update password is set over USB: `++net password <pw>` on the CDC port or
  `!net password <pw>` over USBTMC (1-32 printable ASCII characters, no spaces); `net password off` turns it off again.
  Prologix on TCP 1234 and the web page can't set or clear it. The `net` query ends `update on` or `update off`, never
  the password. It is kept in the settings log in plain text (readable only through the SDI probe).
- **Uploading:** `make netdfu HOST=<ip or name> PASSWORD=<pw>` (`SIM=1` sends `build-sim/`'s), which runs
  `curl --data-binary @build/langpib.dfu -H 'X-Update-Password: <pw>' -H 'Expect:' http://<host>/update`; or the
  "Firmware update" form on the web page (a file picker and the password; the page's script sends the same request).
  Replies: 200 staged (the adapter resets 0.5 s later to install it, about a second, then starts the new firmware);
  403 updates off or wrong password; 429 within 1 s of a wrong password; 400 not a valid image; 413 too large;
  411 no Content-Length; 500 flash error; 409 another upload took over. A refused upload changes nothing that runs.
- **Flash:** the app area (0x08004000-0x0807DFFF, 488 KB) is split in half. Header + app sit below 0x08041000, the
  stage (header page + image, the same layout) from there to the settings. So an update, and an app, can be at most
  249,600 bytes (now ~122 KB): the app's linker script fails the build if it grows into the stage.
- **Staging (the app):** the body streams to flash as it arrives (no image buffer; TCP's window holds the client off
  during erases). The header page is checked first (magic, version, a length that fits and matches Content-Length),
  before anything is erased; each stage sector is erased as the data reaches it. At the end: the DFU suffix (`UFD`,
  its CRC over the whole file), then the header's CRC over the image read back from flash, and only then the stage's
  header page is written. The reply goes out, then a reset with 0x5354 in BKP DATAR1.
- **Installing (the bootloader, at the reset clock):** if the flag is set, or the app is invalid, and the stage is
  valid: the app's first sector (with its header) is erased first, the image copied page by page, its CRC checked in
  place, the header page written last and checked, then the stage's first sector erased. The app then starts if valid,
  else DFU (SW1 and the DFU request still come first and skip the install).
- **Power cuts:** BKP DATAR1 is lost with power (VBAT is tied to VDD), so a resumed copy doesn't depend on it: a cut
  copy leaves the app invalid (its header goes first and comes back last) and the stage valid, so the next boot copies
  again. A cut while staging leaves no valid stage (its header goes last) and the running app untouched. Without the
  flag a valid app is never overwritten, so a stage left behind (a cut in the stage's final erase, or between staging
  and the reset) does nothing. A flash error in the copy ends in DFU with the stage kept; the next boot tries again.
- **First time:** the bootloader that knows the stage ships in the app, which rewrites sectors 0-3 at its first boot,
  before it can take a network update; so a board needs one update to this firmware (USB) first.
- **Tested on the host** (`make test`): gpib-core's `test_http` (password, the wait after a wrong one, uploads in any
  read sizes, every refusal with the body read so the client gets the reply, a second upload taking over),
  `test_netcfg` and `test_prologix` (the password from USB only); the bootloader's `test_stage` on a simulated flash
  that loses power mid-operation: staging in any chunk sizes, every refusal (a bad header before any flash write), and
  a power cut at every erase and program of both the staging and the install (a 150 KB image: 625 operations), each
  followed by a boot without the flag.
- **Not covered:** the password crosses the LAN in clear (no HTTPS), and images aren't signed: anyone with the password
  can install any image with a valid header. The web page keeps three connections, a new one replacing the oldest, so
  a burst of other requests during an upload drops it (409 or a reset; the stage stays invalid).

## Production

`make production` (`Firmware/tools/production.py`) does a board in one run through the RP2350-Zero SDI probe on J4,
with the WCH repo's `tools/sdi_probe.py` for the GDB protocol:

1. Finds the probe by its USB product name (`sdi_probe.find_ports()`; it shares 1209:0001 with the board), attaches,
   reads the UID, chip ID and flash size.
2. Option bytes: stops on read protection (`sdi_probe.py monitor unprotect --force` erases it, then power-cycle) or on
   write protection of any sector (the app rewrites the bootloader). Logs RAM_CODE_MOD. Nothing is written to them.
3. Mass erase, so the settings are defaults (`ARGS=--keep-settings` skips it for a re-flash).
4. Bootloader at 0x08000000 and header + app at 0x08004000 (built like `mkdfu.py`), verified by reading back.
5. Reset, then the boot log through the probe's UART bridge: 200 MHz on the crystal, UID as read over SDI, a PHY that
   answers MDIO, no `ETH:` errors or trap, no bootloader rewrite.
6. A row in `docs/production-log.csv`: time, result, UID, chip ID, hostname, PHY ID, FEATURE_SIGN, option bytes, app
   version, CRCs, error.

No GPIB instrument or LAN is needed. Steps 1-4 pass against the WCH repo's `gdb_sim_server` (`--port-gdb
socket://127.0.0.1:3333 --no-boot-check`); the UART check waits for a board. On hardware a board takes about 10 s:
129 KB programmed and verified in 5.9 s (21 KB/s). Needs the probe firmware from Reference-WCH 2026-11-21 or later:
older ones left the flash half unlocked after the mass erase, and the next sector erase hung.

## Phases

| # | Phase | Needs | Status |
|---|---|---|---|
| 0 | Skeleton, board layer, USB driver, Ethernet driver, lwIP glue, DFU bootloader | nothing | written, builds |
| 1 | Bring-up: SDI flash, UART log (reset cause, 200 MHz, UID), LED, watchdog, settings across a reset | PCB + probe | done, board 1 |
| 2 | USB: enumeration, USBTMC + CDC with gpib-core's simulated instrument or a real one | PCB | done: simulator, HP 3456A, HP 66332A |
| 3 | Ethernet: PHY ID, link, LEDs, ping, DHCP / AutoIP | PCB + LAN | done except DHCP from a server and 10M |
| 4 | VXI-11 (gpib-core `tools/vxi11_check.py <ip> --board`), both transports at once | PCB + instrument | done (3456A, 66332A) |
| 5 | DFU on hardware: SW1 / detach entry, download, power cut mid-download, bootloader self-update | PCB | detach, download and self-update done; SW1 and power cut open |
| 5b | Production script on a board (`make production`) | PCB + probe | done: boards 2-5 |
| 5c | Firmware update over Ethernet (`make netdfu`, web form) | PCB + LAN | done on board 1 (curl); web form open |
| 6 | Extras: raw 5025 and Prologix 1234 | PCB + LAN | done (simulator and both instruments) |
| 7 | Extras: network settings (`!net` / `++net`), mDNS | PCB + LAN | done (`++net name` across a reset, mDNS browse) |
| 8 | Web page and LXI identification; Connection Expert / NI MAX find it | PCB + LAN | page and XML served; Connection Expert / NI MAX not tried |

## Bring-up order

1. Hardware first (hand-off): D1 lit, 3V3, crystal. SDI probe on J4, `make flash` (bootloader + app), UART on J4 at
   115200. If the app doesn't start, try `APP_ORIGIN=0x08000000` alone to take the bootloader out of the picture.
2. The log line: reset cause, `HSE ok`, 200 MHz. No log at all: halt with the probe and look at the PC (a PLL or
   SWS wait in `clock_init()` that never ends).
3. GPIB idle levels on the plug (all released high with an instrument, ATN low without).
4. USB: `ioreg -p IOUSB` / `dfu-util -l`; then gpib-core's checks.
5. Ethernet: PHY ID logged (0 or 0xFFFFFFFF: MDIO or PHY clock), link line, `net: address ...` line, LEDs, then a DHCP DISCOVER from
   02:... in Wireshark, then ping. No RX: DMA RX state (DMASR 19:17) and `rx_missed`; no TX: TX state (22:20), `tx_full`.

## Still to verify

Not yet tested on a board (board 1). Tick each off here with the result.

**GPIB (needs J3)**
- [x] After fitting J3 (2026-11-21): USB still enumerates (SIM build), so D+/D- aren't shorted under J3's west seat;
  the real build's boot log says `GPIB pins: ok` with no instrument (no bridged or grounded J3 joint; an open joint
  only shows with an instrument).
- [x] A real instrument (HP 3456A at GPIB 6, 2026-11-21, real build): auto-ID (no `*IDN?`, so the USB serial is
  `GPIB_06_<UID>`); USBTMC 8/8 and Prologix CDC 15/15 (USBGPIB-STM32 `tools/hp3456a_check.py --port <cdc>`: SRQ,
  serial poll, GET, SDC, abort, 200 reads); VXI-11, 5025 and 1234 all pass (gpib-core `vxi11_check.py <ip> --board`
  and `sockets_check.py <ip>`, both with `--expect '^[+-][0-9.]+E[+-][0-9]'`, since the meter answers with a reading).
- [x] HP 66332A at GPIB 5 (2026-11-21): USBGPIB-STM32 `tools/hp66332a_check.py --port <cdc>` 29/29 (22 KB reads at
  4.3 KB/s with the supply's acquisition, writes to 3005 B, SRQ, USBTMC and Prologix at once); `bringup.py net` and
  gpib-core's VXI-11 and socket checks all pass with no options (7 ms a `*IDN?` over VXI-11 or 5025).
  Seen on the way: after an instrument swap Prologix still addresses the old one if a client ever sent `++addr`
  (saved; only USBTMC, VXI-11 and 5025 follow the instrument found at start-up). And pyvisa-py's USBTMC writes use a
  fixed 1 s timeout, so a USBTMC query fails if another port holds the bus longer than that (e.g. a Prologix read
  waiting out `++read_tmo_ms` on a silent instrument); the adapter holds the next packet off until the bus is free.
- [x] GPIB transfer rate (USBGPIB-STM32 `tools/latency_3456a.py --port <cdc>`): meter-bound. A 14-byte reply reads in
  1.4-2.5 ms (the 3456A's handshake), a 2-byte write in 1.8 ms; 44 readings/s at D0, 0.1 PLC; ~6/s with the display
  on (D1), which stalls the bus ~130 ms per reading as on the STM32 adapter.
- [x] USBTMC, VXI-11 and 5025 clients at once (meter free-running, T1): 45 queries, no errors. Raw 5025 only reads
  after a line with `?`, which the 3456A takes as a syntax error: `T3?` gives no reading, so trigger over the others.
- [ ] Scope a DIO falling edge and the data-to-DAV settle time at the 2 MHz pin speed, on the longest cable in use.
  Traffic: `tools/bus_traffic.py <ip>` (3005-byte writes over VXI-11 to any SCPI instrument; trigger on DAV, J3 pin 6).
- [ ] Pin self-test guard: ATN tied to GND through a low resistance gives `ATN stuck low` and no pulses on other lines.
  Skipped (user, 2026-11-21): every line is open-drain, so a stuck line can't be fought.
- [ ] FT pins with the board unpowered: 5 V through 3k into a J3 pin should sit near 5 V (an unpowered adapter must not
  load the bus; the datasheet doesn't cover VDD = 0).

**Ethernet**
- [ ] 10M link: force the partner to 10baseT. TX is polarity-inverted at the jack and relies on the partner's polarity
  correction at 10BASE-T (hardware review).
- [ ] A partner without auto-MDIX (old switch or NIC with it off): the link and the logged MDI / MDI-X state.
- [ ] DHCP from a real server (so far only AutoIP, cabled straight to a Mac).
  Cabled straight to a Mac, the board answers ping 23 s after a reset (instrument search, ~10 s of DHCP tries, then
  AutoIP's probes); `bringup.py net` straight after a flash fails ping and HTTP for that reason, not a fault.
- [ ] Long run (hours, traffic on all ports): no `rx_missed` growth, no lwIP memory exhaustion, no resets.
  15 min done (2026-11-21): ~700 x 3005-byte VXI-11 writes to an HP 66332A (2.1 MB), 0 bad, with web page load
  alongside; no reset. The `loop:` line now carries the Ethernet counters: in the 4 min load above, 93 of 332k RX
  frames missed (`rx_missed`, the 6-buffer RX ring full in bursts; TCP resent them), no RX or TX errors. With 12 RX
  buffers (+9 KB RAM, now) the same load missed 0 of 338k. Hours still to run.
- [x] TCP throughput far above GPIB rates (2026-11-21): the 3 KB web page, a new connection each, 165 pages/s = 490 KB/s
  while VXI-11 traffic ran, against 4-7 KB/s from the instruments over GPIB. (The simulator caps replies at 254 B, so
  it can't measure long reads.)

**CPU and power**
- [x] CPU headroom (board 1, SIM build, 2026-11-21; `tools/load_all.py`, log line `loop:` every minute): idle 133k
  main-loop passes/s, longest pass 10 ms (once, at start-up; 3-4 ms after). Under all ports at once (VXI-11 165
  queries/s, USBTMC 75 and 5025 76 commands/s, 1234 328 serial polls/s, the web page 9/s; 0 errors in 157k requests)
  33-38k passes/s, so ~70 % busy with the longest pass 4 ms. The web page flat out (133 pages/s) alone takes it to
  ~5k passes/s and a 21 ms pass: the page is rendered again for every TCP window.
- [ ] 3V3 current with Ethernet and USB active; U3's dissipation at (5 - 3.3) V x I; 3V3 stays >= 3.2 V on a weak
  5 V supply (AMS1117 dropout, ceramic-only output caps).

**USB and DFU**
- [x] SW1 held while plugging in: DFU, and `make dfu` downloads (2026-11-21). Its 2 Hz blink then looked like the app's
  "no GPIB bus" blink, so the bootloader now double-blinks.
- [ ] Power cut mid-download stays in DFU; a CRC failure stays in DFU.
- [x] USB-C plugged in reversed: powers and enumerates (2026-11-21). A C-to-C cable (both CC resistors) not tried.
- [x] `board_usb_connect()` waited for the USB PLL with no timeout (a PLL that never locks was a 4 s watchdog loop):
  now a bounded wait; no lock leaves USB off with `USB: PLL didn't lock, USB off` in the log, and Ethernet runs on.

**Firmware update over Ethernet** (SIM build is enough; UART log on J4)
- [x] Good image (board 1, 2026-11-21): `make netdfu` with the real build onto the SIM build: 200, `update: staged`,
  the real build boots (no `SIMULATED` line) ~20 s after the upload began, the stage reads erased after. The web form
  (same endpoint, from a browser) not tried yet.
- [x] Wrong password 403, again within a second 429, no password set 403, `++net password` over TCP 1234 ignored
  (`update off` stays); the app keeps running (2026-11-21).
- [x] One byte of the `.dfu` changed, and a 4 KB non-image: 400 "nothing changed", no reset (2026-11-21).
- [x] Copy cut short (2026-11-21, a probe halt + reset 0.4 s after `update: resetting to install`, app header already
  erased, stage valid): the next boot copied again and started the new firmware. A real power cut takes the same
  path (it doesn't use the flag). Cut at 0.15 s, before the copy began (both headers intact), the old app booted
  and the stage stayed unused: the flag is cleared at entry, so the update is dropped, not retried (safe; send it
  again). A USB DFU download onto a board with such a staged but uninstalled image: the stage reads erased after,
  the downloaded app runs (2026-11-21). A real power cut mid-copy isn't tested: pulling the cable during an update
  isn't a supported use (user, 2026-11-21), and the reset above takes the same recovery path.
- [x] Flash programming in the app with USB and SysTick interrupts on: every upload above staged and verified.
