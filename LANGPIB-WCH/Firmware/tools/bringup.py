#!/usr/bin/env python3
"""LANGPIB-WCH bring-up checks, one stage at a time or all in order (docs/firmware.md, bring-up order).

  bringup.py flash [--sim]     bootloader + app over the SDI probe (settings kept; production.py does the mass erase)
  bringup.py log [--seconds N] reset, then the UART boot log: clock, UID, GPIB pin test, PHY, link, address
  bringup.py usb               the board on USB: Prologix CDC port (++ver, *IDN?) and the DFU runtime interface
  bringup.py usbtmc [--sim]    USBTMC through PyVISA-py (gpib-core's .venv): *IDN?; --sim: also a long read and write
  bringup.py net HOST          ping, web page, LXI XML, raw SCPI 5025, Prologix 1234, gpib-core's VXI-11 and socket checks
  bringup.py all [--sim]       flash, log, usb, usbtmc, net (HOST from the boot log's address)

--sim: the SIM=1 build (build-sim/, a simulated instrument at GPIB 5, no GPIB plug needed); the net stage then also
runs the simulator-only VXI-11 checks. Needs pyserial; the VXI-11 and socket checks use gpib-core's .venv if present.
"""
import argparse
import http.client
import os
import re
import socket
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
GPIB_CORE = os.path.normpath(os.path.join(FIRMWARE, "..", "..", "gpib-core"))
WCH_TOOLS = os.path.normpath(os.path.join(FIRMWARE, "..", "..", "..", "Reference-WCH", "tools"))
sys.path[:0] = [HERE, WCH_TOOLS]
import production  # noqa: E402
import serial  # noqa: E402
import serial.tools.list_ports  # noqa: E402

PRODUCT = "LAN GPIB adapter"
results = []


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print("%s  %s%s" % ("PASS" if cond else "FAIL", name, ("  (%s)" % detail) if detail else ""))
    return bool(cond)


def probe():
    import sdi_probe
    gdb, uart = sdi_probe.find_ports()
    return sdi_probe, sdi_probe.Rsp(gdb), uart


def build_dir(sim):
    return os.path.join(FIRMWARE, "build-sim" if sim else "build")


# --- stages -----------------------------------------------------------------------------------------------------

def stage_flash(a):
    segs, boot_crc, app_crc = production.images(os.path.join(FIRMWARE, "bootloader", "build", "boot.bin"),
                                                os.path.join(build_dir(a.sim), "langpib.bin"),
                                                production.app_version())
    sdi, r, _ = probe()
    ok, text = production.monitor_text(r, "reset halt")
    if not check("probe attaches", ok, text.strip()):
        return
    sdi.flash(r, segs, True)
    check("flashed %s build (boot %08x, app %08x)" % ("SIM" if a.sim else "real", boot_crc, app_crc), True)
    production.monitor_text(r, "reset run")
    r.cmd("D")


def stage_log(a):
    sdi, r, uart = probe()

    def start():
        production.monitor_text(r, "reset run")
        r.cmd("D")
    s = serial.Serial(uart, 115200, timeout=0.1)
    s.reset_input_buffer()
    start()
    end, buf = time.time() + a.seconds, b""
    while time.time() < end:
        buf += s.read(4096)
    s.close()
    log = buf.decode(errors="replace")
    sys.stdout.write("".join("  | " + ln + "\n" for ln in log.splitlines() if ln.strip()))
    info = production.parse_boot_log(log)
    check("boot log: no errors", not info["errors"], "; ".join(info["errors"]))
    check("GPIB pin self-test", re.search(r"GPIB pins: (ok|not tested)", log),
          (re.search(r"GPIB pins:[^\r\n]*", log) or re.search("$", "")).group(0))
    check("Ethernet link", "ETH link up" in log, (re.search(r"ETH link up[^\r\n]*", log) or re.search("$", "")).group(0))
    m = re.findall(r"net: address (\d+\.\d+\.\d+\.\d+) \((\w+)\)", log)
    m = [x for x in m if x[0] != "0.0.0.0"]
    check("IP address", m, " ".join("%s (%s)" % x for x in m))
    a.host = a.host or (m[-1][0] if m else info.get("hostname", "") and info["hostname"] + ".local")
    return info


def prologix(port, cmds, wait=0.3):
    s = serial.Serial(port, 115200, timeout=0.5)
    s.reset_input_buffer()
    out = []
    for c in cmds:
        s.write((c + "\n").encode())
        time.sleep(wait)
        out.append(s.read(4096).decode(errors="replace").strip())
    s.close()
    return out


