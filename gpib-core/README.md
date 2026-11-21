# gpib-core

The board-independent firmware of the GPIB adapters, shared by
[USBGPIB-STM32](../USBGPIB-STM32) and [LANGPIB-WCH](../LANGPIB-WCH) (`GPIB_CORE := ../../gpib-core` in their Makefiles). Descended from
[xyphro's UsbGpib](https://github.com/xyphro/UsbGpib) (MIT).

| File | What |
|---|---|
| `gpib_phy`, `gpib_bus` | IEEE 488 handshake; controller operations as non-blocking jobs, shared by the front ends |
| `instr` | the instrument: start-up auto-ID, ID string, read termination, `!` commands (`miniparser`) |
| `usbtmc`, `usb_descriptors`, `cdc_port` | USBTMC/USB488 class driver and composite descriptors on TinyUSB |
| `prologix` | Prologix-compatible `++` protocol on a byte stream (`port.h`), one instance per port (CDC, TCP 1234) |
| `scpi_raw` | raw SCPI socket protocol (TCP 5025): lines to the instrument, a query's reply back |
| `vxi11`, `xdr.h` | VXI-11 server (portmapper, core, abort) as a transport-independent protocol layer |
| `vxi11_lwip`, `sockets_lwip` | VXI-11, and raw SCPI 5025 + Prologix 1234 (one client each) + HTTP 80 (three), on lwIP raw TCP |
| `settings`, `trace` | CRC-checked settings log in flash; GPIB bus trace |
| `netcfg` | network settings (DHCP / static, hostname, update password from USB only): `!net` and `++net`; the board's network code reads them |
| `http` | web page (`/`), LXI identification (`/lxi/identification`, schema 1.0), a network settings form and, with the board's `board_update_*()` hooks, a firmware upload (`POST /update`); on any `port_t` |
| `mdns_lwip` | mDNS on lwIP: `<hostname>.local`, `_vxi-11._tcp`, `_scpi-raw._tcp`, `_lxi._tcp`, `_http._tcp` (with `LWIP_MDNS_RESPONDER`) |

## What a board provides

In its own `src/`, ahead of `gpib-core/src` on the include path:

- `board_hw.h`: device header, `BOARD_FLASH_PAGE_SIZE`, inline `board_watchdog_feed()`, and `BOARD_FLASH_ERASED` if an
  erased flash word doesn't read 0xFFFFFFFF (WCH: 0xE339E339); included by `board.h`.
- `board.c`: the functions declared in [src/board.h](src/board.h).
- `gpib_pins.h`: the GPIB line macros and `gpib_pins_init()` used by `gpib_phy.c`.
- `app_config.h`: `APP_USB_VID/PID/BCD`, `APP_USB_MANUFACTURER`, `APP_USB_PRODUCT`, `APP_PROLOGIX_VERSION`.
- `tusb_config.h`, `main.c` (main loop, USB interrupt), linker script, startup.

The board Makefile includes `gpib-core.mk` and adds `$(GPIB_CORE_SRC)` (plus `$(GPIB_CORE_VXI_SRC)` and
`$(GPIB_CORE_LWIP_SRC)` with Ethernet: the board's network code calls `vxi11_lwip_*()`, `sockets_lwip_*()` and
`mdns_lwip_*()`, applies `settings.net_*` at start-up and provides `board_net_status()` for the `net` query).

## Tests

```
make test   # host unit tests on a simulated bus: settings, bus, Prologix, instr, VXI-11
make host   # build/host/vxi11_host: VXI-11, raw SCPI 5025, Prologix 1234, web page on 8080, a simulated instrument
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
build/host/vxi11_host & .venv/bin/python tools/vxi11_check.py   # PyVISA-py + python-vxi11 checks
.venv/bin/python tools/sockets_check.py                          # raw SCPI and Prologix over TCP
```

Builds use `-Wall -Wextra -Werror`, nothing suppressed.

## Workflow

Fix or extend here, run `make test`, then build each board's firmware and commit them together.

## License

[MIT](../LICENSE). Derived from UsbGpib by Kai Gossner (MIT).
