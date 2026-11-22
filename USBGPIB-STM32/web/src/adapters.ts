// The two adapters: telling them apart over USB (app or bootloader), and checking their firmware images.

import { APP_FILTER, BOOT_FILTER, checkImage, type FlashOptions } from './dfu';

export type Board = 'stm32' | 'wch';

export interface BoardInfo {
  name: string;
  /** Prologix `++ver` prefix, also found in the image */
  verPrefix: string;
  /** bootloader's USB filter, for the chooser after a detach */
  bootFilter: USBDeviceFilter;
  flash: FlashOptions;
  /** what to do when the adapter isn't listed */
  buttonHelp: string;
}

export const BOARDS: Record<Board, BoardInfo> = {
  stm32: {
    name: 'USBGPIB-STM32',
    verPrefix: 'USBGPIB-STM32 version ',
    bootFilter: BOOT_FILTER,
    flash: { fallbackSize: 1024 },
    buttonHelp: 'hold SW1 while plugging it in (the LED stops blinking)',
  },
  wch: {
    name: 'LANGPIB-WCH',
    verPrefix: 'LANGPIB-WCH version ',
    bootFilter: APP_FILTER, // same VID:PID as the app
    flash: { blockSize: 4096 },
    buttonHelp: 'hold SW1 while plugging it in (D1 double-blinks)',
  },
};

export const WCH_BOOT_BCD = 0xdf10;

export interface Detected {
  board: Board;
  mode: 'app' | 'boot';
  /** firmware version from bcdDevice, app mode only */
  version?: string;
}

function bcdDevice(dev: USBDevice): number {
  return (dev.deviceVersionMajor << 8) | (dev.deviceVersionMinor << 4) | dev.deviceVersionSubminor;
}

function hasDfuMode(dev: USBDevice): boolean {
  return (dev.configuration?.interfaces ?? []).some((itf) =>
    itf.alternates.some((a) => a.interfaceClass === 0xfe && a.interfaceSubclass === 0x01 && a.interfaceProtocol === 2),
  );
}

/** Which adapter, and whether it runs the app or the bootloader; null if neither. */
export function identify(dev: USBDevice): Detected | null {
  if (dev.vendorId !== APP_FILTER.vendorId) return null;
  if (dev.productId === BOOT_FILTER.productId) return { board: 'stm32', mode: 'boot' };
  if (dev.productId !== APP_FILTER.productId) return null;
  const names = `${dev.manufacturerName ?? ''} ${dev.productName ?? ''}`;
  const wchNames = /LANGPIB|LAN GPIB/i.test(names);
  // LANGPIB-WCH's bootloader: its own bcdDevice, product "LAN GPIB adapter DFU", one DFU-mode interface. 1209:0001 is
  // a shared test PID, so another device's strings rule it out (unreadable strings don't)
  if ((bcdDevice(dev) === WCH_BOOT_BCD || hasDfuMode(dev)) && (!names.trim() || wchNames)) return { board: 'wch', mode: 'boot' };
  // bcdDevice is the version (major.minor); its low nibble is the USB interface set
  const version = `${dev.deviceVersionMajor.toString(16)}.${dev.deviceVersionMinor.toString(16)}`;
  if (wchNames) return { board: 'wch', mode: 'app', version };
  if (/USBGPIB|USB GPIB/i.test(names)) return { board: 'stm32', mode: 'app', version };
  return null;
}

