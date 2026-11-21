#!/usr/bin/env python3
"""Load on every port at once for SECONDS (SIM=1 build); counts and errors per port, for the CPU headroom check.

Only VXI-11 sends queries: two query streams to one instrument take each other's replies. USBTMC and raw 5025 send
commands with no reply, Prologix 1234 serial polls, HTTP fetches the page (--http-rate per second, 0 = flat out).
  python tools/load_all.py HOST [--seconds 240] [--http-rate 10]
Needs gpib-core's .venv (PyVISA-py).
"""
import argparse
import http.client
import socket
import threading
import time

import pyvisa

ap = argparse.ArgumentParser()
ap.add_argument("host")
ap.add_argument("--seconds", type=float, default=240)
ap.add_argument("--http-rate", type=float, default=10)
a = ap.parse_args()
rm = pyvisa.ResourceManager("@py")
end = time.time() + a.seconds
stats = {}


def run(name, fn):
    n = bad = 0
    while time.time() < end:
        try:
            bad += not fn()
        except Exception as e:  # noqa: BLE001 - count any failure and keep loading
            bad += 1
            print(f"{name}: {e!r}"[:120], flush=True)
            time.sleep(0.5)
        n += 1
    stats[name] = (n, bad)


def visa_query(res):
    i = rm.open_resource(res, timeout=5000, read_termination="\n", write_termination="\n")
    return lambda: i.query("MEAS?").startswith("+1.")


def visa_command(res):
    i = rm.open_resource(res, timeout=5000, write_termination="\n")

    def w():
        i.write("*CLS")
        time.sleep(0.01)
        return True
    return w


class Line:
    """A TCP line client that reconnects after an error"""

    def __init__(self, port):
        self.port, self.s = port, None

    def __call__(self, line, reply):
        try:
            if not self.s:
                self.s = socket.create_connection((a.host, self.port), timeout=5)
                self.f = self.s.makefile("rb")
            self.s.sendall(line)
            return self.f.readline() if reply else b"ok"
        except OSError:
            self.s = None
            raise


raw = Line(5025)
px = Line(1234)


def raw5025():
    ok = raw(b"*CLS\n", False)
    time.sleep(0.01)
    return ok


def http_page():
    c = http.client.HTTPConnection(a.host, 80, timeout=5)
    c.request("GET", "/")
    r = c.getresponse()
    ok = r.status == 200 and len(r.read()) > 1000
    c.close()
    if a.http_rate:
        time.sleep(1 / a.http_rate)
    return ok


usb = [r for r in rm.list_resources() if r.startswith("USB")]
ports = {"vxi11": visa_query(f"TCPIP::{a.host}::inst0::INSTR"), "5025": raw5025,
         "1234": lambda: px(b"++spoll\n", True).strip().isdigit(), "http": http_page}
if usb:
    ports["usbtmc"] = visa_command(usb[0])
ts = [threading.Thread(target=run, args=kv) for kv in ports.items()]
for t in ts:
    t.start()
for t in ts:
    t.join()
for k, (n, bad) in stats.items():
    print(f"{k:7s} {n:6d} requests, {n / a.seconds:6.1f}/s, {bad} bad")
