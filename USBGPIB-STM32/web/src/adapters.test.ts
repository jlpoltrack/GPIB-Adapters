import { describe, expect, it } from 'vitest';
import { crc32, identify, imageVersion, parseWchDfu, prepareImage } from './adapters';

/** tools/mkdfu.py in TypeScript: 256-byte header, image, DFU suffix */
function mkdfu(image: Uint8Array, appVersion = 0x0010, vid = 0x1209, pid = 0x0001): Uint8Array {
  const hdr = new Uint8Array(256).fill(0xff);
  const h = new DataView(hdr.buffer);
  [0x4c475042, 1, image.length, crc32(image), appVersion].forEach((v, i) => h.setUint32(i * 4, v, true));
  const out = new Uint8Array(256 + image.length + 16);
  out.set(hdr);
  out.set(image, 256);
  const s = new DataView(out.buffer, 256 + image.length);
  s.setUint16(0, 0xffff, true);
  s.setUint16(2, pid, true);
  s.setUint16(4, vid, true);
  s.setUint16(6, 0x0100, true);
  out.set([0x55, 0x46, 0x44, 16], 256 + image.length + 8); // "UFD", bLength
  s.setUint32(12, (crc32(out.subarray(0, out.length - 4)) ^ 0xffffffff) >>> 0, true);
  return out;
}

function wchApp(len: number): Uint8Array {
  const b = new Uint8Array(len);
  for (let i = 0; i < len; i++) b[i] = (i * 7) & 0xff;
  b.set(new TextEncoder().encode('LANGPIB-WCH version 0.1 (Prologix GPIB-USB compatible)'), 100);
  return b;
}

const env = (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env ?? {};

async function readIfThere(path: string | undefined, fallback: string): Promise<Uint8Array | null> {
  const mod = 'node:fs'; // no @types/node here: an untyped dynamic import
  const fs = await import(/* @vite-ignore */ mod);
  const p = path ?? new URL(fallback, import.meta.url);
  if (path && !fs.existsSync(p)) throw new Error(`${path} (from the environment) is missing`); // CI: never pass vacuously
  return fs.existsSync(p) ? new Uint8Array(fs.readFileSync(p)) : null;
}

describe('crc32', () => {
  it('matches zlib', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
    expect(crc32(new Uint8Array(0))).toBe(0);
  });
});

describe('parseWchDfu', () => {
  it('strips the suffix and reads the header version', () => {
    const app = wchApp(5000);
    const w = parseWchDfu(mkdfu(app, 0x0123));
    expect(w.payload.length).toBe(256 + 5000);
    expect(w.payload.subarray(256)).toEqual(app);
    expect(w.version).toBe('1.2');
  });

  it('rejects damaged or foreign files', () => {
    const good = mkdfu(wchApp(3000));
    const flip = (i: number) => {
      const b = good.slice();
      b[i] ^= 1;
      return b;
    };
    expect(() => parseWchDfu(flip(1000))).toThrow(/suffix CRC/);
    expect(() => parseWchDfu(flip(good.length - 6))).toThrow(/no DFU suffix/);
    expect(() => parseWchDfu(mkdfu(wchApp(3000), 0x10, 0x0483, 0xdf11))).toThrow(/another USB device/);
    expect(() => parseWchDfu(wchApp(3000))).toThrow(/no DFU suffix/);
  });

  it('checks the header magic, length and image CRC', () => {
    const resuffix = (payload: Uint8Array) => {
      // a valid suffix around a bad payload, so only the header checks can catch it
      const f = mkdfu(new Uint8Array(0));
      const out = new Uint8Array(payload.length + 16);
      out.set(payload);
      out.set(f.subarray(f.length - 16), payload.length);
      const s = new DataView(out.buffer, payload.length);
      s.setUint32(12, (crc32(out.subarray(0, out.length - 4)) ^ 0xffffffff) >>> 0, true);
      return out;
    };
    const payload = mkdfu(wchApp(3000)).subarray(0, 256 + 3000);
    const edit = (off: number, v: number) => {
      const p = payload.slice();
      new DataView(p.buffer).setUint32(off, v, true);
      return resuffix(p);
    };
    expect(parseWchDfu(resuffix(payload)).payload).toEqual(payload);
    expect(() => parseWchDfu(edit(0, 0x12345678))).toThrow(/magic/);
    expect(() => parseWchDfu(edit(8, 2999))).toThrow(/length/);
    expect(() => parseWchDfu(edit(12, 0))).toThrow(/CRC mismatch/);
  });

  it('accepts the real build output (make in LANGPIB-WCH/Firmware, or WCH_DFU)', async () => {
    const f = await readIfThere(env.WCH_DFU, '../../../LANGPIB-WCH/Firmware/build/langpib.dfu');
    if (!f) return;
    const p = prepareImage('wch', f);
    expect(p.payload.length).toBe(f.length - 16);
    expect(p.version).toMatch(/^\d+\.\d+$/);
  });
});

