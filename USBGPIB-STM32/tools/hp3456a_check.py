#!/usr/bin/env python3
"""End-to-end adapter check against an HP 3456A (commands: ../HP3456A-7.5/docs/GPIB_COMMANDS.md).

Needs the release firmware, with the meter on the bus and powered. Leaves the meter at power-on defaults (H).
  python tools/hp3456a_check.py [--addr 6] [--tmc-only] [--cdc-only] [--port /dev/cu.usbmodemNNN]
Needs pyusb, libusb-package and pyserial.
"""
import glob
import re
import struct
import sys
import time

import libusb_package
import serial
import usb.core
import usb.util

READING = re.compile(r"^[+-][\d.]{8,9}E[+-]\d\r\n$")  # the decimal point moves with the range
CONVERT = 0.7  # after a trigger: the meter hands over its previous reading until the new one is done
def arg(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None


addr = int(arg("--addr") or 6)
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail != "" else ""))


class Cdc:
    def __init__(self):
        port = arg("--port") or next(p for p in glob.glob("/dev/cu.usbmodem*") if not p.endswith("21102"))
        self.s = serial.Serial(port, 115200, timeout=0.05)

    def send(self, line):
        self.s.write(line.encode() + b"\n")

    def q(self, line, wait=3.0):
        """Send, return everything up to the first CR LF"""
        self.s.reset_input_buffer()
        self.send(line)
        end, out = time.time() + wait, b""
        while time.time() < end:
            out += self.s.read(256)
            if out.endswith(b"\n"):
                break
        return out.decode(errors="replace")


def cdc_tests(c):
    c.q("++addr %d" % addr)
    c.send("++auto 0")
    c.send("++read_tmo_ms 3000")
    c.send("++eos 3")  # the meter takes commands with no terminator
    c.send("++eoi 1")
    check("++scan finds the meter", str(addr) in c.q("++scan", 5).split())

    c.send("H")
    time.sleep(1.5)
    c.send("F1R1T1")
    time.sleep(1)
    r = c.q("++read 10", 5)
    check("DCV free-run reading, format", READING.match(r), repr(r))

    c.send("T4")  # hold: from here every reading comes from a trigger
    time.sleep(0.5)
    old = c.q("++read 10", 5)
    c.send("T3")
    time.sleep(CONVERT)
    r = c.q("++read 10", 5)
    check("single trigger T3", READING.match(r) and r != old, f"{old!r} -> {r!r}")

    t0 = time.time()
    r = c.q("++read 10", 4)  # Prologix semantics: a read timeout ends the message, it is not an error
    dt = time.time() - t0
    check("a read from a silent meter ends at ++read_tmo_ms", r == "" and 2.8 < dt and "Prologix" in c.q("++ver"), f"{dt:.1f}s")
    c.send("++clr")
    time.sleep(0.3)
    c.send("F1R4T4")
    time.sleep(0.3)
    c.send("++trg %d" % addr)
    time.sleep(CONVERT)
    r = c.q("++read 10", 5)
    check("bus trigger ++trg (GET)", READING.match(r), repr(r))

    c.send("7.25STY")
    c.send("REY")
    time.sleep(0.3)
    r = c.q("++read 10", 5)
    check("store and recall a register", READING.match(r) and float(r) == 7.25, repr(r))

    c.send("F4R1T4")
    time.sleep(0.3)
    c.send("T3")
    time.sleep(3)
    r = c.q("++read 10", 8)
    check("2-wire ohms reading (open input overloads)", READING.match(r) and "+9" in r, repr(r))

    c.send("F1R1SM020T4")  # SRQ on a syntax error
    time.sleep(0.3)
    c.q("++spoll", 1)
    c.send("QQ9")
    time.sleep(0.5)
    srq = c.q("++srq").strip()
    b = c.q("++spoll").strip()
    check("SRQ on a syntax error, ++srq and ++spoll", srq == "1" and b.isdigit() and (int(b) & 0x50) == 0x50, f"srq {srq}, stb {b}")
    check("SRQ clears after the poll", c.q("++srq").strip() == "0")

    c.send("SM004")  # SRQ on data ready, then a poll while the reading waits
    time.sleep(0.2)
    c.send("T3")
    time.sleep(0.8)
    b = c.q("++spoll").strip()
    r = c.q("++read 10", 5)
    check("SRQ on data ready, reading follows the poll", b.isdigit() and (int(b) & 0x44) == 0x44 and READING.match(r),
          f"stb {b}, {r!r}")
    c.send("SM000")

    c.send("0STL0STUSM200M1T3")  # any reading fails a zero window: status 128 is the only byte that carries DIO8
    time.sleep(1.5)
    b = c.q("++spoll").strip()
    check("limit-fail SRQ, status bit 128 (DIO8)", b.isdigit() and (int(b) & 0xC0) == 0xC0, f"stb {b}")
    c.send("SM000M0O1T3")
    time.sleep(CONVERT)
    r = c.q("++read eoi", 5)
    check("read to EOI (O1)", READING.match(r), repr(r))
    c.send("O0")

    c.send("10STI7STGT3")  # the 7.5-digit output is untested in HP3456A-7.5 (test 15)
    time.sleep(1.5)
    r = c.q("++read 10", 6)
    d = re.sub(r"[^0-9]", "", r.split("E")[0])
    check("7.5-digit output: 7 digits plus the 7th at 10 PLC", READING.match(r) and len(d) == 8, repr(r))

    c.send("H")
    time.sleep(1.5)
    c.send("F1R4Z0T1")  # free run, fastest the meter goes: each read is a full handshake of 14 bytes
    c.send(".01STI")
    c.send("3STG")
    time.sleep(0.5)
    bad, t0, n = 0, time.time(), 200
    for _ in range(n):
        if not READING.match(c.q("++read 10", 3)):
            bad += 1
    dt = time.time() - t0
    check(f"{n} reads in a row with no error", bad == 0, f"{bad} bad, {n / dt:.1f}/s")

    c.send("++llo")
    c.send("++loc")
    c.send("H")
    time.sleep(1.5)
    check("no lasting bus error", "Prologix" in c.q("++ver"))