/* --- images ------------------------------------------------------------------------------------------- */

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** CRC-32, zlib convention (crc32(b) === zlib.crc32(b)); pass the previous result to continue. */
export function crc32(data: Uint8Array, crc = 0): number {
  let c = ~crc >>> 0;
  for (const b of data) c = CRC_TABLE[(c ^ b) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// LANGPIB-WCH layout (Firmware/bootloader/image.h, tools/mkdfu.py)
const WCH_MAGIC = 0x4c475042;
const WCH_HDR_VER = 1;
const WCH_PAGE = 256;
const WCH_APP_MAX = 0x0807e000 - 0x08004100;
const SUFFIX = 16;

export interface WchImage {
  /** header + image: what goes to the bootloader (the .dfu without its suffix) */
  payload: Uint8Array;
  /** app_version from the header (bcdDevice), as "major.minor" */
  version: string;
}

/** Checks a LANGPIB-WCH .dfu (suffix, its CRC, the header and the image CRC) and strips the suffix. */
export function parseWchDfu(file: Uint8Array): WchImage {
  if (file.length < SUFFIX + WCH_PAGE + 4) throw new Error('The file is too small to be LANGPIB-WCH firmware.');
  const sfx = new DataView(file.buffer, file.byteOffset + file.length - SUFFIX, SUFFIX);
  const sig = String.fromCharCode(sfx.getUint8(8), sfx.getUint8(9), sfx.getUint8(10));
  if (sig !== 'UFD' || sfx.getUint8(11) !== SUFFIX) throw new Error('Not a .dfu file (no DFU suffix).');
  if (sfx.getUint16(6, true) !== 0x0100) throw new Error('Unexpected DFU suffix version.');
  if (sfx.getUint16(4, true) !== APP_FILTER.vendorId || sfx.getUint16(2, true) !== APP_FILTER.productId)
    throw new Error('The .dfu file is for another USB device.');
  // dwCRC: CRC-32 over everything before it, without the final inversion
  if ((crc32(file.subarray(0, file.length - 4)) ^ 0xffffffff) >>> 0 !== sfx.getUint32(12, true))
    throw new Error('The .dfu file is damaged (suffix CRC mismatch).');

  const payload = file.slice(0, file.length - SUFFIX);
  const h = new DataView(payload.buffer, 0, 20);
  if (h.getUint32(0, true) !== WCH_MAGIC) throw new Error('Not LANGPIB-WCH firmware (bad header magic).');
  if (h.getUint32(4, true) !== WCH_HDR_VER) throw new Error('Unknown LANGPIB-WCH header version.');
  const len = h.getUint32(8, true);
  if (len !== payload.length - WCH_PAGE || len === 0 || len > WCH_APP_MAX)
    throw new Error('The LANGPIB-WCH header length does not match the image.');
  if (crc32(payload.subarray(WCH_PAGE)) !== h.getUint32(12, true))
    throw new Error('The LANGPIB-WCH image is damaged (CRC mismatch).');
  const bcd = h.getUint32(16, true);
  return { payload, version: `${(bcd >> 8).toString(16)}.${((bcd >> 4) & 0xf).toString(16)}` };
}

/** The firmware's own version from its `++ver` string, e.g. "3.0", or null. */
export function imageVersion(image: Uint8Array, board: Board): string | null {
  const prefix = new TextEncoder().encode(BOARDS[board].verPrefix);
  outer: for (let i = 0; i + prefix.length < image.length; i++) {
    for (let j = 0; j < prefix.length; j++) if (image[i + j] !== prefix[j]) continue outer;
    let v = '';
    for (let k = i + prefix.length; k < image.length && /[0-9.]/.test(String.fromCharCode(image[k])); k++)
      v += String.fromCharCode(image[k]);
    return v || null;
  }
  return null;
}

export interface Prepared {
  payload: Uint8Array;
  version: string | null;
}

/** Validates a firmware file for board and returns what to download; throws with a readable reason. */
export function prepareImage(board: Board, file: Uint8Array): Prepared {
  if (board === 'wch') {
    const w = parseWchDfu(file);
    return { payload: w.payload, version: imageVersion(w.payload, 'wch') ?? w.version };
  }
  if (file.length >= SUFFIX && String.fromCharCode(file[file.length - 6], file[file.length - 7], file[file.length - 8]) === 'DFU')
    throw new Error('This is a .dfu file; USBGPIB-STM32 takes a .bin.');
  const problem = checkImage(file);
  if (problem) throw new Error(problem);
  return { payload: file, version: imageVersion(file, 'stm32') };
}
