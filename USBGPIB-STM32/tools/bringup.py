#!/usr/bin/env python3
"""Bring-up tests for a bare STM32F103 on an ST-Link (Firmware/bringup/, `make -C Firmware flash-bringup PROG=pyocd`).

  bringup.py test          run every check, print a pass/fail summary
  bringup.py test --no-pins  only checks that drive no pins (a board that isn't ours: no contention)
  bringup.py term          interactive Prologix console over RTT (Ctrl-C quits)
  bringup.py send '++ver'  send lines, print what comes back

Needs pyocd: run with its interpreter, e.g. ~/.local/pipx/venvs/pyocd/bin/python tools/bringup.py test
"""
import subprocess
import sys
import threading
import time
from pathlib import Path

from pyocd.core.helpers import ConnectHelper

ELF = Path(__file__).resolve().parent.parent / "Firmware/build/bringup/bringup.elf"
TARGET = "stm32f103c8"
MAGIC = 0xB007B007
CMD_HANG, CMD_BOOTLOADER, CMD_PINTEST, CMD_TIMING, CMD_RESET, CMD_PINS = 1, 2, 3, 4, 5, 6
# offsets in `bringup` (bringup.c)
F = dict(magic=0, sysclk=4, hse_ok=8, reset_csr=12, bkp_dr1=16, loops=20, cmd=24, pins_fail=28, data_fail=32,
         delay_100us=36, task_idle=40, data_rw=44, pins_on=48)
IWDGRSTF, SFTRSTF = 1 << 29, 1 << 28


