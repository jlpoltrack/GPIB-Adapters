#!/usr/bin/env python3
"""End-to-end adapter check against an HP/Agilent 66332A, through real VISA (pyvisa-py) and the Prologix port.

It only queries: output state and setpoints are compared before and after, and the sweep size is put back.
  python tools/hp66332a_check.py [--addr 5] [--port /dev/cu.usbmodemNNN]
Needs pyvisa, pyvisa-py, pyusb, libusb-package and pyserial.
"""
import glob
import struct
import sys
import threading
import time

import pyvisa
import serial
import usb.core
import usb.util

from hp3456a_check import Tmc

def arg(name):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else None


addr = int(arg("--addr") or 5)
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail != "" else ""))


def state(d):
    return {q: d.query(q).strip() for q in ("OUTP?", "VOLT?", "CURR?", "CURR:PROT:STAT?", "VOLT:PROT?")}


def errors(d):
    out = []
    for _ in range(20):
        e = d.query("SYST:ERR?").strip()
        if e.startswith(("+0", "0")):
            return out
        out.append(e)
    return out


def array(d, points, what="VOLT"):
    d.write(f"SENS:SWE:POIN {points}")
    t0 = time.time()
    raw = d.query(f"MEAS:ARR:{what}?")
    dt = time.time() - t0
    vals = [float(x) for x in raw.strip().split(",")]
    return vals, len(raw), dt


