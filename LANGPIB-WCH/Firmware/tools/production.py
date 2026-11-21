#!/usr/bin/env python3
"""Production flashing for LANGPIB-WCH through the RP2350-Zero SDI probe (WCH repo programmer/), one board per run.

  1. probe and chip: UID, chip ID, flash size
  2. option bytes: no read protection and no write protection (the app rewrites the bootloader), else stop
  3. mass erase (settings back to defaults), unless --keep-settings
  4. bootloader at 0x08000000, header + app at 0x08004000, verified
  5. reset, then the boot log over the probe's UART bridge: 200 MHz on the crystal, PHY answers, no ETH errors
  6. a CSV row per board (--log)

  make production                        (builds first)
  python3 tools/production.py [--keep-settings] [--no-boot-check] [--port-gdb P --port-uart P]

Needs pyserial and the WCH repo next to this one (its tools/sdi_probe.py speaks GDB RSP to the probe).
"""
import argparse
import csv
import datetime
import os
import re
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import mkdfu  # noqa: E402

import serial  # noqa: E402

BOOT_BASE = 0x08000000
BOOT_MAX = mkdfu.APP_HDR_ADDR - BOOT_BASE
ESIG_FLACAP, ESIG_UID, CHIP_ID = 0x1FFFF7E0, 0x1FFFF7E8, 0x1FFFF704


class Fail(Exception):
    pass


def images(boot_bin, app_bin, app_version):
    boot = open(boot_bin, "rb").read()
    app = open(app_bin, "rb").read()
    if not 0 < len(boot) <= BOOT_MAX:
        raise Fail("bootloader is %d bytes, its area holds %d" % (len(boot), BOOT_MAX))
    if not 0 < len(app) <= mkdfu.APP_END - mkdfu.APP_START:
        raise Fail("app is %d bytes, its area holds %d" % (len(app), mkdfu.APP_END - mkdfu.APP_START))
    if struct.unpack_from("<I", app, 0)[0] == 0xFFFFFFFF:
        raise Fail("app.bin looks empty")
    hdr_app = mkdfu.header(app, app_version) + app
    return [(BOOT_BASE, boot), (mkdfu.APP_HDR_ADDR, hdr_app)], zlib.crc32(boot), zlib.crc32(app)


def app_version():
    text = open(os.path.join(FIRMWARE, "src", "app_config.h")).read()
    m = re.search(r"APP_USB_BCD\s+(0x[0-9A-Fa-f]+)", text)
    return int(m.group(1), 16) if m else 0


def monitor_text(r, cmd):
    out = []
    reply = r.cmd("qRcmd," + cmd.encode().hex(), out.append)
    return reply == "OK", "".join(out)


def check_option_bytes(r):
    ok, text = monitor_text(r, "ob read")
    m = re.search(r"RDPR (\w\w) USER (\w\w) DATA0 \w\w DATA1 \w\w WRPR (\w\w) (\w\w) (\w\w) (\w\w)", text)
    if not ok or not m:
        raise Fail("option bytes unreadable: " + text.strip())
    rdpr, user = int(m.group(1), 16), int(m.group(2), 16)
    wrpr = [int(m.group(i), 16) for i in range(3, 7)]
    if rdpr != 0xA5:
        raise Fail("read protection is on (RDPR %02x): 'sdi_probe.py monitor unprotect --force' erases the chip, "
                   "then power-cycle it and run this again" % rdpr)
    if any(w != 0xFF for w in wrpr):
        raise Fail("write protection set (WRPR %s): the app must be able to rewrite the bootloader" %
                   " ".join("%02x" % w for w in wrpr))
    return "RAM_CODE_MOD %d (%s)" % ((user >> 6) & 1, "576K code + 136K RAM" if user & 0x40 else "512K code + 200K RAM")


# --- boot log ---------------------------------------------------------------------------------------------------

def parse_boot_log(text):
    """dict of what the app logged at boot; 'errors' lists what's wrong"""
    info, errors = {}, []
    m = re.search(r"LANGPIB-WCH: reset (0x[0-9a-f]+), HSE (\w+), (\d+) MHz, UID ([0-9A-F]{24})", text)
    if not m:
        return {"errors": ["no boot line"]}
    info.update(reset=m.group(1), hse=m.group(2), mhz=int(m.group(3)), uid=m.group(4))
    if info["hse"] != "ok":
        errors.append("25 MHz crystal failed")
    if info["mhz"] != 200:
        errors.append("clock %d MHz" % info["mhz"])
    m = re.search(r"chip ID (0x[0-9a-f]+), FEATURE_SIGN (0x[0-9a-f]+)", text)
    if m:
        info.update(chip_id=m.group(1), feature_sign=m.group(2))
    m = re.search(r"net: (\w+), ([\w-]+)\.local", text)
    if m:
        info.update(net=m.group(1), hostname=m.group(2))
    m = re.search(r"PHY id (0x[0-9a-f]+)", text)
    if not m:
        errors.append("no PHY id line")
    else:
        info["phy_id"] = m.group(1)
        if int(m.group(1), 16) in (0, 0xFFFFFFFF):
            errors.append("PHY doesn't answer (id %s)" % m.group(1))
    for line in re.findall(r"ETH: [^\r\n]*", text):
        if "link LEDs only" not in line:
            errors.append(line)
    m = re.search(r"GPIB pins: FAULT[^\r\n]*", text)
    if m:
        errors.append(m.group(0))
    if "watchdog OFF" in text:
        errors.append("the watchdog isn't running (LSI didn't start)")
    if "last trap" in text:
        errors.append("trap logged: " + re.search(r"last trap[^\r\n]*", text).group(0))
    if "bootloader: updating" in text:
        errors.append("the app rewrote the bootloader: the flashed one differs from the app's copy")
    info["errors"] = errors
    return info


