#!/usr/bin/env python3
"""First flash of a new board over SWD: dapboot and the release app, read back and checked. Needs pyocd's interpreter:
  ~/.local/pipx/venvs/pyocd/bin/python tools/flash_board.py [--force]

Stops if the chip isn't blank, so a board that wasn't swapped isn't flashed twice; --force overwrites and wipes settings.
"""
import subprocess
import sys
import time
from pathlib import Path

from pyocd.core.helpers import ConnectHelper

ROOT = Path(__file__).resolve().parent.parent
IMAGES = (("dapboot", ROOT / "bootloader/dapboot/src/dapboot.bin", 0x08000000),
          ("app", ROOT / "Firmware/build/usbgpib.bin", 0x08002000))
SETTINGS = 0x0800F800


def session():
    return ConnectHelper.session_with_chosen_probe(target_override="stm32f103c8", connect_mode="attach",
                                                   options={"reset_type": "sw"})


def run(*cmd, **kw):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)


def inspect():
    with session() as s:
        t = s.target
        uid = "".join(f"{t.read32(0x1FFFF7E8 + i):08x}" for i in (0, 4, 8))
        blank = all(t.read32(a) == 0xFFFFFFFF for a in (0x08000000, 0x08002000, SETTINGS))
    return uid, blank


def checks():
    bad = []

    def check(name, ok, detail=""):
        print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail != "" else ""))
        if not ok:
            bad.append(name)

    with session() as s:
        t = s.target
        for name, path, addr in IMAGES:
            image = path.read_bytes()
            check(f"{name} reads back", bytes(t.read_memory_block8(addr, len(image))) == image, f"{len(image)} bytes")
        check("settings blank", t.read32(SETTINGS) == 0xFFFFFFFF)
        time.sleep(0.5)
        cr, cfgr = t.read32(0x40021000), t.read32(0x40021004)
        check("16 MHz crystal, 72 MHz PLL", (cr >> 17) & 1 and (cfgr >> 2) & 3 == 2 and (cfgr >> 17) & 1 and ((cfgr >> 18) & 15) + 2 == 9)
        check("app running behind dapboot", t.read32(0xE000ED08) == 0x08002000 and t.get_state().name == "RUNNING")
        check("no watchdog reset", not t.read32(0x40021024) >> 29 & 1)
        last, edges, t0 = None, [], time.time()
        while time.time() - t0 < 1.6:
            v = (t.read32(0x4001100C) >> 13) & 1
            if v != last:
                edges.append(time.time() - t0)
                last = v
        gaps = [b - a for a, b in zip(edges[1:], edges[2:])]
        check("LED pin toggles 250/250 ms", gaps and all(0.2 < g < 0.3 for g in gaps), f"{len(gaps)} gaps")
    return bad


def main():
    uid, blank = inspect()
    print(f"UID {uid}, {'blank' if blank else 'NOT blank'}")
    if not blank and "--force" not in sys.argv:
        sys.exit("not blank: wrong board? --force overwrites it and wipes its settings")
    if not blank:
        run("pyocd", "erase", "--chip", "-t", "stm32f103c8")
    fw = ROOT / "Firmware"
    run("make", "-C", str(ROOT / "bootloader/dapboot/src"), "clean")  # dapboot doesn't track HSE_MHZ
    run("make", "clean", cwd=fw)
    run("make", "-j8", "flash-all", "PROG=pyocd", cwd=fw)
    bad = checks()
    r = subprocess.run([sys.executable, str(ROOT / "tools/gpib_shorts.py")], capture_output=True, text=True)
    print("[" + ("PASS" if r.returncode == 0 else "FAIL") + "] " + r.stdout.strip().splitlines()[-1])
    bad += [] if r.returncode == 0 else ["bridges"]
    print(f"UID {uid}: " + ("flashed, all checks pass" if not bad else f"FAILED: {', '.join(bad)}"))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
