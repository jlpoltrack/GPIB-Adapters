// Firmware update over WebUSB: DFU_DETACH to the app's DFU runtime interface, then a DFU 1.1 download to dapboot.

export const APP_FILTER = { vendorId: 0x1209, productId: 0x0001 };
export const BOOT_FILTER = { vendorId: 0x1209, productId: 0xdb42 };

const DETACH = 0, DNLOAD = 1, GETSTATUS = 3, CLRSTATUS = 4, ABORT = 6;
const STATE = { dfuIDLE: 2, dnBUSY: 4, dnloadIDLE: 5, manifestSYNC: 6, manifest: 7, error: 10 } as const;

const APP_BASE = 0x0800_2000;
const APP_MAX = 54 * 1024;

/** Checks a .bin is an application image for this board: fits the app area, vectors point at RAM and app flash. */
export function checkImage(image: Uint8Array): string | null {
  if (image.length < 8) return 'The file is too small to be firmware.';
  if (image.length > APP_MAX) return `The file is ${image.length} bytes; the application area is ${APP_MAX}.`;
  const v = new DataView(image.buffer, image.byteOffset, 8);
  const sp = v.getUint32(0, true), reset = v.getUint32(4, true);
  if (sp < 0x2000_0000 || sp > 0x2000_5000) return 'Not an application image (bad stack pointer).';
  if (reset < APP_BASE || reset >= APP_BASE + APP_MAX) return 'Not built for 0x08002000 (behind dapboot).';
  return null;
}

function dfuInterface(dev: USBDevice, protocol: 1 | 2): number {
  for (const itf of dev.configuration?.interfaces ?? [])
    for (const alt of itf.alternates)
      if (alt.interfaceClass === 0xfe && alt.interfaceSubclass === 0x01 && alt.interfaceProtocol === protocol)
        return itf.interfaceNumber;
  throw new Error(protocol === 1 ? 'No DFU runtime interface on this device.' : 'No DFU interface on this device.');
}

async function openDevice(dev: USBDevice): Promise<void> {
  if (!dev.opened) await dev.open();
  if (!dev.configuration) await dev.selectConfiguration(1);
}

/** Asks the running adapter to restart into dapboot. */
export async function detach(dev: USBDevice): Promise<void> {
  await openDevice(dev);
  const itf = dfuInterface(dev, 1);
  await dev.claimInterface(itf);
  try {
    await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: DETACH, value: 1000, index: itf });
  } finally {
    await dev.close().catch(() => {}); // it is already resetting
  }
}

interface Status {
  status: number;
  pollMs: number;
  state: number;
}

async function getStatus(dev: USBDevice, itf: number): Promise<Status> {
  const r = await dev.controlTransferIn({ requestType: 'class', recipient: 'interface', request: GETSTATUS, value: 0, index: itf }, 6);
  const d = r.data!;
  return { status: d.getUint8(0), pollMs: d.getUint8(1) | (d.getUint8(2) << 8) | (d.getUint8(3) << 16), state: d.getUint8(4) };
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function waitState(dev: USBDevice, itf: number, want: number): Promise<void> {
  for (let i = 0; i < 1000; i++) {
    const s = await getStatus(dev, itf);
    if (s.status !== 0) throw new Error(`The bootloader reported error ${s.status} (state ${s.state}).`);
    if (s.state === want) return;
    await sleep(s.pollMs);
  }
  throw new Error('The bootloader did not finish writing.');
}

/** wTransferSize from the DFU functional descriptor, 1024 (dapboot's) if it can't be read. */
async function transferSize(dev: USBDevice): Promise<number> {
  try {
    const r = await dev.controlTransferIn({ requestType: 'standard', recipient: 'device', request: 6, value: 0x0200, index: 0 }, 255);
    const d = new Uint8Array(r.data!.buffer);
    for (let i = 0; i + 1 < d.length && d[i] > 0; i += d[i])
      if (d[i + 1] === 0x21 && d[i] >= 7) return d[i + 5] | (d[i + 6] << 8);
  } catch {
    /* fall back */
  }
  return 1024;
}

/** Writes image to dapboot; the adapter restarts into it when done. */
export async function flash(dev: USBDevice, image: Uint8Array, progress: (done: number, total: number) => void): Promise<void> {
  await openDevice(dev);
  const itf = dfuInterface(dev, 2);
  await dev.claimInterface(itf);
  await dev.selectAlternateInterface(itf, 0);

  let s = await getStatus(dev, itf);
  if (s.state === STATE.error) {
    await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: CLRSTATUS, value: 0, index: itf });
    s = await getStatus(dev, itf);
  }
  if (s.state !== STATE.dfuIDLE) {
    await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: ABORT, value: 0, index: itf });
  }

  const size = await transferSize(dev);
  let block = 0;
  for (let off = 0; off < image.length; off += size, block++) {
    const chunk = image.slice(off, off + size);
    await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: DNLOAD, value: block, index: itf }, chunk);
    await waitState(dev, itf, STATE.dnloadIDLE);
    progress(Math.min(off + size, image.length), image.length);
  }

  // an empty download ends it; the bootloader then manifests and resets into the new firmware
  await dev.controlTransferOut({ requestType: 'class', recipient: 'interface', request: DNLOAD, value: block, index: itf });
  try {
    for (let i = 0; i < 50; i++) {
      s = await getStatus(dev, itf);
      if (s.status !== 0) throw new Error(`The bootloader reported error ${s.status} after writing.`);
      if (s.state === STATE.dfuIDLE) break;
      await sleep(Math.max(s.pollMs, 10));
    }
  } catch (e) {
    if (e instanceof Error && e.message.startsWith('The bootloader')) throw e;
    // the device went away to restart: that is the normal end
  }
  await dev.close().catch(() => {});
}
