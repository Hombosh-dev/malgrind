import { describe, expect, it } from 'vitest';
import { Sidecar, type Transport } from './sidecar';

// A transport the test drives by hand: `sent` collects what the client wrote, `push` plays what the sidecar printed.
function fake() {
  const sent: any[] = [];
  let data: (chunk: string) => void = () => {};
  let exit: (code: number | null) => void = () => {};
  const transport: Transport = {
    write: async (line) => void sent.push(JSON.parse(line)),
    onData: (cb) => (data = cb),
    onExit: (cb) => (exit = cb),
  };
  return { transport, sent, push: (s: string) => data(s), exit: (c: number | null = 0) => exit(c) };
}
const line = (o: unknown) => JSON.stringify(o) + '\n';

describe('Sidecar', () => {
  it('sends the command with an id and the arguments', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const p = sc.call('probe', { port: '/dev/ttyUSB0' });
    expect(f.sent).toEqual([{ id: 1, cmd: 'probe', port: '/dev/ttyUSB0' }]);
    f.push(line({ id: 1, ok: true, result: { agent: true } }));
    expect(await p).toEqual({ agent: true });
  });

  it('matches replies by id, in any order', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const a = sc.call('a'), b = sc.call('b');
    f.push(line({ id: 2, ok: true, result: 'B' }) + line({ id: 1, ok: true, result: 'A' }));
    expect([await a, await b]).toEqual(['A', 'B']);
  });

  it('rejects with the error text when ok is false', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const p = sc.call('open', { run: 'x' });
    f.push(line({ id: 1, ok: false, error: 'no such run: x' }));
    await expect(p).rejects.toThrow('no such run: x');
  });

  it('handles delivery in lines, in raw chunks, and without a trailing newline', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const reply = (id: number, result: unknown) => JSON.stringify({ id, ok: true, result });
    const a = sc.call('a'), b = sc.call('b'), c = sc.call('c');
    f.push(reply(1, 'A')); // one line, no newline: the shell plugin may hand lines over without it
    const text = reply(2, 'B') + '\n' + reply(3, 'C') + '\n';
    for (let i = 0; i < text.length; i += 7) f.push(text.slice(i, i + 7)); // 7-byte pieces
    expect([await a, await b, await c]).toEqual(['A', 'B', 'C']);
  });

  it('copes with a reply of hundreds of kilobytes', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const p = sc.call('snapshot');
    const big = line({ id: 1, ok: true, result: { names: 'x'.repeat(600_000) } });
    for (let i = 0; i < big.length; i += 65_536) f.push(big.slice(i, i + 65_536));
    expect(((await p) as any).names.length).toBe(600_000);
  });

  it('delivers events to subscribers only, and unsubscribe works', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const seen: any[] = [];
    const off = sc.on('build', (e) => seen.push(e.line));
    const p = sc.call('new_run');
    f.push(line({ event: 'build', line: 'compiling' }) + line({ event: 'other', x: 1 }));
    off();
    f.push(line({ event: 'build', line: 'linking' }) + line({ id: 1, ok: true, result: {} }));
    await p;
    expect(seen).toEqual(['compiling']);
  });

  it('rejects pending calls, and later ones, when the sidecar exits', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const p = sc.call('a');
    f.exit(1);
    await expect(p).rejects.toThrow('sidecar exited');
    await expect(sc.call('b')).rejects.toThrow('sidecar exited');
  });

  it('ignores lines that are not JSON (a stray log line must not break the stream)', async () => {
    const f = fake();
    const sc = new Sidecar(f.transport);
    const p = sc.call('a');
    f.push('some stray text\n' + line({ id: 1, ok: true, result: 1 }));
    expect(await p).toBe(1);
  });
});