describe('prepareImage', () => {
  it('reads the version from the ++ver string', () => {
    expect(imageVersion(wchApp(400), 'wch')).toBe('0.1');
    expect(imageVersion(wchApp(400), 'stm32')).toBeNull();
  });

  it('does not take one board firmware for the other', () => {
    expect(() => prepareImage('stm32', mkdfu(wchApp(3000)))).toThrow(/\.dfu file/);
    expect(() => prepareImage('wch', wchApp(3000))).toThrow(/no DFU suffix/);
  });

  it('accepts the real USBGPIB-STM32 build (make in USBGPIB-STM32/Firmware, or STM32_BIN)', async () => {
    const f = await readIfThere(env.STM32_BIN, '../../Firmware/build/usbgpib.bin');
    if (!f) return;
    const p = prepareImage('stm32', f);
    expect(p.payload).toEqual(f);
    expect(p.version).toMatch(/^\d+\.\d+$/);
  });
});

interface Fake {
  vendorId?: number;
  productId?: number;
  bcd?: number;
  manufacturerName?: string;
  productName?: string;
  dfuProtocol?: number;
}

function dev(f: Fake): USBDevice {
  const bcd = f.bcd ?? 0;
  return {
    vendorId: f.vendorId ?? 0x1209,
    productId: f.productId ?? 0x0001,
    deviceVersionMajor: bcd >> 8,
    deviceVersionMinor: (bcd >> 4) & 0xf,
    deviceVersionSubminor: bcd & 0xf,
    manufacturerName: f.manufacturerName,
    productName: f.productName,
    configuration: {
      interfaces: [
        { interfaceNumber: 0, alternates: [{ interfaceClass: 0xfe, interfaceSubclass: 3, interfaceProtocol: 1 }] },
        ...(f.dfuProtocol
          ? [{ interfaceNumber: 1, alternates: [{ interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: f.dfuProtocol }] }]
          : []),
      ],
    },
  } as unknown as USBDevice;
}

describe('identify', () => {
  it('tells the apps apart by their USB strings and reads the version from bcdDevice', () => {
    expect(identify(dev({ bcd: 0x0301, manufacturerName: 'USBGPIB-STM32', productName: 'USB GPIB adapter', dfuProtocol: 1 })))
      .toEqual({ board: 'stm32', mode: 'app', version: '3.0' });
    expect(identify(dev({ bcd: 0x0012, manufacturerName: 'LANGPIB-WCH', productName: 'LAN GPIB adapter', dfuProtocol: 1 })))
      .toEqual({ board: 'wch', mode: 'app', version: '0.1' });
    expect(identify(dev({ bcd: 0x0010, productName: 'LAN GPIB adapter (simulated GPIB)' }))?.board).toBe('wch');
  });

  it('finds the bootloaders', () => {
    expect(identify(dev({ productId: 0xdb42, productName: 'DAPBoot DFU Bootloader', dfuProtocol: 2 })))
      .toEqual({ board: 'stm32', mode: 'boot' });
    // LANGPIB-WCH's shares the app's VID:PID: bcdDevice 0xDF10 and a DFU-mode interface
    expect(identify(dev({ bcd: 0xdf10, manufacturerName: 'LANGPIB-WCH', productName: 'LAN GPIB adapter DFU', dfuProtocol: 2 })))
      .toEqual({ board: 'wch', mode: 'boot' });
    expect(identify(dev({ bcd: 0xdf10 }))).toEqual({ board: 'wch', mode: 'boot' });
    expect(identify(dev({ dfuProtocol: 2 }))).toEqual({ board: 'wch', mode: 'boot' });
    // another 1209:0001 device in DFU mode (the shared test PID) isn't taken for it
    expect(identify(dev({ bcd: 0xdf10, manufacturerName: 'Someone', productName: 'Other DFU', dfuProtocol: 2 }))).toBeNull();
  });

  it('ignores other devices', () => {
    expect(identify(dev({ productName: 'Something else' }))).toBeNull();
    expect(identify(dev({ vendorId: 0x0483, productId: 0xdf11 }))).toBeNull();
  });
});
