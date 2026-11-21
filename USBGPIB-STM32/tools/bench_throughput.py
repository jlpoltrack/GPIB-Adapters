#!/usr/bin/env python3
"""Measure read throughput through the adapter: repeat a query that returns a large response and report KB/s.

pip install pyvisa pyvisa-py pyusb
  python3 tools/bench_throughput.py --list
  python3 tools/bench_throughput.py --resource 'USB0::0x03EB::0x2065::...::INSTR' --query ':WAV:DATA?' --setup ':WAV:FORM BYTE'
"""
import argparse
import statistics
import time

import pyvisa


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--resource", help="VISA resource string")
    ap.add_argument("--list", action="store_true", help="list VISA resources and exit")
    ap.add_argument("--backend", default="@py", help="pyvisa backend ('@py' = pyvisa-py, '' = system VISA)")
    ap.add_argument("--query", default="*IDN?", help="command whose response is read back")
    ap.add_argument("--setup", action="append", default=[], help="command sent once before timing (repeatable)")
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--timeout", type=int, default=20000, help="ms")
    ap.add_argument("--chunk", type=int, default=0, help="pyvisa chunk_size in bytes (0 = default)")
    args = ap.parse_args()

    rm = pyvisa.ResourceManager(args.backend)
    if args.list or not args.resource:
        for r in rm.list_resources():
            print(r)
        return

    inst = rm.open_resource(args.resource)
    inst.timeout = args.timeout
    if args.chunk:
        inst.chunk_size = args.chunk
    print("IDN:", inst.query("*IDN?").strip())
    for cmd in args.setup:
        inst.write(cmd)

    sizes, rates = [], []
    for i in range(args.reps):
        inst.write(args.query)
        t0 = time.perf_counter()
        data = inst.read_raw()
        dt = time.perf_counter() - t0
        sizes.append(len(data))
        rates.append(len(data) / dt / 1024)
        print(f"{i + 1:3d}: {len(data):8d} B  {dt * 1000:8.1f} ms  {rates[-1]:7.1f} KB/s")

    print(f"\nresponse {statistics.median(sizes):.0f} B; KB/s median {statistics.median(rates):.1f}, "
          f"min {min(rates):.1f}, max {max(rates):.1f}")
    print("time includes the instrument's own response latency: use a query whose data is already prepared")


if __name__ == "__main__":
    main()
