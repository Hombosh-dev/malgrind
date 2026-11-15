// The client of `malgrind-cli serve`: one JSON object per line on stdin/stdout (protocol: host/src/serve.hpp).
import { resolveResource } from '@tauri-apps/api/path';
import { Command } from '@tauri-apps/plugin-shell';

export interface Transport {
  write(line: string): Promise<void>;
  // Whatever the sidecar printed: whole lines, or arbitrary pieces of them; the client does the framing.
  onData(cb: (chunk: string) => void): void;
  onExit(cb: (code: number | null) => void): void;
}

interface Pending {
  resolve(value: unknown): void;
  reject(error: Error): void;
}

export class Sidecar {
  #nextId = 1;
  #pending = new Map<number, Pending>();
  #listeners = new Map<string, Set<(event: any) => void>>();
  #buffer = '';
  #exited = false;

  constructor(private readonly transport: Transport) {
    transport.onData((chunk) => this.#feed(chunk));
    transport.onExit(() => this.#exit());
  }

  call<T = unknown>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
    if (this.#exited) return Promise.reject(new Error('sidecar exited'));
    const id = this.#nextId++;
    return new Promise<T>((resolve, reject) => {
      this.#pending.set(id, { resolve: resolve as (v: unknown) => void, reject });
      this.transport.write(JSON.stringify({ ...args, id, cmd })).catch((e) => {
        this.#pending.delete(id);
        reject(e instanceof Error ? e : new Error(String(e)));
      });
    });
  }

  // Subscribes to events such as `build`, `target`, `notice`, `ended`; returns the unsubscribe function.
  on(event: string, cb: (event: any) => void): () => void {
    const set = this.#listeners.get(event) ?? new Set();
    set.add(cb);
    this.#listeners.set(event, set);
    return () => set.delete(cb);
  }

  #feed(chunk: string) {
    this.#buffer += chunk;
    for (let nl; (nl = this.#buffer.indexOf('\n')) >= 0; ) {
      const line = this.#buffer.slice(0, nl);
      this.#buffer = this.#buffer.slice(nl + 1);
      this.#dispatch(line);
    }
    // The plugin may hand over a whole line without its newline; a complete JSON object is never a prefix of another.
    if (this.#buffer.trim() && this.#dispatch(this.#buffer)) this.#buffer = '';
  }

  #dispatch(line: string): boolean {
    let msg: any;
    try {
      msg = JSON.parse(line);
    } catch {
      return false; // not JSON: a stray log line, or an incomplete one
    }
    if (typeof msg !== 'object' || msg === null) return false;
    if (typeof msg.event === 'string') {
      this.#listeners.get(msg.event)?.forEach((cb) => cb(msg));
    } else if (typeof msg.id === 'number') {
      const pending = this.#pending.get(msg.id);
      this.#pending.delete(msg.id);
      if (msg.ok) pending?.resolve(msg.result);
      else pending?.reject(new Error(msg.error ?? 'unknown error'));
    }
    return true;
  }

  #exit() {
    this.#exited = true;
    for (const p of this.#pending.values()) p.reject(new Error('sidecar exited'));
    this.#pending.clear();
  }
}

// Spawns the bundled sidecar. The agent sources travel with the app as a resource.
export async function startSidecar(): Promise<Sidecar> {
  const agentDir = await resolveResource('agent-src');
  const command = Command.sidecar('binaries/malgrind-cli', ['serve', '--agent-dir', agentDir]);
  let data: (chunk: string) => void = () => {};
  let exit: (code: number | null) => void = () => {};
  command.stdout.on('data', (chunk) => data(chunk)); // no newline added: delivery may be lines or raw pieces
  command.stderr.on('data', (line) => console.warn('[malgrind-cli]', line));
  command.on('close', ({ code }) => exit(code));
  command.on('error', (error) => {
    console.error('[malgrind-cli]', error);
    exit(null);
  });
  const child = await command.spawn();
  return new Sidecar({
    write: (line) => child.write(line + '\n'),
    onData: (cb) => (data = cb),
    onExit: (cb) => (exit = cb),
  });
}
