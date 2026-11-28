import type { SortDir } from './series';
import type { LeakGroup, MemcheckData } from './types';

export type { SortDir } from './series';
export type LeakSortKey = 'kind' | 'bytes' | 'count' | 'oldest_ms' | 'frame';

const ERROR_LABELS: Record<string, string> = {
  heap_corrupt: 'Heap corruption',
  stack_overflow: 'Stack overflow',
  alloc_failed: 'Allocation failed',
};

/** Count active memory problems: all recorded errors plus definite leak groups. */
export function memcheckProblemCount(data: MemcheckData | null): number {
  if (!data) return 0;
  let count = data.errors.length;
  for (const leak of data.leaks) {
    if (leak.kind === 'definitely') count += 1;
  }
  return count;
}

/** Human-friendly label for a target memory or stack error event. */
export function errorLabel(code: number, kind: string): string {
  return ERROR_LABELS[kind] ?? `Memory error #${code}`;
}

/** Innermost stack frame (allocation site) for a leak group, or `''` when absent. */
export function leakFrame(stack: readonly string[]): string {
  return stack.length > 0 ? stack[stack.length - 1] : '';
}

/** Clamp a leak age threshold in seconds to a finite non-negative integer. */
export function clampLeakAgeS(age: number): number {
  if (!Number.isFinite(age)) return 30;
  return Math.max(0, Math.round(age));
}

/** Format an age or timestamp offset in milliseconds (`450 ms`, `31.0 s`, `1 min 05 s`, `1 h 05 min`). */
export function formatAgeMs(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '0 ms';
  const roundedMs = Math.round(ms);
  if (roundedMs < 1000) return `${roundedMs} ms`;
  const tenths = Math.round(ms / 100);
  if (tenths < 600) return `${(tenths / 10).toFixed(1)} s`;
  const totalSeconds = Math.floor(tenths / 10);
  const totalMinutes = Math.floor(totalSeconds / 60);
  if (totalMinutes < 60) {
    return `${totalMinutes} min ${String(totalSeconds % 60).padStart(2, '0')} s`;
  }
  const hours = Math.floor(totalMinutes / 60);
  return `${hours} h ${String(totalMinutes % 60).padStart(2, '0')} min`;
}

function compareStrings(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

function compareStacks(a: readonly string[], b: readonly string[]): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i += 1) {
    const cmp = compareStrings(a[i], b[i]);
    if (cmp !== 0) return cmp;
  }
  return a.length - b.length;
}

function kindRank(kind: LeakGroup['kind']): number {
  return kind === 'definitely' ? 0 : 1;
}

function tieBreakLeak(a: LeakGroup, b: LeakGroup): number {
  const byKind = kindRank(a.kind) - kindRank(b.kind);
  if (byKind !== 0) return byKind;
  if (a.bytes !== b.bytes) return b.bytes - a.bytes;
  if (a.count !== b.count) return b.count - a.count;
  if (a.oldest_ms !== b.oldest_ms) return a.oldest_ms - b.oldest_ms;
  return compareStacks(a.stack, b.stack);
}

/** Return a sorted copy of `leaks` with deterministic severity, size, count, age, and stack tie-breaking. */
export function sortLeaks(
  leaks: readonly LeakGroup[],
  key: LeakSortKey = 'kind',
  dir: SortDir = 'asc',
): LeakGroup[] {
  const sign = dir === 'asc' ? 1 : -1;
  return [...leaks].sort((a, b) => {
    if (key === 'kind') {
      const cmp = kindRank(a.kind) - kindRank(b.kind);
      return cmp !== 0 ? sign * cmp : tieBreakLeak(a, b);
    }
    if (key === 'frame') {
      const fa = leakFrame(a.stack);
      const fb = leakFrame(b.stack);
      if (fa === '' && fb === '') return tieBreakLeak(a, b);
      if (fa === '') return 1;
      if (fb === '') return -1;
      const cmp = compareStrings(fa, fb);
      return cmp !== 0 ? sign * cmp : tieBreakLeak(a, b);
    }
    const va = a[key];
    const vb = b[key];
    if (va !== vb) return sign * (va - vb);
    return tieBreakLeak(a, b);
  });
}
