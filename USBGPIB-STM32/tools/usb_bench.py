#!/usr/bin/env python3
"""USB checks that need no instrument: USBTMC control and bulk paths, the Prologix CDC port, and a mode switch.

Runs against the bench build (make -C Firmware BENCH=1 flash), which enumerates with nothing on the bus:
  ~/.local/pipx/venvs/pyocd/bin/python tools/usb_bench.py [--modes]

--modes also cycles ++usb cdc / both / tmc, re-enumerating each time. It ends in tmc-only: run it last.
"""
import glob
import struct
import sys
import time

import libusb_package
import serial
import usb.core
import usb.util

VID, PID = 0x1209, 0x0001
ITF = 0
EP_OUT, EP_IN, EP_INT = None, None, None
SUCCESS, PENDING, FAILED = 1, 2, 0x80
GET_CAPS, PULSE, READ_STB = 7, 64, 128
ABORT_IN, CHECK_ABORT_IN, CLEAR, CHECK_CLEAR = 3, 4, 5, 6
REN_CONTROL, GO_TO_LOCAL, LOCAL_LOCKOUT = 160, 161, 162

results = []
tag = 0


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail != "" else ""))


def next_tag():
    global tag
    tag = tag % 127 + 1
    return tag


def ctrl(dev, req, value, length, recipient=1):
    return bytes(dev.ctrl_transfer(0xA0 | recipient, req, value, ITF, length, timeout=2000))


def out_msg(dev, data, eom=True):
    t = next_tag()
    hdr = struct.pack("<BBBBIB3x", 1, t, ~t & 0xFF, 0, len(data), 1 if eom else 0)
    dev.write(EP_OUT, hdr + data + b"\0" * (-len(data) % 4), timeout=3000)


def request_in(dev, size=1024):
    t = next_tag()
    dev.write(EP_OUT, struct.pack("<BBBBIBBxx", 2, t, ~t & 0xFF, 0, size, 0, 0), timeout=2000)
    return t


def read_in(dev, t, timeout=3000):
    raw = bytes(dev.read(EP_IN, 2048, timeout=timeout))
    if len(raw) < 12:
        return raw, None
    msg, rt, inv, _, size, attrs = struct.unpack("<BBBBIB3x", raw[:12])
    assert msg == 2 and rt == t and inv == (~t & 0xFF), f"bad IN header {raw[:12].hex()}"
    return raw[12:12 + size], attrs


def bang(dev, cmd):
    """An internal '!' command: INDICATOR_PULSE arms it, the answer comes on the next read"""
    ctrl(dev, PULSE, 0, 1)
    out_msg(dev, cmd)
    return read_in(dev, request_in(dev))[0]


def tmc_tests(dev):
    caps = ctrl(dev, GET_CAPS, 0, 0x18)
    check("GET_CAPABILITIES", caps[0] == SUCCESS, caps[:8].hex())
    check("INDICATOR_PULSE", ctrl(dev, PULSE, 0, 1)[0] == SUCCESS)
    v = bang(dev, b"!ver?\n")
    check("!ver? through bulk OUT and IN", len(v) > 0, repr(v))

    while True:  # notifications of earlier runs stay queued until someone reads them
        try:
            dev.read(EP_INT, 2, timeout=100)
        except usb.core.USBTimeoutError:
            break
    stb_tag = 5
    r = ctrl(dev, READ_STB, stb_tag, 3)
    check("READ_STATUS_BYTE", r[0] == SUCCESS and r[1] == stb_tag, r.hex())
    n = bytes(dev.read(EP_INT, 2, timeout=2000))  # 2 is wMaxPacketSize: more would wait for a short packet
    check("status byte notification on interrupt IN", n[0] == (0x80 | stb_tag), n.hex())

    # What a VISA timeout does: a read that nothing answers, then the abort sequence, then the next query must work
    t = next_tag()
    dev.write(EP_OUT, struct.pack("<BBBBIBBxx", 2, t, ~t & 0xFF, 0, 256, 0, 0), timeout=2000)
    time.sleep(0.2)
    a = bytes(dev.ctrl_transfer(0xA2, ABORT_IN, t, EP_IN, 2, timeout=2000))
    check("INITIATE_ABORT_BULK_IN", a[0] in (SUCCESS, FAILED, 0x81), a.hex())
    if a[0] == SUCCESS:
        for _ in range(50):
            c = bytes(dev.ctrl_transfer(0xA2, CHECK_ABORT_IN, 0, EP_IN, 8, timeout=2000))
            if c[0] != PENDING:
                break
            try:
                dev.read(EP_IN, 2048, timeout=100)
            except usb.core.USBError:
                pass
        check("CHECK_ABORT_BULK_IN_STATUS", c[0] == SUCCESS, c.hex())
    try:
        dev.read(EP_IN, 2048, timeout=100)
    except usb.core.USBError:
        pass
    check("query works after the abort", bang(dev, b"!ver?\n") == v)

    c = ctrl(dev, CLEAR, 0, 1)
    check("INITIATE_CLEAR", c[0] == SUCCESS, c.hex())
    for _ in range(100):
        s = ctrl(dev, CHECK_CLEAR, 0, 2)
        if s[0] != PENDING:
            break
        time.sleep(0.02)
    check("CHECK_CLEAR_STATUS ends", s[0] == SUCCESS, s.hex())
    check("query works after the clear", bang(dev, b"!ver?\n") == v)

    for name, req, val in (("REN_CONTROL on", REN_CONTROL, 1), ("LOCAL_LOCKOUT", LOCAL_LOCKOUT, 0),
                           ("GO_TO_LOCAL", GO_TO_LOCAL, 0), ("REN_CONTROL off", REN_CONTROL, 0)):
        r = ctrl(dev, req, val, 1)
        check(name, r[0] in (SUCCESS, 0x81), r.hex())

    # A write with no listener has to end, not wedge the OUT path
    t0 = time.time()
    out_msg(dev, b"*IDN?\n")
    time.sleep(0.5)
    v2 = bang(dev, b"!ver?\n")
    check("write to no listener, then a query", v2 == v, f"{time.time() - t0:.1f}s")


