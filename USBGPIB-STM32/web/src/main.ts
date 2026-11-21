import './style.css';
import { PrologixClient } from './prologix';
import { APP_FILTER, BOOT_FILTER, checkImage, detach, flash } from './dfu';

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

const LATEST = './firmware/usbgpib.bin';

function fwStatus(text: string): void {
  $('fw-status').textContent = text;
}

async function findLatest(): Promise<void> {
  try {
    const r = await fetch('./firmware/info.json', { cache: 'no-store' });
    if (!r.ok) return;
    const info = (await r.json()) as { commit?: string; date?: string };
    $<HTMLInputElement>('fw-latest').disabled = false;
    $<HTMLInputElement>('fw-latest').checked = true;
    $('fw-info').textContent = `(${(info.commit ?? '').slice(0, 7)}, ${info.date ?? ''})`;
  } catch {
    /* only on the published site */
  }
}

async function firmwareImage(): Promise<Uint8Array> {
  if ($<HTMLInputElement>('fw-latest').checked) {
    const r = await fetch(LATEST, { cache: 'no-store' });
    if (!r.ok) throw new Error('Could not download the latest build.');
    return new Uint8Array(await r.arrayBuffer());
  }
  const f = $<HTMLInputElement>('fw-file').files?.[0];
  if (!f) throw new Error('Choose a .bin file first.');
  return new Uint8Array(await f.arrayBuffer());
}

async function fwDetach(): Promise<void> {
  if (client) { // over the serial port: works on every OS without a WebUSB driver
    await client.command('dfu');
    await disconnect();
  } else {
    if (!('usb' in navigator)) throw new Error('WebUSB is not available in this browser.');
    await detach(await navigator.usb.requestDevice({ filters: [APP_FILTER] }));
  }
  fwStatus('The adapter is restarting into the updater. Continue with step 2.');
}

async function fwFlash(): Promise<void> {
  if (!('usb' in navigator)) throw new Error('WebUSB is not available in this browser.');
  const img = await firmwareImage();
  const problem = checkImage(img);
  if (problem) throw new Error(problem);
  const dev = await navigator.usb.requestDevice({ filters: [BOOT_FILTER] });
  const bar = $<HTMLProgressElement>('fw-progress');
  fwStatus('Writing…');
  await flash(dev, img, (done, total) => {
    bar.value = done / total;
  });
  fwStatus(`Done: ${img.length} bytes written. The adapter restarts with the new firmware.`);
}

function fwGuard(op: () => Promise<void>): () => void {
  return () => {
    op().catch((e: unknown) => {
      if (e instanceof DOMException && e.name === 'NotFoundError') return; // chooser cancelled
      fwStatus(e instanceof Error ? e.message : String(e));
    });
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
$('fw-detach').addEventListener('click', fwGuard(fwDetach));
$('fw-flash').addEventListener('click', fwGuard(fwFlash));
$('fw-file').addEventListener('change', () => ($<HTMLInputElement>('fw-file-radio').checked = true));
void findLatest();
