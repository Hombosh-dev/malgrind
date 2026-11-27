import { describe, expect, it } from 'vitest';
import {
  clampLeakAgeS,
  errorLabel,
  formatAgeMs,
  leakFrame,
  memcheckProblemCount,
  sortLeaks,
} from './memcheck';
import type { LeakGroup, MemcheckData } from './types';

describe('memcheckProblemCount', () => {
  it('returns 0 for null or clean data (ignoring possible leaks)', () => {
    expect(memcheckProblemCount(null)).toBe(0);
    const clean: MemcheckData = {
      leaks: [{ kind: 'possibly', stack: ['main', 'buf'], count: 5, bytes: 1024, oldest_ms: 100 }],
      errors: [],
      dropped: 0,
      live_bytes: 1024,
      live_allocs: 5,
    };
    expect(memcheckProblemCount(clean)).toBe(0);
  });

  it('sums errors and definite leak groups', () => {
    const data: MemcheckData = {
      leaks: [
        { kind: 'definitely', stack: ['main', 'leak_a'], count: 10, bytes: 640, oldest_ms: 500 },
        { kind: 'possibly', stack: ['main', 'cache'], count: 2, bytes: 128, oldest_ms: 9000 },
        { kind: 'definitely', stack: ['main', 'leak_b'], count: 1, bytes: 64, oldest_ms: 200 },
      ],
      errors: [
        { t_ms: 1200, code: 1, kind: 'heap_corrupt', arg: 0, name: 'worker', stack: ['worker'] },
        { t_ms: 2400, code: 3, kind: 'alloc_failed', arg: 4096, name: 'net', stack: ['net'] },
      ],
      dropped: 4,
      live_bytes: 832,
      live_allocs: 13,
    };
    expect(memcheckProblemCount(data)).toBe(4);
  });
});

describe('errorLabel', () => {
  it('maps known error kinds and falls back to numeric error code', () => {
    expect(errorLabel(1, 'heap_corrupt')).toBe('Heap corruption');
    expect(errorLabel(2, 'stack_overflow')).toBe('Stack overflow');
    expect(errorLabel(3, 'alloc_failed')).toBe('Allocation failed');
    expect(errorLabel(42, 'unknown')).toBe('Memory error #42');
    expect(errorLabel(7, 'unrecognized_kind')).toBe('Memory error #7');
  });
});

describe('leakFrame', () => {
  it('extracts the innermost frame or empty string when stack is empty', () => {
    expect(leakFrame([])).toBe('');
    expect(leakFrame(['app_main'])).toBe('app_main');
    expect(leakFrame(['app_main', 'worker', 'alloc_buf'])).toBe('alloc_buf');
  });
});

describe('clampLeakAgeS', () => {
  it('clamps non-finite, negative, and fractional values to non-negative integers', () => {
    expect(clampLeakAgeS(Number.NaN)).toBe(30);
    expect(clampLeakAgeS(Number.POSITIVE_INFINITY)).toBe(30);
    expect(clampLeakAgeS(-10)).toBe(0);
    expect(clampLeakAgeS(0)).toBe(0);
    expect(clampLeakAgeS(12.6)).toBe(13);
    expect(clampLeakAgeS(45)).toBe(45);
  });
});

describe('formatAgeMs', () => {
  it('formats sub-second, seconds, minutes, and hours consistently', () => {
    expect(formatAgeMs(0)).toBe('0 ms');
    expect(formatAgeMs(-200)).toBe('0 ms');
    expect(formatAgeMs(Number.NaN)).toBe('0 ms');
    expect(formatAgeMs(450)).toBe('450 ms');
    expect(formatAgeMs(999.6)).toBe('1.0 s');
    expect(formatAgeMs(1500)).toBe('1.5 s');
    expect(formatAgeMs(31_000)).toBe('31.0 s');
    expect(formatAgeMs(59_960)).toBe('1 min 00 s');
    expect(formatAgeMs(65_000)).toBe('1 min 05 s');
    expect(formatAgeMs(3_661_000)).toBe('1 h 01 min');
  });
});

describe('sortLeaks', () => {
  const rows: LeakGroup[] = [
    { kind: 'possibly', stack: ['app_main', 'zeta'], count: 4, bytes: 512, oldest_ms: 4000 },
    { kind: 'definitely', stack: ['app_main', 'beta'], count: 2, bytes: 256, oldest_ms: 1000 },
    { kind: 'definitely', stack: ['app_main', 'alpha'], count: 8, bytes: 512, oldest_ms: 2000 },
    { kind: 'possibly', stack: ['app_main', 'alpha'], count: 1, bytes: 128, oldest_ms: 5000 },
    { kind: 'possibly', stack: [], count: 3, bytes: 256, oldest_ms: 3000 },
  ];

  it('does not mutate the input array and uses deterministic tie-breaking', () => {
    const before = rows.map((r) => `${r.kind}:${r.stack.join('/')}`);
    const byKind = sortLeaks(rows, 'kind', 'asc');
    expect(rows.map((r) => `${r.kind}:${r.stack.join('/')}`)).toEqual(before);

    // definitely first (ordered by bytes desc: 512 alpha before 256 beta), then possibly (512 zeta, 256 [], 128 alpha)
    expect(byKind.map((r) => `${r.kind}:${r.stack[r.stack.length - 1] ?? 'empty'}`)).toEqual([
      'definitely:alpha',
      'definitely:beta',
      'possibly:zeta',
      'possibly:empty',
      'possibly:alpha',
    ]);
    expect(sortLeaks(rows, 'kind', 'desc').map((r) => r.kind)).toEqual([
      'possibly',
      'possibly',
      'possibly',
      'definitely',
      'definitely',
    ]);
  });

  it('sorts by bytes, count, and oldest_ms in both directions with tie-breaking', () => {
    // bytes desc: 512 tie -> definitely:alpha before possibly:zeta; 256 tie -> definitely:beta before possibly:empty
    expect(sortLeaks(rows, 'bytes', 'desc').map((r) => `${r.kind}:${r.bytes}:${r.stack[r.stack.length - 1] ?? 'empty'}`)).toEqual([
      'definitely:512:alpha',
      'possibly:512:zeta',
      'definitely:256:beta',
      'possibly:256:empty',
      'possibly:128:alpha',
    ]);
    expect(sortLeaks(rows, 'bytes', 'asc').map((r) => r.bytes)).toEqual([128, 256, 256, 512, 512]);
    expect(sortLeaks(rows, 'count', 'desc').map((r) => r.count)).toEqual([8, 4, 3, 2, 1]);
    expect(sortLeaks(rows, 'oldest_ms', 'asc').map((r) => r.oldest_ms)).toEqual([1000, 2000, 3000, 4000, 5000]);
  });

  it('sorts by innermost stack frame and keeps empty stacks last in both directions', () => {
    // frame asc: alpha (definitely before possibly), beta, zeta, empty
    expect(sortLeaks(rows, 'frame', 'asc').map((r) => `${r.kind}:${r.stack[r.stack.length - 1] ?? 'empty'}`)).toEqual([
      'definitely:alpha',
      'possibly:alpha',
      'definitely:beta',
      'possibly:zeta',
      'possibly:empty',
    ]);
    // frame desc: zeta, beta, alpha (definitely before possibly tie-break), empty still last
    expect(sortLeaks(rows, 'frame', 'desc').map((r) => `${r.kind}:${r.stack[r.stack.length - 1] ?? 'empty'}`)).toEqual([
      'possibly:zeta',
      'definitely:beta',
      'definitely:alpha',
      'possibly:alpha',
      'possibly:empty',
    ]);
  });
});
