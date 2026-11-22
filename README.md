# GPIB-Adapters

GPIB (IEEE-488) adapters that plug straight into an instrument's GPIB socket.

| Folder | What it is |
|---|---|
| [gpib-core](gpib-core) | Shared, board-independent firmware (descended from xyphro's UsbGpib) |
| [LANGPIB-WCH](LANGPIB-WCH) | GPIB to Ethernet/USB on the WCH CH32V407 (on-chip PHY), USB-powered |
| [USBGPIB-STM32](USBGPIB-STM32) | USB to GPIB on an STM32F1: USBTMC and Prologix-compatible serial |

## Build

Clone with `git clone --recurse-submodules`. Each board's firmware builds against `../../gpib-core`.

```
make -C gpib-core test             # host tests
make -C USBGPIB-STM32/Firmware     # arm-none-eabi-gcc
make -C LANGPIB-WCH/Firmware       # riscv-none-elf-gcc (xPack)
```

Flashing and setup are in each board's README and `docs/firmware.md`.

## License

[MIT](LICENSE)
