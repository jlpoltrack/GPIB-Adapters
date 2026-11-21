#!/usr/bin/env python3
"""Raw SCPI (TCP 5025) and Prologix (TCP 1234) checks, with plain sockets and PyVISA-py's SOCKET resource.

Against the host server (make host; build/host/vxi11_host -p 11111):
    .venv/bin/python tools/sockets_check.py
Against a board:
    .venv/bin/python tools/sockets_check.py 192.168.1.50
An instrument without *IDN? (it answers with a reading): --expect REGEX, e.g. '^[+-][0-9.]+E[+-][0-9]' for an HP 3456A.
"""
import argparse
import re
import socket
import sys
import time

import pyvisa

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print(f"{'PASS' if cond else 'FAIL'}  {name}" + (f"  ({detail})" if detail else ""))


_pending = {}


def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    _pending[s] = b""
    return s


def line(s, timeout=5.0):
    """One reply line, LF-terminated; what came after it is kept for the next call"""
    s.settimeout(timeout)
    buf = _pending.get(s, b"")
    while b"\n" not in buf:
        d = s.recv(4096)
        if not d:
            break
        buf += d
    head, sep, rest = buf.partition(b"\n")
    _pending[s] = rest
    return head + sep


def quiet(s, wait=0.5):
    """True if nothing arrives within wait seconds"""
    s.settimeout(wait)
    try:
        return not s.recv(4096)
    except socket.timeout:
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="127.0.0.1")
    ap.add_argument("--expect", help="regex every *IDN? reply must match (default: the same reply each time)")
    a = ap.parse_args()

    # --- raw SCPI ---
    s = connect(a.host, 5025)
    s.sendall(b"*IDN?\n")
    idn = line(s)
    if a.expect:
        rx = re.compile(a.expect.encode())
        same = lambda r: bool(rx.search(r)) and r.endswith(b"\n")  # noqa: E731
    else:
        same = lambda r: r == idn  # noqa: E731
    check("5025: *IDN? answers", (same(idn) if a.expect else idn.count(b",") >= 3) and idn.endswith(b"\n"), idn)
    s.sendall(b"*CLS\n")
    check("5025: a command gets no reply", quiet(s))
    s.sendall(b"*IDN?\r\n*IDN?\n")
    check("5025: two queries in one send", same(line(s)) and same(line(s)))

    s2 = connect(a.host, 5025)  # replaces s
    s2.sendall(b"*IDN?\n")
    check("5025: a new client replaces the old one", same(line(s2)))
    s.settimeout(2)
    try:
        dropped = s.recv(1) == b""
    except (ConnectionResetError, socket.timeout):
        dropped = True
    check("5025: the old client is closed", dropped)
    s.close()
    s2.close()

    rm = pyvisa.ResourceManager("@py")
    inst = rm.open_resource(f"TCPIP::{a.host}::5025::SOCKET", read_termination="\n", write_termination="\n",
                            timeout=5000)
    q = inst.query("*IDN?")
    check("5025: PyVISA SOCKET query", same(q.encode() + b"\n"), q)
    t0 = time.time()
    n = 20
    for _ in range(n):
        inst.query("*IDN?")
    check("5025: PyVISA query rate", True, f"{(time.time() - t0) / n * 1000:.1f} ms a query")
    inst.close()

    # --- Prologix ---
    p = connect(a.host, 1234)
    p.sendall(b"++ver\n")
    ver = line(p)
    check("1234: ++ver", b"Prologix" in ver, ver)
    p.sendall(b"++auto 0\n*IDN?\n++read eoi\n")
    check("1234: write, ++read eoi", same(line(p)))
    p.sendall(b"++auto 1\n*IDN?\n")
    check("1234: ++auto 1 reads after a write", same(line(p)))
    p.sendall(b"++auto 0\n")

    # both ports at once share the bus: raw queries against Prologix serial polls (which leave the reply alone)
    r = connect(a.host, 5025)
    n, good = 20, True
    for _ in range(n):
        r.sendall(b"*IDN?\n")
        p.sendall(b"++spoll\n")
        good = good and same(line(r)) and line(p).strip().isdigit()
    check("5025 queries and 1234 serial polls at once", good)
    r.close()
    p.close()

    print("all passed" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
