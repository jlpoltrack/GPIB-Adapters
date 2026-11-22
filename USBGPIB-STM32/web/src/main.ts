import './style.css';
import { PrologixClient } from './prologix';
import { APP_FILTER, BOOT_FILTER, detach, flash } from './dfu';
import { BOARDS, identify, prepareImage, type Board, type Prepared } from './adapters';

const USB_FILTER = { usbVendorId: 0x1209, usbProductId: 0x0001 };

const $ = <T extends HTMLElement = HTMLElement>(id: string) => document.getElementById(id) as T;

let port: SerialPort | null = null;
let reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
let client: PrologixClient | null = null;
let quiet = 0; // > 0 while the page talks to the adapter itself: keep that out of the terminal
let capture: number[] | null = null; // bytes of a listen-only capture in progress
let captured: Uint8Array<ArrayBuffer> | null = null;

/* --- terminal ----------------------------------------------------------------------------------------- */

function show(text: string, cls?: string): void {
  const log = $('log');
  const span = document.createElement('span');
  if (cls) span.className = cls;
  span.textContent = text;
  log.append(span);
  log.scrollTop = log.scrollHeight;
}

function printable(bytes: Uint8Array): string {
  let s = '';
  for (const b of bytes) {
    if (b === 10 || b === 13 || b === 9 || (b >= 32 && b < 127)) s += String.fromCharCode(b);
    else s += `\\x${b.toString(16).padStart(2, '0')}`;
  }
  return s;
}

async function quietly<T>(op: () => Promise<T>): Promise<T> {
  quiet++;
  try {
    return await op();
  } finally {
    quiet--;
  }
}

/* --- connection --------------------------------------------------------------------------------------- */

async function readLoop(p: SerialPort): Promise<void> {
  while (p.readable) {
    reader = p.readable.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) client?.feed(value);
      }
    } catch {
      break; // device unplugged
    } finally {
      reader.releaseLock();
    }
  }
  disconnected();
}

async function connect(): Promise<void> {
  const p = await navigator.serial.requestPort({ filters: [USB_FILTER] });
  await p.open({ baudRate: 115200 });
  await p.setSignals({ dataTerminalReady: true });
  port = p;
  client = new PrologixClient(
    async (bytes) => {
      const w = p.writable!.getWriter();
      try {
        await w.write(bytes);
      } finally {
        w.releaseLock();
      }
    },
    (bytes) => {
      if (capture) {
        for (const b of bytes) capture.push(b);
        $('capture-count').textContent = String(capture.length);
      } else if (!quiet) show(printable(bytes));
    },
  );
  void readLoop(p);
  document.body.classList.add('connected');
  $('connect').textContent = 'Disconnect';
  await loadSettings();
  await refreshStatus();
}

async function disconnect(): Promise<void> {
  const p = port;
  port = null;
  await reader?.cancel().catch(() => {});
  await p?.close().catch(() => {});
  disconnected();
}

function disconnected(): void {
  port = null;
  client = null;
  document.body.classList.remove('connected');
  $('connect').textContent = 'Connect';
  $('version').textContent = 'Not connected';
}

/* --- settings ----------------------------------------------------------------------------------------- */

const settingsForm = () => $<HTMLFormElement>('settings');
const adapterForm = () => $<HTMLFormElement>('adapter');

function field(form: HTMLFormElement, name: string): HTMLInputElement | HTMLSelectElement {
  return form.elements.namedItem(name) as HTMLInputElement | HTMLSelectElement;
}

async function loadSettings(): Promise<void> {
  if (!client) return;
  const c = client;
  await quietly(async () => {
    $('version').textContent = (await c.query('ver')) ?? 'No answer: is this the adapter?';
    for (const form of [settingsForm(), adapterForm()]) {
      for (const el of Array.from(form.elements) as (HTMLInputElement | HTMLSelectElement)[]) {
        if (!el.name) continue;
        const v = await c.query(el.name);
        if (v === null) continue;
        if (el instanceof HTMLInputElement && el.type === 'checkbox') el.checked = v === '1';
        else el.value = v;
      }
    }
  });
}

