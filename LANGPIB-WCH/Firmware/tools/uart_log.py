#!/usr/bin/env python3
"""Passive UART log through the SDI probe's bridge (no reset, unlike `bringup.py log`), appended to FILE and stdout.

  python3 tools/uart_log.py FILE
"""
import os
import sys

import serial

sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "Reference-WCH", "tools")))
import sdi_probe  # noqa: E402

_, uart = sdi_probe.find_ports()
s = serial.Serial(uart, 115200, timeout=0.2)
with open(sys.argv[1], "a") as f:
    while True:
        d = s.read(4096).decode(errors="replace")
        if d:
            f.write(d)
            f.flush()
            sys.stdout.write(d)
            sys.stdout.flush()
