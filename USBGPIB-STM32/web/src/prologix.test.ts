import { describe, expect, it } from 'vitest';
import { escapeData, PrologixClient } from './prologix';

const enc = new TextEncoder();
const dec = new TextDecoder();

/** A fake adapter: records what was written and answers through feed() */
function fake(answer: (line: string, c: PrologixClient) => void = () => {}) {
  const written: string[] = [];
  const c: PrologixClient = new PrologixClient(async (bytes) => {
    const line = dec.decode(bytes);
    written.push(line);
    queueMicrotask(() => answer(line, c));
  });
  return { c, written };
}

describe('escapeData', () => {
  it('escapes CR, LF, ESC and +', () => {
    expect(Array.from(escapeData(Uint8Array.from([0, 13, 1, 10, 27, 43, 2])))).toEqual([0, 27, 13, 1, 27, 10, 27, 27, 27, 43, 2]);
  });
});

describe('PrologixClient', () => {
  it('sends ++ commands as lines', async () => {
    const { c, written } = fake();
    await c.command('addr 5');
    expect(written).toEqual(['++addr 5\n']);
  });

  it('returns a query reply without CR LF', async () => {
    const { c } = fake((line, c) => line === '++addr\n' && c.feed(enc.encode('5\r\n')));
    expect(await c.query('addr')).toBe('5');
  });

  it('returns null when no reply comes', async () => {
    const { c } = fake();
    expect(await c.query('spoll', 50)).toBeNull();
  });

  it('keeps queries in order when replies arrive in pieces', async () => {
    const { c } = fake((line, c) => {
      if (line === '++ver\n') {
        c.feed(enc.encode('USBGPIB'));
        setTimeout(() => c.feed(enc.encode('-STM32\r\n')), 10);
      }
      if (line === '++srq\n') c.feed(enc.encode('0\r\n'));
    });
    const [v, s] = await Promise.all([c.query('ver'), c.query('srq')]);
    expect(v).toBe('USBGPIB-STM32');
    expect(s).toBe('0');
  });

  it('escapes instrument data and asks for the answer up to LF', async () => {
    const { c, written } = fake((line, c) => line === '++read 10\n' && c.feed(enc.encode('ACME,M1\n')));
    expect(await c.ask('VOLT +1.0')).toBe('ACME,M1');
    expect(written).toEqual(['VOLT \x1b+1.0\n', '++read 10\n']);
  });

  it('collects a read until the stream goes quiet', async () => {
    const { c } = fake((line, c) => {
      if (line !== '++read eoi\n') return;
      c.feed(enc.encode('12'));
      setTimeout(() => c.feed(enc.encode('34')), 20);
    });
    expect(dec.decode(await c.read('eoi', 50))).toBe('1234');
  });

  it('shows every received byte to onData', async () => {
    const seen: string[] = [];
    const c = new PrologixClient(async () => {}, (b) => seen.push(dec.decode(b)));
    c.feed(enc.encode('x'));
    expect(seen).toEqual(['x']);
  });
});
