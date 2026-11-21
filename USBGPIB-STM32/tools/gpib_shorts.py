#!/usr/bin/env python3
"""Solder-bridge test of the 16 GPIB lines over SWD. Flashes nothing: it works on whatever firmware is on the chip.

Run it after soldering J2 and before plugging into an instrument, with nothing on the GPIB connector:
  ~/.local/pipx/venvs/pyocd/bin/python tools/gpib_shorts.py

The core is held in reset state and the registers are driven from the probe. Every line gets its internal pull-up;
one line at a time is then pulled low and all the others are read. Finds a line stuck low (a bridge to GND, J2 has 8
of those) and a bridge between two lines. It can't see a line that is open: only an instrument shows that.
"""
import sys
import time

from pyocd.core.helpers import ConnectHelper

GPIOA, GPIOB = 0x40010800, 0x40010C00
CRL, CRH, IDR, ODR = 0x00, 0x04, 0x08, 0x0C
RCC_APB2ENR, AFIO_MAPR = 0x40021018, 0x40010004
IOPA_IOPB_AFIO = (1 << 2) | (1 << 3) | (1 << 0)
SWJ_JTAG_OFF = 2 << 24  # SWD stays on, PA15/PB3/PB4 become GPIOs
PULL_INPUT, OD_OUTPUT = 0x8, 0x7

LINES = {
    "DIO1": (GPIOB, 9), "DIO2": (GPIOB, 7), "DIO3": (GPIOB, 10), "DIO4": (GPIOB, 11),
    "DIO5": (GPIOB, 8), "DIO6": (GPIOB, 6), "DIO7": (GPIOB, 4), "DIO8": (GPIOB, 3),
    "EOI": (GPIOB, 12), "DAV": (GPIOB, 13), "NRFD": (GPIOB, 14), "NDAC": (GPIOB, 15),
    "IFC": (GPIOA, 8), "SRQ": (GPIOA, 9), "ATN": (GPIOA, 10), "REN": (GPIOA, 15),
}


def mode(t, port, pin, nibble):
    reg = port + (CRH if pin >= 8 else CRL)
    shift = (pin % 8) * 4
    t.write32(reg, (t.read32(reg) & ~(0xF << shift)) | (nibble << shift))


def odr_bit(t, port, pin, high):
    reg = port + ODR
    t.write32(reg, (t.read32(reg) & ~(1 << pin)) | (int(high) << pin))


def reads_high(t):
    ports = {p: t.read32(p + IDR) for p in (GPIOA, GPIOB)}
    return {name: bool(ports[p] >> n & 1) for name, (p, n) in LINES.items()}


def run(t):
    t.reset_and_halt()
    t.write32(RCC_APB2ENR, t.read32(RCC_APB2ENR) | IOPA_IOPB_AFIO)
    t.write32(AFIO_MAPR, (t.read32(AFIO_MAPR) & ~(7 << 24)) | SWJ_JTAG_OFF)
    for port, pin in LINES.values():
        odr_bit(t, port, pin, True)
        mode(t, port, pin, PULL_INPUT)
    time.sleep(0.01)

    faults = []
    stuck = [n for n, high in reads_high(t).items() if not high]
    faults += [f"{n} stuck low (bridge to GND, or something on the bus)" for n in stuck]

    for name, (port, pin) in LINES.items():
        if name in stuck:
            continue
        odr_bit(t, port, pin, False)
        mode(t, port, pin, OD_OUTPUT)
        time.sleep(0.002)
        state = reads_high(t)
        if state[name]:
            faults.append(f"{name} does not go low when driven")
        faults += [f"{name} - {o} bridged" for o, high in state.items() if o != name and o not in stuck and not high
                   and f"{o} - {name} bridged" not in faults]
        mode(t, port, pin, PULL_INPUT)
        odr_bit(t, port, pin, True)
        time.sleep(0.001)
    return faults


def main():
    with ConnectHelper.session_with_chosen_probe(target_override="stm32f103c8", connect_mode="attach",
                                                 options={"reset_type": "sw"}) as session:
        faults = run(session.target)
        session.target.reset()  # the firmware starts over and sets the pins up itself
    for f in faults:
        print("FAIL", f)
    print(f"{len(LINES)} lines checked: " + (f"{len(faults)} fault(s)" if faults else "no bridges, none stuck low"))
    sys.exit(1 if faults else 0)


if __name__ == "__main__":
    main()
