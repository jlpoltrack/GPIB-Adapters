#!/usr/bin/env python3
"""Recover an STM32F1 whose firmware locks out SWD, when NRST isn't wired to the probe and BOOT0 is tied low.

Start this, then either power-cycle the board (the bring-up image waits 1 s at reset before touching any pin) or hold
reset low for a few seconds and release it. The script keeps trying to attach; once in, it halts the core (DHCSR
C_HALT + DEMCR VC_CORERESET, so a reset also stops at the vector) and mass-erases the flash.
With NRST wired to the probe, `pyocd erase --chip -M under-reset` does the same in one step.

  ~/.local/pipx/venvs/pyocd/bin/python tools/swd_rescue.py [seconds]
"""
import logging
import sys
import time

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.eraser import FlashEraser

DHCSR, DEMCR = 0xE000EDF0, 0xE000EDFC
S_HALT = 1 << 17
logging.disable(logging.CRITICAL)


def try_once():
    session = ConnectHelper.session_with_chosen_probe(target_override="stm32f103c8", connect_mode="attach",
                                                      options={"frequency": 400000})
    session.open()
    return session


def main():
    window = float(sys.argv[1]) if len(sys.argv) > 1 else 180
    end = time.time() + window
    print(f"trying for {window:.0f} s: hold reset low now, release a few seconds after it says connected", flush=True)
    session = None
    while time.time() < end and session is None:
        try:
            session = try_once()
        except Exception:
            time.sleep(0.05)
    if session is None:
        sys.exit("no connection")
    t = session.target
    t.write32(DHCSR, 0xA05F0003)  # debug enable + halt
    t.write32(DEMCR, t.read32(DEMCR) | 1)  # VC_CORERESET
    print("connected; halt requested. Release reset now if still holding it.", flush=True)
    while time.time() < end and not (t.read32(DHCSR) & S_HALT):
        time.sleep(0.1)
    if not t.read32(DHCSR) & S_HALT:
        sys.exit("core never halted")
    print(f"halted at pc 0x{t.read_core_register('pc'):08x}; mass-erasing", flush=True)
    FlashEraser(session, FlashEraser.Mode.CHIP).erase()
    print("flash first words:", [hex(w) for w in t.read_memory_block32(0x08000000, 4)], flush=True)
    t.write32(DEMCR, t.read32(DEMCR) & ~1)
    session.close()
    print("done: SWD recovered", flush=True)


if __name__ == "__main__":
    main()