def symbols():
    out = subprocess.run(["arm-none-eabi-nm", str(ELF)], capture_output=True, text=True, check=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


class Board:
    def __init__(self, session):
        self.t = session.target
        sym = symbols()
        self.bu, self.cb, self.millis = sym["bringup"], sym["s_cb"], sym["s_millis"]

    def field(self, name):
        return self.t.read32(self.bu + F[name])

    def cmd(self, c):
        self.t.write32(self.bu + F["cmd"], c)

    def wait_boot(self, timeout=10.0):  # includes the 1 s debug window
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.field("magic") == MAGIC:
                    return True
            except Exception:
                pass
            time.sleep(0.05)
        return False

    def reset(self):
        self.t.reset()
        return self.wait_boot()

    # RTT rings: up at cb+24, down at cb+48; each name, buf, size, wr, rd, flags
    def _ring(self, off):
        name, buf, size, wr, rd, flags = self.t.read_memory_block32(self.cb + off, 6)
        return buf, size, wr, rd

    def rtt_read(self):
        buf, size, wr, rd = self._ring(24)
        if wr == rd:
            return b""
        if wr > rd:
            data = bytes(self.t.read_memory_block8(buf + rd, wr - rd))
        else:
            data = bytes(self.t.read_memory_block8(buf + rd, size - rd)) + bytes(self.t.read_memory_block8(buf, wr))
        self.t.write32(self.cb + 24 + 16, wr)
        return data

    def rtt_write(self, data):
        while data:
            buf, size, wr, rd = self._ring(48)
            room = (rd - wr - 1) % size
            n = min(room, len(data), size - wr)
            if n == 0:
                time.sleep(0.01)
                continue
            self.t.write_memory_block8(buf + wr, list(data[:n]))
            self.t.write32(self.cb + 48 + 12, (wr + n) % size)
            data = data[n:]

    def send(self, line):
        self.rtt_write(line.encode() + b"\n")
        time.sleep(0.05)

    def query(self, line, timeout=2.0):
        """Send a line, return what comes back up to the first line end (or all of it at the timeout)"""
        self.rtt_read()
        self.rtt_write(line.encode() + b"\n")
        out, end = b"", time.time() + timeout
        while time.time() < end and b"\n" not in out:
            out += self.rtt_read()
            time.sleep(0.01)
        return out.decode(errors="replace")

    def run_cmd(self, c, timeout=2.0):
        self.rtt_read()
        self.cmd(c)
        end = time.time() + timeout
        while self.field("cmd") and time.time() < end:
            time.sleep(0.02)
        time.sleep(0.05)
        return self.rtt_read().decode(errors="replace")


def connect():
    return ConnectHelper.session_with_chosen_probe(target_override=TARGET, connect_mode="attach",
                                                   options={"reset_type": "sw"})


def run_tests(b, pins=True):
    results = []

    def check(name, ok, detail=""):
        results.append((name, ok))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))

    check("boots after reset", b.reset())
    time.sleep(0.2)
    print(b.rtt_read().decode(errors="replace").strip())
    hse = b.field("hse_ok")
    check("clock: 72 MHz on HSE, else 64 MHz on HSI", b.field("sysclk") == (72_000_000 if hse else 64_000_000),
          f"sysclk {b.field('sysclk')}, hse_ok {hse}")

    m0, t0 = b.t.read32(b.millis), time.time()
    time.sleep(3.0)
    m1, t1 = b.t.read32(b.millis), time.time()
    err = ((m1 - m0) / ((t1 - t0) * 1000) - 1) * 100
    check("SysTick rate vs host clock (catches a wrong HSE_MHZ)", abs(err) < 3, f"{err:+.2f}%")

    if pins:
        print(b.run_cmd(CMD_PINS).rstrip())
        print(b.run_cmd(CMD_PINTEST).rstrip())
        check("GPIB pins drive low (JTAG off, SWD kept)", b.field("pins_fail") == 0,
              f"mask 0x{b.field('pins_fail'):x}")
        check("DIO bit-to-pin mapping", b.field("data_fail") == 0, f"mask 0x{b.field('data_fail'):x}")

    print(b.run_cmd(CMD_TIMING).rstrip())
    d = b.field("delay_100us")
    ideal = b.field("sysclk") // 10_000
    check("board_delay_us(100) accuracy", ideal <= d < ideal * 1.05, f"{d} cycles, ideal {ideal}")

    v = b.query("++ver")
    check("Prologix ++ver over RTT", "Prologix" in v, v.strip())
    b.send("++savecfg 0")
    b.send("++addr 12")
    a = b.query("++addr").strip()
    check("++addr set/read", a == "12", a)
    h = b.query("++help")
    check("++help", "addr" in h, h.strip()[:40])

    # No transceivers: the lines float, so a write waits out its listener timeout (WRITE_TMO 10 s) or sees no
    # listener. Checks that it ends, reports the error, and later commands still run.
    t = time.time()
    b.send("*IDN?")
    e = b.query("++err", 15).strip()
    check("write with no bus ends and reports it", e.startswith(("timeout", "no listener")),
          f"++err {e!r} after {time.time() - t:.1f}s")
    t = time.time()
    b.send("++read eoi")
    e = b.query("++err", 15).strip().splitlines()[-1:] or [""]
    check("read with no bus ends and reports it", e[0].startswith(("timeout", "no listener", "none")),
          f"++err {e[0]!r} after {time.time() - t:.1f}s")
    t = time.time()
    s = b.query("++scan", 15)
    check("++scan with no bus returns", "\n" in s, f"{s.strip()!r} after {time.time() - t:.1f}s")

    b.send("++savecfg 1")
    b.send("++addr 9")
    time.sleep(0.5)  # SETTLE_MS, then the flash write
    b.reset()
    a = b.query("++addr").strip()
    check("settings survive a reset (flash log)", a == "9", a)
    if pins:
        b.run_cmd(CMD_PINS)
    # 10 records: the log (2 pages x 4 slots) wraps, so a page erase happens with the newest record in the other page.
    # Automatic writes are 60 s apart, so each record is forced by board_reset(), which flushes.
    for n in range(10):
        b.send(f"++addr {n + 3}")
        b.cmd(CMD_RESET)
        time.sleep(0.1)
        b.wait_boot()
    a = b.query("++addr").strip()
    check("settings log wraps across both flash pages", a == "12", a)
    b.send("++addr 1")
    time.sleep(0.5)

    l1 = b.field("loops")
    b.t.halt()
    time.sleep(6)
    b.t.resume()
    time.sleep(0.05)
    l2 = b.field("loops")
    check("watchdog paused while halted (DBG_IWDG_STOP)", l2 > l1, f"loops {l1} -> {l2}")

    b.rtt_read()
    b.cmd(CMD_HANG)
    time.sleep(6)
    ok = b.wait_boot(3) and bool(b.field("reset_csr") & IWDGRSTF)
    check("watchdog resets a hung main loop", ok, f"RCC_CSR 0x{b.field('reset_csr'):08x}")

    if not pins:  # board_enter_bootloader() drives PA12 low (USB detach)
        return report(results)
    b.cmd(CMD_BOOTLOADER)
    time.sleep(1.5)
    ok = b.wait_boot(3) and b.field("bkp_dr1") == 0x4F42 and bool(b.field("reset_csr") & SFTRSTF)
    check("enter-bootloader sets BKP DR1 = 0x4F42 and resets", ok,
          f"DR1 0x{b.field('bkp_dr1'):04x}, RCC_CSR 0x{b.field('reset_csr'):08x}")

    return report(results)


def report(results):
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed" + (f"; failed: {', '.join(failed)}" if failed else ""))
    return not failed


def term(b):
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            out = b.rtt_read()
            if out:
                sys.stdout.write(out.decode(errors="replace"))
                sys.stdout.flush()
            time.sleep(0.02)

    threading.Thread(target=reader, daemon=True).start()
    try:
        for line in sys.stdin:
            b.rtt_write(line.encode())
    except KeyboardInterrupt:
        pass
    stop.set()


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "test"
    with connect() as session:
        b = Board(session)
        if not b.wait_boot(4):
            sys.exit("bring-up firmware not running (make -C Firmware flash-bringup PROG=pyocd)")
        if mode == "test":
            sys.exit(0 if run_tests(b, "--no-pins" not in sys.argv) else 1)
        if mode in ("term", "send") and "--no-pins" not in sys.argv:
            b.run_cmd(CMD_PINS)
        if mode == "term":
            term(b)
        elif mode == "send":
            for line in [a for a in sys.argv[2:] if a != "--no-pins"]:
                print(b.query(line, 3.0), end="")
        else:
            sys.exit(__doc__)


if __name__ == "__main__":
    main()