async function saveField(ev: Event): Promise<void> {
  const el = ev.target as HTMLInputElement | HTMLSelectElement;
  if (!client || !el.name) return;
  const value = el instanceof HTMLInputElement && el.type === 'checkbox' ? (el.checked ? '1' : '0') : el.value.trim();
  if (value === '') return;
  await client.command(`${el.name} ${value}`);
  const c = client;
  const now = await quietly(() => c.query(el.name)); // show what the adapter accepted
  if (now === null) return;
  if (el instanceof HTMLInputElement && el.type === 'checkbox') el.checked = now === '1';
  else el.value = now;
}

/* --- bus ---------------------------------------------------------------------------------------------- */

async function scan(): Promise<void> {
  if (!client) return;
  const c = client;
  const body = $('instruments').querySelector('tbody')!;
  body.innerHTML = '<tr><td colspan="3" class="hint">Scanning…</td></tr>';
  await quietly(async () => {
    const list = await c.query('scan', 3000);
    const addrs = (list ?? '').split(/\s+/).filter(Boolean);
    body.innerHTML = '';
    if (!addrs.length) {
      body.innerHTML = `<tr><td colspan="3" class="hint">${list === 'busy' ? 'Bus busy, try again.' : 'No instruments answered.'}</td></tr>`;
      return;
    }
    const saved = await c.query('addr');
    for (const a of addrs) {
      await c.command(`addr ${a}`);
      const idn = (await c.ask('*IDN?')) ?? '(no answer to *IDN?)';
      const tr = document.createElement('tr');
      tr.innerHTML = '<td></td><td></td><td><button class="small">Use</button></td>';
      tr.children[0].textContent = a;
      tr.children[1].textContent = idn;
      tr.querySelector('button')!.addEventListener('click', async () => {
        await c.command(`addr ${a}`);
        field(settingsForm(), 'addr').value = a;
      });
      body.append(tr);
    }
    if (saved) await c.command(`addr ${saved}`);
  });
  await refreshStatus();
}

async function refreshStatus(): Promise<void> {
  if (!client) return;
  const c = client;
  await quietly(async () => {
    $('srq').textContent = (await c.query('srq')) === '1' ? 'asserted' : 'idle';
    $('err').textContent = (await c.query('err')) ?? '–';
  });
}

/* --- terminal commands -------------------------------------------------------------------------------- */

async function sendLine(read: 'none' | 'eoi'): Promise<void> {
  const input = $<HTMLInputElement>('line');
  const text = input.value;
  if (!client || !text) return;
  show(`> ${text}\n`, 'sent');
  if (text.startsWith('++')) {
    await client.command(text.slice(2));
  } else {
    await client.send(text);
    if (read === 'eoi') {
      const tmo = Number(field(settingsForm(), 'read_tmo_ms').value) || 500;
      await client.read('eoi', tmo);
    }
  }
  input.select();
}

async function readOnce(): Promise<void> {
  if (!client) return;
  const tmo = Number(field(settingsForm(), 'read_tmo_ms').value) || 500;
  await client.read('eoi', tmo);
}

/* --- capture ---------------------------------------------------------------------------------------- */

async function captureStart(): Promise<void> {
  if (!client || capture) return;
  capture = [];
  captured = null;
  $('capture-count').textContent = '0';
  $<HTMLButtonElement>('capture-save').disabled = true;
  await client.command('lon 1');
}

async function captureStop(): Promise<void> {
  if (!client || !capture) return;
  await client.writeRaw(Uint8Array.of(10)); // any input ends ++lon; an empty line does nothing else
  await new Promise((r) => setTimeout(r, 200));
  captured = Uint8Array.from(capture);
  capture = null;
  $<HTMLButtonElement>('capture-save').disabled = captured.length === 0;
}

