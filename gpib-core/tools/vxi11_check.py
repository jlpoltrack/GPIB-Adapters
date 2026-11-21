#!/usr/bin/env python3
"""VXI-11 interoperability check with real clients (PyVISA-py and python-vxi11).

Against the host server (Firmware: make host; build/host/vxi11_host):
    .venv/bin/python tools/vxi11_check.py
Against a board (instrument answering *IDN?; the simulator-only commands are skipped):
    .venv/bin/python tools/vxi11_check.py 192.168.1.50 --board
An instrument without *IDN? (it answers with a reading): --expect REGEX, e.g. '^[+-][0-9.]+E[+-][0-9]' for an HP 3456A.
"""
import argparse
import re
import sys
import threading
import time

import pyvisa
import pyvisa_py.protocols.rpc as pv_rpc
import vxi11
import vxi11.rpc as v_rpc

ok = True
good = None


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print(f"{'PASS' if cond else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))


def expect_error(fn, code_name):
    try:
        fn()
    except pyvisa.errors.VisaIOError as e:
        return code_name in str(e), str(e).split(":")[0]
    return False, "no error"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--pmap-port", type=int, default=111, help="portmapper port, if the server isn't on 111")
    ap.add_argument("--board", action="store_true", help="real instrument: skip the simulator's test commands")
    ap.add_argument("--expect", default=",", help="regex a reply to *IDN? must contain (default: a comma)")
    a = ap.parse_args()
    global good
    good = re.compile(a.expect).search
    pv_rpc.PMAP_PORT = a.pmap_port
    v_rpc.PMAP_PORT = a.pmap_port

    # portmapper over UDP, as VISA finders use it
    pm = pv_rpc.UDPPortMapperClient(a.host)
    port = pm.get_port((0x0607AF, 1, pv_rpc.IPPROTO_TCP, 0))
    check("portmapper UDP GETPORT core", port > 0, f"port {port}")
    pm.close()

    rm = pyvisa.ResourceManager("@py")
    res = f"TCPIP::{a.host}::inst0::INSTR"
    inst = rm.open_resource(res, timeout=3000)
    inst.read_termination = "\n"
    inst.write_termination = "\n"

    idn = inst.query("*IDN?")
    check("pyvisa-py *IDN?", good(idn), idn)

    t0 = time.time()
    n = 50
    for _ in range(n):
        inst.query("*IDN?")
    check("pyvisa-py 50 queries", True, f"{1000 * (time.time() - t0) / n:.1f} ms each")

    stb = inst.read_stb()
    check("read_stb", isinstance(stb, int), f"stb {stb}")
    inst.clear()
    check("clear", True)
    inst.assert_trigger()
    check("trigger", True)

    if not a.board:
        inst.chunk_size = 16  # many device_reads per message: REQCNT then END
        blk = inst.query("BLOCK? 200")
        check("long read in 16-byte chunks", len(blk) == 200 and blk.startswith("ABC"), f"{len(blk)} bytes")
        inst.chunk_size = 20 * 1024
        big = inst.query("BLOCK? 254")
        check("read capped at maxRecvSize, client continues", len(big) == 254)
        long_cmd = "X" * 3000 + "?"  # > maxRecvSize: the client splits the write
        got = inst.query(long_cmd)
        check("long write split by the client", got == str(len(long_cmd)), f"instrument counted {got}")

        inst.write("NOANSWER")
        inst.timeout = 500
        hit, code = expect_error(inst.read, "TMO")
        check("read timeout", hit, code)
        inst.timeout = 3000
        check("still works after a timeout", good(inst.query("*IDN?")))

    # gpib0,N resource name
    addr_inst = rm.open_resource(f"TCPIP::{a.host}::gpib0,5::INSTR", timeout=3000) if not a.board else None
    if addr_inst:
        addr_inst.read_termination = "\n"
        check("gpib0,5 link", good(addr_inst.query("*IDN?")))

    # exclusive lock between two sessions
    other = rm.open_resource(res, timeout=1000)
    other.read_termination = "\n"
    inst.lock_excl(timeout=1000)
    hit, code = expect_error(lambda: other.write("*IDN?"), "VI_ERROR")  # pyvisa-py maps code 11 to VI_ERROR_IO
    check("locked against another session (pyvisa-py)", hit, code)
    v2 = vxi11.Instrument(a.host, "inst0")
    try:
        v2.write("*IDN?")
        code = None
    except vxi11.vxi11.Vxi11Exception as e:
        code = e.err
    check("locked: error 11 (python-vxi11)", code == 11, f"error {code}")
    v2.close()
    inst.unlock()
    check("unlocked: other link works", good(other.query("*IDN?")))
    other.close()
    if addr_inst:
        addr_inst.close()
    inst.close()

    # python-vxi11, including the abort channel
    v = vxi11.Instrument(a.host, "inst0")
    v.timeout = 3
    check("python-vxi11 ask", good(v.ask("*IDN?")))
    v.local()   # pyvisa-py's TCPIP sessions don't implement control_ren
    v.remote()
    check("local / remote", True)
    if not a.board:
        v.write("NOANSWER")
        v.timeout = 20
        err = []

        def reader():
            try:
                v.read()
            except Exception as e:  # noqa: BLE001 - any failure is what we expect here
                err.append(e)

        th = threading.Thread(target=reader)
        th.start()
        time.sleep(0.5)
        v.abort()
        th.join(5)
        check("device_abort ends a pending read", (not th.is_alive()) and err, repr(err[0]) if err else "")
        v.timeout = 3
        check("python-vxi11 after abort", good(v.ask("*IDN?")))
    v.close()

    rm.close()
    print("all passed" if ok else "FAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
