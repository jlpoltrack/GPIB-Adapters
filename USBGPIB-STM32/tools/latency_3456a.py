#!/usr/bin/env python3
"""Per-transfer latency of the adapter with an HP 3456A: where the time of one triggered reading goes.

Times every USB transfer of the USBTMC path (OUT message, READ_STATUS_BYTE control and interrupt, IN request,
bulk IN) with the meter's display on (D1) and off (D0), and the same reading over the Prologix serial port.
"T3 done on bus" is when a second, empty message is accepted: the adapter takes it once the T3 job has ended.
  python tools/latency_3456a.py [--reps 20] [--no-cdc] [--port /dev/cu.usbmodemNNN]
  python tools/latency_3456a.py --lib ../../../Tools-Bench/hp3456a/tools    # readings/s of hp3456lib's read() instead
Needs pyusb and libusb. Holds ~/.cache/hp3456a.lock, as Tools-Bench's hp3456a tools do. Leaves the meter in D1 T1, local.
"""
import argparse
import fcntl
import glob
import os
import pathlib
import select
import statistics
import struct
import sys
import termios
import time

if os.path.isdir("/opt/homebrew/lib") and "DYLD_FALLBACK_LIBRARY_PATH" not in os.environ:
    os.environ["DYLD_FALLBACK_LIBRARY_PATH"] = "/opt/homebrew/lib"

import usb.core
import usb.util

LOCK = pathlib.Path.home() / ".cache" / "hp3456a.lock"
MAX_IN = 1008
now = time.perf_counter


def ms(xs):
    xs = [x * 1000 for x in xs]
    return f"med {statistics.median(xs):6.2f}  min {min(xs):6.2f}  max {max(xs):6.2f} ms"


class Tmc:
    def __init__(self):
        self.d = usb.core.find(idVendor=0x1209, idProduct=0x0001)
        if self.d is None:
            raise SystemExit("adapter not found")
        intf = usb.util.find_descriptor(self.d.get_active_configuration(), bInterfaceClass=0xFE)
        self.intf = intf.bInterfaceNumber
        eps = {(e.bEndpointAddress & 0x80, e.bmAttributes & 3): e for e in intf}
        self.ep_out, self.ep_in, self.ep_int = (eps[k].bEndpointAddress for k in ((0, 2), (0x80, 2), (0x80, 3)))
        self.int_interval = eps[(0x80, 3)].bInterval
        usb.util.claim_interface(self.d, self.intf)
        self.tag = 0
        self.flush()

    def close(self):
        self.d.ctrl_transfer(0xA1, 160, 0, self.intf, 1, timeout=2000)
        self.d.ctrl_transfer(0xA1, 161, 0, self.intf, 1, timeout=2000)
        usb.util.release_interface(self.d, self.intf)
        usb.util.dispose_resources(self.d)

    def _next(self):
        self.tag = self.tag % 127 + 1
        return self.tag

    def flush(self):
        while True:
            try:
                self.d.read(self.ep_int, 2, timeout=20)
            except usb.core.USBError:
                return

    def write(self, text):
        """-> seconds for the OUT transfer"""
        t, b = self._next(), text.encode()
        hdr = struct.pack("<BBBBIB3x", 1, t, ~t & 255, 0, len(b), 1)
        t0 = now()
        self.d.write(self.ep_out, hdr + b + b"\0" * (-len(b) % 4), timeout=3000)
        return now() - t0

    def stb(self):
        """-> (status byte, control seconds, interrupt seconds)"""
        tag = self._next() + 1
        t0 = now()
        r = bytes(self.d.ctrl_transfer(0xA1, 128, tag, self.intf, 3, timeout=2000))
        t1 = now()
        while True:
            n = bytes(self.d.read(self.ep_int, 2, timeout=2000))
            if n[0] == (0x80 | tag):
                break
        return r[2], t1 - t0, now() - t1

    def stb_ctrl(self):
        """Status byte from the control reply alone, leaving the interrupt notification unread -> (stb, seconds)"""
        tag = self._next() + 1
        t0 = now()
        r = bytes(self.d.ctrl_transfer(0xA1, 128, tag, self.intf, 3, timeout=2000))
        return r[2], now() - t0

    def read(self, term=None, timeout=5000):
        """-> (payload, request seconds, bulk IN seconds)"""
        t = self._next()
        attr, tc = (2, term[0]) if term else (0, 0)
        req = struct.pack("<BBBBIBBxx", 2, t, ~t & 255, 0, MAX_IN, attr, tc)
        t0 = now()
        self.d.write(self.ep_out, req, timeout=2000)
        t1 = now()
        raw = bytes(self.d.read(self.ep_in, MAX_IN + 12 + 64, timeout=timeout))
        t2 = now()
        return raw[12:12 + struct.unpack("<I", raw[4:8])[0]], t1 - t0, t2 - t1


def wait_ready(t, limit=5.0):
    """Poll READ_STATUS_BYTE until data ready -> (seconds, polls)"""
    t0, n = now(), 0
    while now() - t0 < limit:
        n += 1
        if t.stb()[0] & 4:
            return now() - t0, n
    raise SystemExit("no data ready")