function captureSave(): void {
  if (!captured) return;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([captured], { type: 'application/octet-stream' }));
  a.download = $<HTMLInputElement>('capture-name').value || 'capture.plt';
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/* --- firmware ----------------------------------------------------------------------------------------- */

interface Release {
  version: string;
  date?: string;
  files: Partial<Record<Board, string>>;
}

let release: Release | null = null;
let awaiting: Board | null = null; // detached, but the updater needs a second pick in the chooser
let pending: Prepared | null = null; // the image checked before detaching

function fwStatus(text: string): void {
  $('fw-status').textContent = text;
}

function setAwaiting(board: Board | null): void {
  awaiting = board;
  $('fw-update').textContent = board ? `Continue: choose the ${BOARDS[board].name} updater` : 'Update firmware';
}

async function findRelease(): Promise<void> {
  try {
    const r = await fetch('./firmware/info.json', { cache: 'no-store' });
    if (!r.ok) return;
    release = (await r.json()) as Release;
    $<HTMLInputElement>('fw-latest').disabled = false;
    $<HTMLInputElement>('fw-latest').checked = true;
    $('fw-info').textContent = `${release.version}${release.date ? ` (${release.date})` : ''}`;
  } catch {
    /* only on the published site */
  }
}

async function firmwareFor(board: Board): Promise<Prepared> {
  let bytes: Uint8Array;
  if ($<HTMLInputElement>('fw-latest').checked) {
    const name = release?.files[board];
    if (!name) throw new Error(`This page has no ${BOARDS[board].name} firmware.`);
    const r = await fetch(`./firmware/${name}`, { cache: 'no-store' });
    if (!r.ok) throw new Error(`Could not download ${name}.`);
    bytes = new Uint8Array(await r.arrayBuffer());
  } else {
    const f = $<HTMLInputElement>('fw-file').files?.[0];
    if (!f) throw new Error('Choose a firmware file first (.bin for USBGPIB-STM32, .dfu for LANGPIB-WCH).');
    bytes = new Uint8Array(await f.arrayBuffer());
  }
  return prepareImage(board, bytes);
}

function sourceLabel(p: Prepared): string {
  const v = p.version ? `firmware ${p.version}` : 'the firmware';
  return $<HTMLInputElement>('fw-latest').checked && release ? `${v} from release ${release.version}` : `${v} from the file`;
}

/** Polls for the board's updater after a detach; null if it doesn't show up (no permission for it yet). */
async function waitForUpdater(board: Board, ms = 8000): Promise<USBDevice | null> {
  for (const end = Date.now() + ms; Date.now() < end; ) {
    for (const d of await navigator.usb.getDevices()) {
      const id = identify(d);
      if (id?.board === board && id.mode === 'boot') return d;
    }
    await new Promise((r) => setTimeout(r, 250));
  }
  return null;
}

function boardFromVer(ver: string): Board | null {
  for (const b of Object.keys(BOARDS) as Board[]) if (ver.startsWith(BOARDS[b].verPrefix)) return b;
  return null;
}

/** After a detach: flash if the updater shows up by itself, else ask for a second pick. */
async function afterDetach(board: Board): Promise<void> {
  fwStatus(`${BOARDS[board].name} is restarting into its updater…`);
  const dev = await waitForUpdater(board);
  if (dev) return writeFirmware(dev, board);
  setAwaiting(board);
  fwStatus(`${BOARDS[board].name} is restarting into its updater. Click Continue and pick it in the list.`);
}

async function writeFirmware(dev: USBDevice, board: Board): Promise<void> {
  const img = pending ?? (await firmwareFor(board));
  pending = null;
  setAwaiting(null);
  const bar = $<HTMLProgressElement>('fw-progress');
  bar.value = 0;
  fwStatus(`Writing ${sourceLabel(img)} to ${BOARDS[board].name}…`);
  await flash(dev, img.payload, (done, total) => (bar.value = done / total), BOARDS[board].flash);
  fwStatus(`Done: ${BOARDS[board].name} now has ${sourceLabel(img)} (${img.payload.length} bytes). It restarts by itself.`);
}