def cdc_tests(port):
    with serial.Serial(port, 115200, timeout=0.3) as s:
        def q(line, wait=0.3):
            s.reset_input_buffer()
            s.write(line.encode() + b"\n")
            end, out = time.time() + wait, b""
            while time.time() < end:
                out += s.read(256)
                if b"\n" in out:
                    break
            return out.decode(errors="replace").strip()

        v = q("++ver", 1)
        check("CDC ++ver", "Prologix" in v, v)
        q("++addr 7")
        check("CDC ++addr", q("++addr") == "7")
        check("CDC ++help", "addr" in q("++help"))
        s.write(b"*IDN?\n")
        time.sleep(1)
        e = q("++err", 2)
        check("CDC write with no bus reports it", e.startswith("no listener"), e)
        t = time.time()
        q("++scan", 2)
        check("CDC ++scan returns", time.time() - t < 2, f"{time.time() - t:.1f}s")
        s.write(("x" * 4000 + "\n").encode())  # more than the buffers hold: flow control has to hold up
        time.sleep(1.5)
        check("CDC survives a 4 KB burst", "Prologix" in q("++ver", 2))
        q("++addr 1")


def wait_dev(present=True, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        d = usb.core.find(idVendor=VID, idProduct=PID, backend=libusb_package.get_libusb1_backend())
        if bool(d) == present:
            return d
        time.sleep(0.1)
    return None


def cdc_port():
    time.sleep(0.5)
    ports = [p for p in glob.glob("/dev/cu.usbmodem*")]
    return next((p for p in ports if not p.endswith("21102")), None)  # 21102 is the ST-Link's own port


def setup():
    global EP_OUT, EP_IN, EP_INT
    dev = wait_dev()
    cfg = dev.get_active_configuration()
    print(f"bcdDevice {dev.bcdDevice:04x}, {cfg.bNumInterfaces} interfaces, serial {dev.serial_number}")
    tmc = usb.util.find_descriptor(cfg, bInterfaceClass=0xFE, bInterfaceSubClass=3)
    eps = {(e.bEndpointAddress & 0x80, e.bmAttributes & 3): e.bEndpointAddress for e in tmc}
    EP_OUT, EP_IN, EP_INT = eps[(0, 2)], eps[(0x80, 2)], eps[(0x80, 3)]
    usb.util.claim_interface(dev, ITF)
    return dev


def main():
    dev = setup()
    tmc_tests(dev)
    usb.util.dispose_resources(dev)
    port = cdc_port()
    check("CDC port present", port, port)
    if port:
        cdc_tests(port)

    if "--modes" in sys.argv and port:
        for mode, n in (("cdc", 3), ("both", 4), ("tmc", 2)):  # tmc last: it leaves no serial port to switch back
            with serial.Serial(cdc_port(), 115200, timeout=0.3) as s:
                s.write(f"++usb {mode}\n".encode())
            time.sleep(2.5)
            d = wait_dev()
            check(f"++usb {mode}", d and d.get_active_configuration().bNumInterfaces == n,
                  d and f"bcdDevice {d.bcdDevice:04x}")

    bad = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(bad)}/{len(results)} passed" + (f"; failed: {', '.join(bad)}" if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