def tmc_case(t, name, setup, reps, term, settle=0.0):
    t.write(setup + "T4")
    time.sleep(0.3)
    t.stb()
    t.flush()
    w, bus, rdy, polls, ctl, itr, req, bulk, total, size = [], [], [], [], [], [], [], [], [], 0
    for _ in range(reps):
        t.stb()
        t0 = now()
        w.append(t.write("T3"))
        t.write("")
        bus.append(now() - t0)
        r, n = wait_ready(t)
        rdy.append(now() - t0)
        polls.append(n)
        if settle:
            time.sleep(settle)
        data, a, b = t.read(term)
        req.append(a)
        bulk.append(b)
        total.append(now() - t0 - settle)
        size = len(data)
    for _ in range(reps):  # status polls with the meter idle
        _, a, b = t.stb()
        ctl.append(a)
        itr.append(b)
    print(f"\n== USBTMC {name}  ({size} B reply, settle {settle * 1000:.0f} ms, {statistics.mean(polls):.1f} polls/reading)")
    print(f"  OUT 'T3'              {ms(w)}")
    print(f"  T3 done on bus        {ms(bus)}")
    print(f"  T3 -> data ready      {ms(rdy)}")
    print(f"  stb control (idle)    {ms(ctl)}")
    print(f"  stb interrupt (idle)  {ms(itr)}")
    print(f"  IN request (OUT)      {ms(req)}")
    print(f"  bulk IN               {ms(bulk)}")
    print(f"  whole reading         {ms(total)}   -> {1 / statistics.median(total):.1f} readings/s")


class Cdc:
    def __init__(self, port):
        self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0], a[1], a[3] = 0, 0, 0  # raw: no input, output or local processing
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        self.drain()

    def close(self):
        os.close(self.fd)

    def drain(self, wait=0.05):
        while select.select([self.fd], [], [], wait)[0]:
            os.read(self.fd, 4096)

    def send(self, line):
        os.write(self.fd, line.encode() + b"\n")

    def until(self, end, wait=3.0):
        out, t_end = b"", now() + wait
        while not out.endswith(end) and now() < t_end:
            if select.select([self.fd], [], [], 0.05)[0]:
                out += os.read(self.fd, 4096)
        return out


def cdc_case(c, addr, reps):
    for line in (f"++addr {addr}", "++auto 0", "++read_tmo_ms 3000", "++eos 3", "++eoi 1"):
        c.send(line)
    c.send("SM024P0O1F1R4Z11STIT4")
    time.sleep(0.5)
    c.send("++spoll")
    c.until(b"\n")
    c.drain()
    spoll, rdy, rd, total = [], [], [], []
    for _ in range(reps):
        t0 = now()
        c.send("T3")
        while True:
            t1 = now()
            c.send("++spoll")
            s = c.until(b"\n")
            spoll.append(now() - t1)
            if int(s or b"0") & 4:
                break
        rdy.append(now() - t0)
        t1 = now()
        c.send("++read eoi")
        data = c.until(b"\n")
        rd.append(now() - t1)
        total.append(now() - t0)
    print(f"\n== Prologix serial, DCV 10 V, 1 PLC, autozero on, ASCII ({len(data)} B reply)")
    print(f"  ++spoll               {ms(spoll)}")
    print(f"  T3 -> data ready      {ms(rdy)}")
    print(f"  ++read eoi            {ms(rd)}")
    print(f"  whole reading         {ms(total)}   -> {1 / statistics.median(total):.1f} readings/s")
    c.send("SM000T1")
    c.send("++loc")


def lib_bench(path, reps):
    """Readings/s of hp3456lib's HP3456A.read() with the display on and off"""
    sys.path.insert(0, path)
    import hp3456lib
    with hp3456lib.HP3456A(lock=False) as m:
        for disp in ("D1", "D0"):
            for packed in (False, True):
                m.configure("dcv", range=10, nplc=1, autozero=True, packed=packed)
                m.write(disp)
                m.read()
                t0 = now()
                for _ in range(reps):
                    m.read()
                dt = (now() - t0) / reps
                print(f"  {disp} {'packed' if packed else 'ASCII '}  {dt * 1000:6.1f} ms/reading  {1 / dt:5.1f} readings/s")
        m.write("D1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reps", type=int, default=20)
    ap.add_argument("--no-cdc", action="store_true")
    ap.add_argument("--no-tmc", action="store_true")
    ap.add_argument("--port", help="Prologix serial port (default: the only /dev/cu.usbmodem*)")
    ap.add_argument("--addr", type=int, default=6)
    ap.add_argument("--lib", help="directory of hp3456lib.py: time its read() and skip the transfer timings")
    args = ap.parse_args()

    LOCK.parent.mkdir(parents=True, exist_ok=True)
    lock = open(LOCK, "a+")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    if args.lib:
        print(f"hp3456lib in {args.lib}: DCV 10 V, 1 PLC, autozero on")
        lib_bench(args.lib, args.reps)
        return

    if not args.no_tmc:
        t = Tmc()
        print(f"interrupt endpoint bInterval {t.int_interval} ms")
        t.write("SM024F1R4")
        for disp in ("D1", "D0"):
            base = "F1R4" + disp
            tmc_case(t, f"{disp}, 1 PLC, autozero on, ASCII", base + "Z11STIP0O1", args.reps, b"\n")
            tmc_case(t, f"{disp}, 1 PLC, autozero on, packed", base + "Z11STIP1O1", args.reps, None)
            tmc_case(t, f"{disp}, 1 PLC, autozero off, ASCII", base + "Z01STIP0O1", args.reps, b"\n")
            tmc_case(t, f"{disp}, 0.1 PLC, autozero on, ASCII", base + "Z1.1STIP0O1", args.reps, b"\n")
        tmc_case(t, "D1, 1 PLC, autozero on, ASCII, read 100 ms after ready", "F1R4D1Z11STIP0O1", args.reps, b"\n", 0.1)
        t.write("D1Z11STIP0O1SM000T1")
        t.close()

    if not args.no_cdc:
        ports = [args.port] if args.port else glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            raise SystemExit(f"pick the adapter's serial port with --port: {ports}")
        c = Cdc(ports[0])
        cdc_case(c, args.addr, args.reps)
        c.close()


if __name__ == "__main__":
    main()