async function fwUpdate(): Promise<void> {
  if (!('usb' in navigator)) throw new Error('WebUSB is not available: use Chrome or Edge on a desktop computer.');

  if (awaiting) {
    const board = awaiting;
    const dev = await navigator.usb.requestDevice({ filters: [BOARDS[board].bootFilter] });
    const id = identify(dev);
    if (id?.board !== board || id.mode !== 'boot') throw new Error(`That is not the ${BOARDS[board].name} updater.`);
    return writeFirmware(dev, board);
  }

  if (client) { // the settings' serial port is open: restart over it (works on every OS without a WebUSB driver)
    const board = boardFromVer($('version').textContent ?? '');
    if (!board) throw new Error('Disconnect the serial port first: the adapter did not say which one it is.');
    pending = await firmwareFor(board);
    await client.command('dfu');
    await disconnect();
    return afterDetach(board);
  }

  const dev = await navigator.usb.requestDevice({ filters: [APP_FILTER, BOOT_FILTER] });
  const id = identify(dev);
  if (!id) throw new Error(`${dev.productName ?? 'This device'} is not an adapter this page knows.`);
  pending = await firmwareFor(id.board); // check the image before restarting anything
  if (id.mode === 'boot') return writeFirmware(dev, id.board);
  fwStatus(`Found ${BOARDS[id.board].name} running firmware ${id.version}.`);
  await detach(dev);
  return afterDetach(id.board);
}

function fwGuard(op: () => Promise<void>): () => void {
  return () => {
    const button = $<HTMLButtonElement>('fw-update');
    button.disabled = true;
    op()
      .catch((e: unknown) => {
        if (!awaiting) pending = null;
        if (e instanceof DOMException && e.name === 'NotFoundError') {
          fwStatus(
            awaiting
              ? 'Nothing chosen. Pick the updater in the list, or reload the page to start over.'
              : 'Nothing chosen. If the adapter was not in the list, see below: hold SW1 while plugging it in.',
          );
          return;
        }
        fwStatus(e instanceof Error ? e.message : String(e));
      })
      .finally(() => (button.disabled = false));
  };
}

/* --- wiring ------------------------------------------------------------------------------------------- */

function guard(op: () => Promise<void>): () => void {
  return () => {
    op().catch((e: unknown) => show(`! ${e instanceof Error ? e.message : String(e)}\n`, 'error'));
  };
}

if (!('serial' in navigator)) {
  $('unsupported').hidden = false;
  $<HTMLButtonElement>('connect').disabled = true;
} else {
  $('connect').addEventListener('click', guard(() => (port ? disconnect() : connect())));
  navigator.serial.addEventListener('disconnect', (e) => {
    if (e.target === port) disconnected();
  });
}

settingsForm().addEventListener('change', (e) => guard(() => saveField(e))());
adapterForm().addEventListener('change', (e) => guard(() => saveField(e))());
settingsForm().addEventListener('submit', (e) => e.preventDefault());
$('scan').addEventListener('click', guard(scan));
$('refresh').addEventListener('click', guard(refreshStatus));
$('clear').addEventListener('click', () => ($('log').textContent = ''));
$('term').addEventListener('submit', (e) => {
  e.preventDefault();
  guard(() => sendLine('none'))();
});
$('ask').addEventListener('click', guard(() => sendLine('eoi')));
$('read').addEventListener('click', guard(readOnce));
$('capture-start').addEventListener('click', guard(captureStart));
$('capture-stop').addEventListener('click', guard(captureStop));
$('capture-save').addEventListener('click', captureSave);
$('fw-update').addEventListener('click', fwGuard(fwUpdate));
$('fw-file').addEventListener('change', () => ($<HTMLInputElement>('fw-file-radio').checked = true));
if (!('usb' in navigator)) {
  $('unsupported').hidden = false;
  $<HTMLButtonElement>('fw-update').disabled = true;
}
void findRelease();