def read_boot_log(uart_port, seconds, start_cb):
    """UART output from start_cb() (reset run) until the PHY id line, the last boot line before the instrument search"""
    s = serial.Serial(uart_port, 115200, timeout=0.1)
    s.reset_input_buffer()
    start_cb()
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        buf += s.read(4096)
        if re.search(rb"PHY id 0x[0-9a-f]+\r?\n", buf):
            time.sleep(0.2)
            buf += s.read(4096)
            break
    s.close()
    return buf.decode(errors="replace")


# --- main -------------------------------------------------------------------------------------------------------

def run(a):
    row = {"time": datetime.datetime.now().isoformat(timespec="seconds"), "result": "FAIL"}
    try:
        segs, boot_crc, app_crc = images(a.boot, a.app, app_version())
        row.update(boot_crc="%08x" % boot_crc, app_crc="%08x" % app_crc, app_version="%04x" % app_version())
        sys.path.insert(0, a.wch_tools)
        import sdi_probe  # finds the probe by its product string: the board is 1209:0001 too
        try:
            gdb_port, uart_port = (a.port_gdb, a.port_uart) if a.port_gdb else sdi_probe.find_ports()
        except SystemExit as e:
            raise Fail("%s; is it plugged in?" % e)
        print("probe: GDB %s, UART %s" % (gdb_port, uart_port))
        r = sdi_probe.Rsp(gdb_port)  # also socket://host:port: the WCH repo's gdb_sim_server
        stop = r.cmd("?")
        if stop.startswith("E"):
            _, err = monitor_text(r, "lasterr")
            raise Fail("can't attach: " + err.strip())
        monitor_text(r, "reset halt")

        uid = r.read(ESIG_UID, 12)
        row["uid"] = "".join("%02X" % b for w in range(3) for b in uid[4 * w:4 * w + 4])
        flash_kb, = struct.unpack("<H", r.read(ESIG_FLACAP, 2))
        chip, = struct.unpack("<I", r.read(CHIP_ID, 4))
        row["chip_id"] = "0x%08x" % chip
        print("chip: ID 0x%08x, flash %d KB, UID %s" % (chip, flash_kb, row["uid"]))
        if flash_kb < 512:
            raise Fail("flash size %d KB: not a CH32V407RET6?" % flash_kb)

        row["option_bytes"] = check_option_bytes(r)
        print("option bytes: OK, " + row["option_bytes"])

        if not a.keep_settings:
            print("mass erase")
            ok, text = monitor_text(r, "erase all")
            if not ok:
                raise Fail("mass erase failed: " + text.strip())
        try:
            sdi_probe.flash(r, segs, True)
        except SystemExit as e:
            raise Fail(str(e))

        if a.no_boot_check:
            monitor_text(r, "reset run")
            r.cmd("D")
            row["result"] = "OK (no boot check)"
            return row

        def start():
            monitor_text(r, "reset run")
            r.cmd("D")
        log = read_boot_log(uart_port, a.boot_seconds, start)
        sys.stdout.write("".join("  | " + ln + "\n" for ln in log.splitlines() if ln.strip()))
        info = parse_boot_log(log)
        for k in ("hostname", "phy_id", "feature_sign"):
            row[k] = info.get(k, "")
        if info.get("uid") and info["uid"] != row["uid"]:
            info["errors"].append("logged UID %s != probe's %s" % (info["uid"], row["uid"]))
        if info["errors"]:
            raise Fail("boot check: " + "; ".join(info["errors"]))
        row["result"] = "OK"
    except Fail as e:
        row["error"] = str(e)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boot", default=os.path.join(FIRMWARE, "bootloader", "build", "boot.bin"))
    ap.add_argument("--app", default=os.path.join(FIRMWARE, "build", "langpib.bin"))
    ap.add_argument("--wch-tools", default=os.path.normpath(os.path.join(FIRMWARE, "..", "..", "..", "Reference-WCH", "tools")))
    ap.add_argument("--port-gdb", help="probe GDB port (default: found by its USB product name); "
                    "socket://127.0.0.1:3333 for the WCH repo's gdb_sim_server, with --no-boot-check")
    ap.add_argument("--port-uart", help="probe UART port")
    ap.add_argument("--keep-settings", action="store_true", help="no mass erase: re-flash, keep the settings log")
    ap.add_argument("--no-boot-check", action="store_true", help="skip the boot log check")
    ap.add_argument("--boot-seconds", type=float, default=6.0, help="how long to wait for the boot log")
    ap.add_argument("--log", default=os.path.join(FIRMWARE, "..", "docs", "production-log.csv"))
    a = ap.parse_args()
    if bool(a.port_gdb) != bool(a.port_uart):
        ap.error("--port-gdb and --port-uart go together")

    row = run(a)
    fields = ["time", "result", "uid", "chip_id", "hostname", "phy_id", "feature_sign", "option_bytes",
              "app_version", "app_crc", "boot_crc", "error"]
    new = not os.path.exists(a.log)
    with open(a.log, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        if new:
            w.writeheader()
        w.writerow(row)
    print("\n%s%s  (logged to %s)" % (row["result"], (": " + row["error"]) if row.get("error") else "",
                                     os.path.relpath(a.log)))
    sys.exit(0 if row["result"].startswith("OK") else 1)


if __name__ == "__main__":
    main()