def stage_usb(a):
    ports = [p for p in serial.tools.list_ports.comports() if PRODUCT in (p.product or "")]
    if not check("board enumerates on USB (CDC port)", ports, ", ".join(p.device for p in ports)):
        return
    p = ports[0]
    check("USB serial number (from *IDN?, else the UID)", p.serial_number, p.serial_number)
    ver, addr, _, idn = prologix(p.device, ["++ver", "++addr", "*IDN?", "++read eoi"])
    check("Prologix ++ver", "LANGPIB-WCH" in ver, ver)
    check("Prologix ++addr", addr.isdigit(), addr)
    check("*IDN? through the instrument (any reply: not every instrument has *IDN?)", idn, idn)
    try:
        dfu = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True, timeout=10).stdout
        check("DFU runtime interface listed", re.search(r"1209:0001.*Runtime|Runtime.*1209:0001", dfu),
              (re.search(r"Found Runtime[^\n]*", dfu) or re.search("$", "")).group(0))
    except FileNotFoundError:
        check("dfu-util installed", False, "brew install dfu-util")


USBTMC_CHECK = r"""
import re, sys, pyvisa
rm = pyvisa.ResourceManager("@py")
res = [r for r in rm.list_resources() if re.match(r"USB\d*::(4617|0x1209)::(1|0x0001)::", r, re.I)]  # 1209:0001
if not res:
    sys.exit("no USBTMC resource for 1209:0001")
i = rm.open_resource(res[0], timeout=3000)
idn = i.query("*IDN?").strip()
ok = idn.count(",") >= 3
print("%s idn=%r" % (res[0], idn))
if "--sim" in sys.argv:  # the simulated instrument's BLOCK? and X..X? commands
    blk = i.query("BLOCK? 254").strip()
    x = i.query("X" * 499 + "?").strip()
    ok = ok and blk == "".join(chr(65 + k % 26) for k in range(254)) and x == "500"
    print("block=%d long-write=%r" % (len(blk), x))
sys.exit(0 if ok else 1)
"""


def stage_usbtmc(a):
    py = os.path.join(GPIB_CORE, ".venv", "bin", "python")
    if not check("gpib-core .venv (PyVISA-py, pyusb)", os.path.exists(py)):
        return
    p = subprocess.run([py, "-c", USBTMC_CHECK] + (["--sim"] if a.sim else []), capture_output=True, text=True,
                       timeout=60)
    check("USBTMC: *IDN?" + (", 254-byte read, 500-byte write" if a.sim else ""), p.returncode == 0,
          (p.stdout + p.stderr).strip()[-200:])


def http_get(host, path):
    with urllib.request.urlopen("http://%s%s" % (host, path), timeout=5) as r:
        return r.status, r.read().decode(errors="replace")


def tcp_query(host, port, line, wait=1.0):
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(line)
    time.sleep(wait)
    s.settimeout(2)
    try:
        return s.recv(4096).decode(errors="replace").strip()
    finally:
        s.close()


def stage_net(a):
    if not a.host:
        check("host to test", False, "give HOST, or run 'log' first")
        return
    h = a.host
    check("ping " + h, subprocess.run(["ping", "-c", "3", "-t", "5", h], capture_output=True).returncode == 0)
    for path, want in (("/", "<html"), ("/lxi/identification", "LXIDevice")):
        try:
            st, body = http_get(h, path)
            check("HTTP %s" % path, st == 200 and want in body, "%d, %d bytes" % (st, len(body)))
        except (OSError, http.client.HTTPException) as e:
            check("HTTP %s" % path, False, repr(e))
    for port, q in ((5025, b"*IDN?\n"), (1234, b"++ver\n")):
        try:
            ans = tcp_query(h, port, q)
            check("TCP %d %s" % (port, q.decode().strip()), ans, ans)
        except OSError as e:
            check("TCP %d" % port, False, str(e))
    py = os.path.join(GPIB_CORE, ".venv", "bin", "python")
    if not os.path.exists(py):
        check("gpib-core .venv for VXI-11 checks", False, "cd gpib-core && python3 -m venv .venv && "
              ".venv/bin/pip install -r tools/requirements.txt")
        return
    vx = [py, os.path.join(GPIB_CORE, "tools", "vxi11_check.py"), h] + ([] if a.sim else ["--board"])
    check("gpib-core vxi11_check.py", subprocess.run(vx).returncode == 0)
    check("gpib-core sockets_check.py",
          subprocess.run([py, os.path.join(GPIB_CORE, "tools", "sockets_check.py"), h]).returncode == 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage", choices=["flash", "log", "usb", "usbtmc", "net", "all"])
    ap.add_argument("host", nargs="?", help="board IP or name for 'net' (default: from the boot log)")
    ap.add_argument("--sim", action="store_true", help="SIM=1 build: simulated instrument, no GPIB plug")
    ap.add_argument("--seconds", type=float, default=12.0, help="boot log capture time (DHCP takes a few s)")
    a = ap.parse_args()

    stages = ["flash", "log", "usb", "usbtmc", "net"] if a.stage == "all" else [a.stage]
    for st in stages:
        print("\n== %s ==" % st)
        try:
            globals()["stage_" + st](a)
        except (OSError, SystemExit, production.Fail, subprocess.TimeoutExpired) as e:
            check(st + " stage ran", False, str(e))
        if st == "log" and a.stage == "all":
            time.sleep(1)  # USB enumerates after the instrument search
    bad = [n for n, ok in results if not ok]
    print("\n%d checks, %d failed%s" % (len(results), len(bad), (": " + "; ".join(bad)) if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
