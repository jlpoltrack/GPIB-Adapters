#!/usr/bin/env python3
"""App .bin (linked at 0x08004100) -> .dfu for the DFU bootloader, and optionally an Intel HEX for SDI.

The image gets the 256-byte header of bootloader/image.h in front (magic, version, length, zlib CRC-32, app version)
and, in the .dfu, the DFU 1.1 suffix. dfu-util downloads header + image from 0x08004000; the bootloader writes the
header page last, after checking the CRC.
"""
import argparse
import struct
import sys
import zlib

APP_HDR_ADDR = 0x08004000
APP_START = 0x08004100
APP_END = 0x0807E000
APP_MAGIC = 0x4C475042
APP_HDR_VER = 1
PAGE = 256


def header(image, app_version):
    h = struct.pack('<5I', APP_MAGIC, APP_HDR_VER, len(image), zlib.crc32(image), app_version)
    return h + b'\xff' * (PAGE - len(h))


def dfu_suffix(data, vid, pid):
    # bcdDevice 0xFFFF (any), PID, VID, bcdDFU 1.00, "UFD", length 16; dwCRC: CRC-32 without the final inversion
    s = struct.pack('<4H3sB', 0xFFFF, pid, vid, 0x0100, b'UFD', 16)
    return s + struct.pack('<I', zlib.crc32(data + s) ^ 0xFFFFFFFF)


def ihex(data, base):
    out, ext = [], None

    def rec(kind, addr, payload):
        b = bytes([len(payload), (addr >> 8) & 0xFF, addr & 0xFF, kind]) + payload
        out.append(':' + (b + bytes([(-sum(b)) & 0xFF])).hex().upper())

    for off in range(0, len(data), 16):
        a = base + off
        if a >> 16 != ext:
            ext = a >> 16
            rec(4, 0, struct.pack('>H', ext))
        rec(0, a & 0xFFFF, data[off:off + 16])
    rec(1, 0, b'')
    return '\n'.join(out) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('bin', help='app binary linked at 0x%08X' % APP_START)
    p.add_argument('dfu', help='output .dfu')
    p.add_argument('--hex', help='also write header + image as Intel HEX at 0x%08X (SDI)' % APP_HDR_ADDR)
    p.add_argument('--vid', type=lambda s: int(s, 0), default=0x1209)
    p.add_argument('--pid', type=lambda s: int(s, 0), default=0x0001)
    p.add_argument('--version', type=lambda s: int(s, 0), default=0, help='app bcdDevice, for the header')
    a = p.parse_args()

    image = open(a.bin, 'rb').read()
    if not 0 < len(image) <= APP_END - APP_START:
        sys.exit('mkdfu: image is %d bytes, the app area holds %d' % (len(image), APP_END - APP_START))
    data = header(image, a.version) + image
    with open(a.dfu, 'wb') as f:
        f.write(data + dfu_suffix(data, a.vid, a.pid))
    if a.hex:
        with open(a.hex, 'w') as f:
            f.write(ihex(data, APP_HDR_ADDR))


if __name__ == '__main__':
    main()
