import { describe, expect, it } from 'vitest';
import { checkImage, detach, flash } from './dfu';
import { BOARDS } from './adapters';

function image(len: number, sp = 0x2000_5000, reset = 0x0800_2101): Uint8Array {
  const b = new Uint8Array(len);
  const v = new DataView(b.buffer);
  v.setUint32(0, sp, true);
  v.setUint32(4, reset, true);
  for (let i = 8; i < len; i++) b[i] = i & 0xff;
  return b;
}

/** dapboot's (and TinyUSB's) DFU state machine, as far as a download goes; manifestStatus 0 = image accepted */
function fakeBootloader(transferSize = 1024, manifestStatus = 0) {
  const written: number[] = [];
  const blocks: [number, number][] = []; // [wValue, length] of each download
  let status = 0;
  let state = 2; // dfuIDLE
  let pending: Uint8Array | null = null;
  let gone = false;
  const functional = [9, 2, 27, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 0xfe, 1, 2, 0, 9, 0x21, 0x0b, 255, 0, transferSize & 0xff, transferSize >> 8, 0x10, 0x01];
  const dev = {
    opened: false,
    configuration: { interfaces: [{ interfaceNumber: 0, alternates: [{ interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 2 }] }] },
    async open() { this.opened = true; },
    async close() { this.opened = false; },
    async selectConfiguration() {},
    async claimInterface() {},
    async selectAlternateInterface() {},
    async controlTransferOut(setup: USBControlTransferParameters, data?: BufferSource) {
      if (gone) throw new Error('disconnected');
      if (setup.request === 1) {
        const d = data ? new Uint8Array(data as ArrayBuffer) : new Uint8Array(0);
        expect(d.length).toBeLessThanOrEqual(transferSize);
        blocks.push([setup.value, d.length]);
        if (d.length) {
          pending = d;
          state = 3; // DNLOAD_SYNC
        } else {
          state = 6; // MANIFEST_SYNC
        }
      }
      return { status: 'ok', bytesWritten: 0 };
    },
    async controlTransferIn(setup: USBControlTransferParameters) {
      if (gone) throw new Error('disconnected');
      if (setup.request === 6) return { status: 'ok', data: new DataView(Uint8Array.from(functional).buffer) };
      // GETSTATUS
      let reply = state;
      if (state === 3) {
        reply = state = 4; // DNBUSY; the write happens once this request completes
        queueMicrotask(() => {
          written.push(...pending!);
          state = 5;
        });
      } else if (state === 6 && manifestStatus) {
        reply = state = 10; // dfuERROR: the bootloader refused the image
        status = manifestStatus;
      } else if (state === 6) {
        reply = 7; // MANIFEST, then it resets
        queueMicrotask(() => (gone = true));
      }
      return { status: 'ok', data: new DataView(Uint8Array.from([status, 1, 0, 0, reply, 0]).buffer) };
    },
  };
  return { dev: dev as unknown as USBDevice, written, blocks };
}

describe('checkImage', () => {
  it('accepts an application image', () => {
    expect(checkImage(image(20000))).toBeNull();
  });
  it('rejects images that do not fit or are not linked behind dapboot', () => {
    expect(checkImage(image(60 * 1024))).toMatch(/application area/);
    expect(checkImage(image(100, 0x1234))).toMatch(/stack pointer/);
    expect(checkImage(image(100, 0x2000_5000, 0x0800_0101))).toMatch(/0x08002000/);
  });
});

describe('flash', () => {
  it('downloads the whole image in transfer-size blocks and survives the reset', async () => {
    const { dev, written } = fakeBootloader(1024);
    const img = image(2500);
    const progress: number[] = [];
    await flash(dev, img, (done) => progress.push(done));
    expect(Uint8Array.from(written)).toEqual(img);
    expect(progress).toEqual([1024, 2048, 2500]);
  });

  it('uses the transfer size the bootloader reports', async () => {
    const { dev, written } = fakeBootloader(256);
    const img = image(700);
    await flash(dev, img, () => {});
    expect(written.length).toBe(700);
  });
});

describe('flash to the LANGPIB-WCH bootloader', () => {
  const wch = BOARDS.wch.flash;
  const payload = (len: number) => Uint8Array.from({ length: len }, (_, i) => (i * 13) & 0xff);

  it('sends 4 KB blocks numbered from 0 (block n = 0x08004000 + n * 4 KB), then the empty download', async () => {
    const { dev, written, blocks } = fakeBootloader(4096);
    const img = payload(256 + 9000);
    await flash(dev, img, () => {}, wch);
    expect(Uint8Array.from(written)).toEqual(img);
    expect(blocks).toEqual([[0, 4096], [1, 4096], [2, 1064], [3, 0]]);
  });

  it('refuses a bootloader with another block size', async () => {
    const { dev, blocks } = fakeBootloader(1024);
    await expect(flash(dev, payload(5000), () => {}, wch)).rejects.toThrow(/1024-byte blocks/);
    expect(blocks).toEqual([]);
  });

  it('reports a rejected image (header CRC check at manifestation)', async () => {
    const { dev } = fakeBootloader(4096, 0x0a); // errFIRMWARE
    await expect(flash(dev, payload(5000), () => {}, wch)).rejects.toThrow(/error 10 after writing/);
  });
});

describe('detach', () => {
  it('sends DFU_DETACH to the runtime interface', async () => {
    const sent: USBControlTransferParameters[] = [];
    const dev = {
      opened: false,
      configuration: {
        interfaces: [
          { interfaceNumber: 0, alternates: [{ interfaceClass: 0xfe, interfaceSubclass: 3, interfaceProtocol: 1 }] },
          { interfaceNumber: 3, alternates: [{ interfaceClass: 0xfe, interfaceSubclass: 1, interfaceProtocol: 1 }] },
        ],
      },
      async open() {},
      async close() {},
      async selectConfiguration() {},
      async claimInterface(n: number) { expect(n).toBe(3); },
      async controlTransferOut(s: USBControlTransferParameters) { sent.push(s); return { status: 'ok' }; },
    } as unknown as USBDevice;
    await detach(dev);
    expect(sent).toEqual([{ requestType: 'class', recipient: 'interface', request: 0, value: 1000, index: 3 }]);
  });
});
