// Client for the adapter's Prologix-style serial port: ++ commands, escaped instrument data, line replies.

const CR = 13, LF = 10, ESC = 27, PLUS = 43;

const enc = new TextEncoder();
const dec = new TextDecoder();

/** Instrument data with CR, LF, ESC and '+' escaped, so the adapter passes them on instead of acting on them. */
export function escapeData(data: Uint8Array): Uint8Array {
  const out: number[] = [];
  for (const b of data) {
    if (b === CR || b === LF || b === ESC || b === PLUS) out.push(ESC);
    out.push(b);
  }
  return Uint8Array.from(out);
}

export type ReadMode = 'eoi' | 'timeout' | number; // ++read eoi, ++read, ++read <char>

export class PrologixClient {
  private buf: number[] = [];
  private waiter: (() => void) | null = null;
  private chain: Promise<unknown> = Promise.resolve();

  /** write sends bytes to the port; onData sees every byte that arrives (e.g. for a terminal view) */
  constructor(
    private readonly write: (bytes: Uint8Array) => Promise<void>,
    private readonly onData: (bytes: Uint8Array) => void = () => {},
  ) {}

  /** Feed bytes read from the port. */
  feed(bytes: Uint8Array): void {
    this.onData(bytes);
    for (const b of bytes) this.buf.push(b);
    this.waiter?.();
  }

  /** Runs operations one at a time, so replies can't interleave. */
  private serial<T>(op: () => Promise<T>): Promise<T> {
    const next = this.chain.then(op, op);
    this.chain = next.catch(() => {});
    return next;
  }

  private async sendLine(bytes: Uint8Array): Promise<void> {
    const line = new Uint8Array(bytes.length + 1);
    line.set(bytes);
    line[bytes.length] = LF;
    await this.write(line);
  }

  /** Next line (without CR/LF), or null if none arrives in time. */
  private nextLine(timeoutMs: number): Promise<string | null> {
    return new Promise((resolve) => {
      const done = (v: string | null) => {
        clearTimeout(timer);
        this.waiter = null;
        resolve(v);
      };
      const check = () => {
        const i = this.buf.indexOf(LF);
        if (i < 0) return;
        const line = this.buf.splice(0, i + 1);
        done(dec.decode(Uint8Array.from(line)).replace(/\r?\n$/, ''));
      };
      const timer = setTimeout(() => done(null), timeoutMs);
      this.waiter = check;
      check();
    });
  }

  /** Bytes until the stream has been quiet for quietMs (for replies with no line structure). */
  private drain(quietMs: number, maxMs: number): Promise<Uint8Array> {
    return new Promise((resolve) => {
      const start = Date.now();
      let last = this.buf.length;
      const tick = setInterval(() => {
        const grew = this.buf.length !== last;
        last = this.buf.length;
        if ((!grew && Date.now() - start >= quietMs) || Date.now() - start >= maxMs) {
          clearInterval(tick);
          resolve(Uint8Array.from(this.buf.splice(0)));
        }
      }, quietMs);
    });
  }

  /** A ++ command with no reply. */
  command(cmd: string): Promise<void> {
    return this.serial(() => this.sendLine(enc.encode('++' + cmd)));
  }

  /** A ++ command with a one-line reply (null on timeout). */
  query(cmd: string, timeoutMs = 1000): Promise<string | null> {
    return this.serial(async () => {
      this.buf = [];
      await this.sendLine(enc.encode('++' + cmd));
      return this.nextLine(timeoutMs);
    });
  }

  /** Sends a message to the instrument at ++addr (the adapter adds ++eos and EOI). */
  send(data: string | Uint8Array): Promise<void> {
    const bytes = typeof data === 'string' ? enc.encode(data) : data;
    return this.serial(() => this.sendLine(escapeData(bytes)));
  }

  /** Sends a query to the instrument and reads its answer up to LF (or until readTimeoutMs of quiet). */
  ask(data: string, readTimeoutMs = 1000): Promise<string | null> {
    return this.serial(async () => {
      this.buf = [];
      await this.sendLine(escapeData(enc.encode(data)));
      await this.sendLine(enc.encode('++read 10'));
      return this.nextLine(readTimeoutMs + 200);
    });
  }

  /** Raw bytes to the port, e.g. a line end that stops ++lon. */
  writeRaw(bytes: Uint8Array): Promise<void> {
    return this.serial(async () => {
      this.buf = [];
      await this.write(bytes);
    });
  }

  /** ++read: returns everything the instrument sends; done once the stream is quiet past the read timeout. */
  read(mode: ReadMode, readTimeoutMs: number): Promise<Uint8Array> {
    const arg = mode === 'timeout' ? '' : ' ' + String(mode);
    return this.serial(async () => {
      this.buf = [];
      await this.sendLine(enc.encode('++read' + arg));
      return this.drain(readTimeoutMs + 100, 60_000);
    });
  }
}