class Tmc:
    def __init__(self):
        self.d = usb.core.find(idVendor=0x1209, idProduct=1, backend=libusb_package.get_libusb1_backend())
        eps = usb.util.find_descriptor(self.d.get_active_configuration(), bInterfaceClass=0xFE)
        m = {(e.bEndpointAddress & 0x80, e.bmAttributes & 3): e.bEndpointAddress for e in eps}
        self.o, self.i, self.n = m[(0, 2)], m[(0x80, 2)], m[(0x80, 3)]
        usb.util.claim_interface(self.d, 0)
        self.tag = 0

    def next(self):
        self.tag = self.tag % 127 + 1
        return self.tag

    def write(self, text, timeout=3000):
        t, b = self.next(), text.encode()
        self.d.write(self.o, struct.pack("<BBBBIB3x", 1, t, ~t & 255, 0, len(b), 1) + b + b"\0" * (-len(b) % 4), timeout=timeout)

    def read(self, term=b"\n", timeout=4000):
        t = self.next()
        attr, tc = (2, term[0]) if term else (0, 0)
        self.d.write(self.o, struct.pack("<BBBBIBBxx", 2, t, ~t & 255, 0, 1024, attr, tc), timeout=2000)
        raw = bytes(self.d.read(self.i, 2048, timeout=timeout))
        size = struct.unpack("<I", raw[4:8])[0]
        return raw[12:12 + size].decode(errors="replace")

    def ctrl(self, req, val, n):
        return bytes(self.d.ctrl_transfer(0xA1, req, val, 0, n, timeout=3000))

    def stb(self):
        tag = self.next() + 1
        r = self.ctrl(128, tag, 3)
        n = bytes(self.d.read(self.n, 2, timeout=2000))
        return r[0], n[1], n[0] == (0x80 | tag)