def visa_tests(d):
    idn = d.query("*IDN?").strip()
    check("*IDN? through VISA", "66332A" in idn, idn)
    print(f"       before: {state(d)}, V {d.query('MEAS:VOLT?').strip()}, A {d.query('MEAS:CURR?').strip()}")
    errors(d)

    pts = d.query("SENS:SWE:POIN?").strip()
    d.timeout = 30000
    try:
        for n in (64, 512, 2048, 4096):
            vals, size, dt = array(d, n)
            check(f"long read, {n} points", len(vals) == n, f"{size} bytes, {size / dt / 1024:.1f} KB/s incl. acquisition")
    finally:
        d.write(f"SENS:SWE:POIN {int(float(pts))}")
    d.timeout = 5000

    for n in (40, 52, 56, 64, 116, 128, 1000, 3000):  # EOI too early would cut a command in two
        msg = "*CLS;" * (n // 5) + "*OPC?"
        r = d.query(msg).strip()
        e = errors(d)
        check(f"long write, {len(msg)} bytes", r.lstrip("+") == "1" and not e, e or r)

    bad, t0, n = 0, time.time(), 300
    for _ in range(n):
        if "66332A" not in d.query("*IDN?"):
            bad += 1
    check(f"{n} *IDN? in a row", bad == 0, f"{bad} bad, {n / (time.time() - t0):.0f}/s")

    d.timeout = 500
    t0 = time.time()
    try:
        d.read()
        check("VISA read timeout with nothing to read", False)
    except pyvisa.errors.VisaIOError as e:
        check("VISA read timeout with nothing to read", "TMO" in str(e), f"{time.time() - t0:.1f}s")
    d.timeout = 5000
    check("query works after the timeout", "66332A" in d.query("*IDN?"))

    unsupported(d.clear, "viClear")
    unsupported(d.assert_trigger, "viAssertTrigger")
    unsupported(d.read_stb, "viReadSTB")
    errors(d)


def unsupported(fn, name):
    """pyvisa-py leaves some VISA calls out: say so, and let the raw USB checks cover them"""
    try:
        fn()
        print(f"       {name}: ok through pyvisa-py")
    except (pyvisa.errors.VisaIOError, NotImplementedError) as e:
        print(f"       {name}: not in pyvisa-py ({type(e).__name__}), covered by the raw USB checks")


def status_tests():
    """READ_STATUS_BYTE and SRQ, which pyvisa-py can't do: the same USBTMC requests over pyusb"""
    t = Tmc()
    while True:
        try:
            t.d.read(t.n, 2, timeout=100)
        except usb.core.USBError:
            break
    st, b, ok = t.stb()
    check("READ_STATUS_BYTE from the supply", st == 1 and ok, f"stb {b}")
    t.write("*ESE?\n")
    ese = t.read().strip()
    t.write("*SRE?\n")
    sre = t.read().strip()
    t.write("*CLS;*ESE 1;*SRE 32;*OPC\n")  # operation complete -> ESB -> RQS
    n = bytes(t.d.read(t.n, 2, timeout=2500))
    check("SRQ notification after *OPC", n[0] == 0x81 and (n[1] & 0x60) == 0x60, n.hex())
    st, b, ok = t.stb()
    t.write("*ESR?\n")
    r = t.read().strip()
    check("the event register still holds it", r.lstrip("+").startswith("1"), f"*ESR? {r}")
    st, b, ok = t.stb()
    check("status clears after *ESR?", st == 1 and not b & 0x60, f"stb {b}")
    t.write(f"*ESE {ese};*SRE {sre}\n")
    time.sleep(0.2)

    t.write("SENS:SWE:POIN?\n")  # READ_STATUS_BYTE cuts into a read that waits for its first byte: the riskiest path
    pts = int(float(t.read()))
    t.write("SENS:SWE:POIN 512\n")
    t.write("MEAS:ARR:VOLT?\n")
    data, polls, eom = b"", 0, False
    while not eom:
        x = t.next()
        t.d.write(t.o, struct.pack("<BBBBIBBxx", 2, x, ~x & 255, 0, 1024, 0, 0), timeout=2000)
        st, b, ok = t.stb()
        polls += st == 1 and ok
        raw = bytes(t.d.read(t.i, 2048, timeout=6000))
        size = struct.unpack("<I", raw[4:8])[0]
        data += raw[12:12 + size]
        eom = bool(raw[8] & 1)
    vals = [float(v) for v in data.strip().split(b",")]
    check("long read with a status poll in every transfer", len(vals) == 512, f"{polls} polls, {len(data)} bytes")
    t.write(f"SENS:SWE:POIN {pts}\n")
    time.sleep(0.2)

    t.write("*IDN?\n")  # an answer left waiting: SDC must drop it
    time.sleep(0.2)
    c = t.ctrl(5, 0, 1)
    for _ in range(100):
        s = t.ctrl(6, 0, 2)
        if s[0] != 2:
            break
        time.sleep(0.02)
    time.sleep(0.2)
    t.write("*OPC?\n")
    r = t.read().strip()
    check("USBTMC clear drops a pending answer", c[0] == 1 and s[0] == 1 and r.lstrip("+") == "1", repr(r))

    x = t.next()
    t.d.write(t.o, struct.pack("<BBBBIxxxx", 128, x, ~x & 255, 0, 0), timeout=2000)  # TRIGGER message: GET on the bus
    time.sleep(0.2)
    t.write("SYST:ERR?\n")
    r = t.read().strip()
    check("USBTMC trigger reaches the supply (not armed, so ignored)", "-211" in r or r.startswith(("+0", "0")), r)
    while not t.write("SYST:ERR?\n") and not t.read().startswith(("+0", "0")):
        pass
    for req, val in ((160, 1), (161, 0)):  # REN on, then go to local
        t.ctrl(req, val, 1)
    time.sleep(0.2)
    usb.util.dispose_resources(t.d)


class Cdc:
    def __init__(self):
        port = arg("--port") or next(p for p in glob.glob("/dev/cu.usbmodem*") if not p.endswith("21102"))
        self.s = serial.Serial(port, 115200, timeout=0.02)

    def q(self, line, wait=5.0, end=b"\n"):
        self.s.reset_input_buffer()
        self.s.write(line.encode() + b"\n")
        stop, out = time.time() + wait, b""
        while time.time() < stop:
            out += self.s.read(4096)
            if out.endswith(end):
                break
        return out.decode(errors="replace")


def cdc_tests(c):
    c.q("++addr %d" % addr, 0.1)
    c.s.write(b"++auto 1\n++eos 2\n++eoi 1\n++read_tmo_ms 3000\n")
    time.sleep(0.2)
    check("++scan", str(addr) in c.q("++scan", 5).split())
    r = c.q("*IDN?")
    check("Prologix ++auto 1 query", "66332A" in r, r.strip())

    pts = c.q("SENS:SWE:POIN?").strip()
    c.s.write(b"SENS:SWE:POIN 2048\n")
    time.sleep(0.2)
    t0 = time.time()
    raw = c.q("MEAS:ARR:VOLT?", 30)
    vals = [x for x in raw.strip().split(",") if x]
    check("Prologix long read, 2048 points", len(vals) == 2048, f"{len(raw)} bytes, {time.time() - t0:.1f}s")
    c.s.write(b"++auto 0\n")  # first: with auto 1 the next command starts a read that holds the bus 3 s
    time.sleep(0.2)
    c.s.write(f"SENS:SWE:POIN {int(float(pts))}\n".encode())
    time.sleep(0.2)


def both_at_once(d, c):
    """Different traffic on each side: two query streams to one instrument would collide in its output queue"""
    fails = []

    def tmc():
        for i in range(200):
            try:
                if "66332A" not in d.query("*IDN?"):
                    fails.append("tmc")
            except (ValueError, pyvisa.errors.VisaIOError) as e:  # a dead thread must not read as a pass
                fails.append(f"tmc #{i} {e}")
                return

    def cdc():
        for i in range(200):
            r = c.q("++spoll" if i % 2 else "++ver", 3).strip()
            if not (r.isdigit() if i % 2 else "Prologix" in r):
                fails.append(f"cdc #{i} {r!r}")

    ts = [threading.Thread(target=f) for f in (tmc, cdc)]
    t0 = time.time()
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    check("USBTMC queries with Prologix polls at once, 200 each", not fails, f"{len(fails)} bad {fails[:2]}, {time.time() - t0:.1f}s")


def main():
    rm = pyvisa.ResourceManager("@py")
    res = [r for r in rm.list_resources("USB?*") if "0x1209" in r.lower() or "4617" in r]
    check("VISA finds the adapter", res, res)
    d = rm.open_resource(res[0], read_termination="\n", write_termination="\n", timeout=5000)
    before = state(d)
    visa_tests(d)
    after = state(d)
    d.close()

    status_tests()
    c = Cdc()
    cdc_tests(c)
    d = rm.open_resource(res[0], read_termination="\n", write_termination="\n", timeout=5000)
    both_at_once(d, c)
    after = state(d)
    unsupported(lambda: d.control_ren(pyvisa.constants.RENLineOperation.address_gtl), "go to local")
    d.close()
    check("output state and setpoints unchanged", before == after, after)

    bad = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed" + (f"; failed: {', '.join(bad)}" if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
