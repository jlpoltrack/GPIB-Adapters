#!/usr/bin/env python3
"""Steady adapter-driven GPIB writes for scoping DIO edges and data-to-DAV settle (docs/firmware.md, still to verify).

Sends '*CLS;' x N + '*OPC?' over VXI-11 in a loop, so the adapter talks for most of the time; any SCPI instrument.
  python tools/bus_traffic.py HOST [--seconds 600] [--bytes 3000]
Trigger the scope on DAV (J3 pin 6) falling; DIO1 is pin 1, grounds are pins 18-24. Needs gpib-core's .venv.
"""
import argparse
import time

import pyvisa

ap = argparse.ArgumentParser()
ap.add_argument("host")
ap.add_argument("--seconds", type=float, default=600)
ap.add_argument("--bytes", type=int, default=3000)
a = ap.parse_args()

d = pyvisa.ResourceManager("@py").open_resource(f"TCPIP::{a.host}::inst0::INSTR", timeout=10000)
d.read_termination = d.write_termination = "\n"
msg = "*CLS;" * (a.bytes // 5) + "*OPC?"
end = report = time.time()
end += a.seconds
n = bad = 0
while time.time() < end:
    bad += d.query(msg).strip().lstrip("+") != "1"
    n += 1
    if time.time() >= report:
        print(f"{n} messages of {len(msg)} B, {bad} bad", flush=True)
        report += 30
print(f"done: {n} messages, {bad} bad")