def tmc_tests(t):
    while True:
        try:
            t.d.read(t.n, 2, timeout=100)
        except usb.core.USBError:
            break
    t.write("H")
    time.sleep(1.5)
    t.write("F1R1T4")
    time.sleep(0.3)
    t.write("T3")
    time.sleep(CONVERT)
    r = t.read()
    check("USBTMC write and read", READING.match(r), repr(r))

    st, b, ok = t.stb()
    check("USBTMC READ_STATUS_BYTE from the meter", st == 1 and ok, f"stb {b}")

    t.write("SM004")
    time.sleep(0.2)
    t.write("T3")
    srq = None
    try:
        srq = bytes(t.d.read(t.n, 2, timeout=2500))
    except usb.core.USBError:
        pass
    check("USBTMC SRQ notification on interrupt IN", srq and srq[0] == 0x81 and (srq[1] & 0x44) == 0x44, srq and srq.hex())
    st, b, ok = t.stb()
    r = t.read()
    check("reading still there after the SRQ", READING.match(r), repr(r))
    t.write("SM000")

    t.d.write(t.o, struct.pack("<BBBBIxxxx", 128, (x := t.next()), ~x & 255, 0, 0), timeout=2000)  # TRIGGER message
    time.sleep(CONVERT)
    r = t.read()
    check("USBTMC TRIGGER message (GET)", READING.match(r), repr(r))

    x = t.next()  # what a VISA timeout does: hold, nothing to read, then abort
    t.d.write(t.o, struct.pack("<BBBBIBBxx", 2, x, ~x & 255, 0, 256, 2, 10), timeout=2000)
    time.sleep(0.5)
    a = bytes(t.d.ctrl_transfer(0xA2, 3, x, t.i, 2, timeout=2000))
    if a[0] == 1:
        for _ in range(100):
            c = bytes(t.d.ctrl_transfer(0xA2, 4, 0, t.i, 8, timeout=2000))
            if c[0] != 2:
                break
            try:
                t.d.read(t.i, 2048, timeout=50)
            except usb.core.USBError:
                pass
    try:
        t.d.read(t.i, 2048, timeout=100)
    except usb.core.USBError:
        pass
    t.write("T3")
    time.sleep(CONVERT)
    r = t.read()
    check("read abort, then the next query works", READING.match(r), f"abort {a.hex()}, {r!r}")

    c = t.ctrl(5, 0, 1)
    for _ in range(100):
        s = t.ctrl(6, 0, 2)
        if s[0] != 2:
            break
        time.sleep(0.02)
    time.sleep(0.5)
    t.write("F1R1T4")
    time.sleep(0.3)
    t.write("T3")
    time.sleep(CONVERT)
    r = t.read()
    check("viClear (SDC), then a query", c[0] == 1 and s[0] == 1 and READING.match(r), repr(r))

    for req, val in ((160, 0), (161, 0), (160, 1)):
        t.ctrl(req, val, 1)
    t.write("H")
    time.sleep(1.5)
    bad, t0, n = 0, time.time(), 100
    t.write("F1R4T1")
    time.sleep(0.5)
    for _ in range(n):
        if not READING.match(t.read()):
            bad += 1
    check(f"USBTMC: {n} reads in a row", bad == 0, f"{bad} bad, {n / (time.time() - t0):.1f}/s")
    t.write("H")


def main():
    if "--tmc-only" not in sys.argv:
        cdc_tests(Cdc())
    if "--cdc-only" not in sys.argv:
        t = Tmc()
        print(f"serial {t.d.serial_number}")
        tmc_tests(t)
    bad = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed" + (f"; failed: {', '.join(bad)}" if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
