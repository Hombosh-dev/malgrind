import { describe, expect, it } from 'vitest';
import {
  diffColor,
  filterDiffFunctions,
  formatDelta,
  nodeDeltaPct,
  nodeSharePct,
  pickSlot,
  sortDiffFunctions,
  toggleRunPick,
} from './diff';
import type { DiffFunction } from './types';

function parseHexRgb(hex: string): [number, number, number] {
  expect(hex).toMatch(/^#[0-9a-f]{6}$/);
  return [
    parseInt(hex.slice(1, 3), 16),
    parseInt(hex.slice(3, 5), 16),
    parseInt(hex.slice(5, 7), 16),
  ];
}

describe('nodeDeltaPct', () => {
  it('computes normalized percentage-point share difference bPct - aPct', () => {
    // 50/100 (50%) vs 150/200 (75%) -> +25 percentage points
    expect(nodeDeltaPct({ a: 50, b: 150 }, 100, 200)).toBeCloseTo(25);
    // Same share even when raw sample counts double -> 0 percentage points
    expect(nodeDeltaPct({ a: 50, b: 100 }, 100, 200)).toBeCloseTo(0);
    // Shrinkage from 40% to 15% -> -25 percentage points
    expect(nodeDeltaPct({ a: 40, b: 30 }, 100, 200)).toBeCloseTo(-25);
  });

  it('handles zero totals safely without NaN', () => {
    expect(nodeSharePct(10, 0)).toBe(0);
    expect(nodeDeltaPct({ a: 0, b: 0 }, 0, 0)).toBe(0);
    expect(nodeDeltaPct({ a: 0, b: 50 }, 0, 100)).toBeCloseTo(50);
    expect(nodeDeltaPct({ a: 50, b: 0 }, 100, 0)).toBeCloseTo(-50);
  });
});

describe('diffColor', () => {
  it('returns neutral warm gray inside [-0.05, +0.05) and for non-finite values', () => {
    expect(diffColor(0)).toBe('#e6e1d8');
    expect(diffColor(-0)).toBe('#e6e1d8');
    expect(diffColor(0.049)).toBe('#e6e1d8');
    expect(diffColor(-0.049)).toBe('#e6e1d8');
    expect(diffColor(Number.NaN)).toBe('#e6e1d8');
    expect(diffColor(Number.POSITIVE_INFINITY)).toBe('#e6e1d8');
  });

  it('returns monotonically intensifying red shades for positive growth and clamps cleanly', () => {
    const cLow = parseHexRgb(diffColor(0.1));
    const cMid = parseHexRgb(diffColor(10));
    const cClamp = parseHexRgb(diffColor(20));
    const cOver = parseHexRgb(diffColor(80));

    for (const [r, g, b] of [cLow, cMid, cClamp]) {
      expect(r).toBeGreaterThan(g);
      expect(r).toBeGreaterThan(b);
    }
    // Red dominance (r - g) increases from low -> mid -> clamp
    expect(cMid[0] - cMid[1]).toBeGreaterThan(cLow[0] - cLow[1]);
    expect(cClamp[0] - cClamp[1]).toBeGreaterThan(cMid[0] - cMid[1]);
    expect(cOver).toEqual(cClamp);
  });

  it('returns monotonically intensifying blue shades for reductions and clamps cleanly', () => {
    const cLow = parseHexRgb(diffColor(-0.1));
    const cMid = parseHexRgb(diffColor(-10));
    const cClamp = parseHexRgb(diffColor(-20));
    const cOver = parseHexRgb(diffColor(-75));

    for (const [r, g, b] of [cLow, cMid, cClamp]) {
      expect(b).toBeGreaterThan(r);
      expect(b).toBeGreaterThan(g);
    }
    // Blue dominance (b - r) increases from low -> mid -> clamp
    expect(cMid[2] - cMid[0]).toBeGreaterThan(cLow[2] - cLow[0]);
    expect(cClamp[2] - cClamp[0]).toBeGreaterThan(cMid[2] - cMid[0]);
    expect(cOver).toEqual(cClamp);
  });
});

describe('formatDelta', () => {
  it('formats positive, negative (unicode minus), and zero deltas cleanly', () => {
    expect(formatDelta(12.34)).toBe('+12.3%');
    expect(formatDelta(-4.5)).toBe('−4.5%');
    expect(formatDelta(0)).toBe('0.0%');
    expect(formatDelta(-0)).toBe('0.0%');
    expect(formatDelta(0.04)).toBe('0.0%');
    expect(formatDelta(-0.04)).toBe('0.0%');
    expect(formatDelta(0.05)).toBe('+0.1%');
    expect(formatDelta(-0.05)).toBe('−0.1%');
    expect(formatDelta(Number.NaN)).toBe('0.0%');
  });
});

describe('sortDiffFunctions', () => {
  const rows: DiffFunction[] = [
    { name: 'zeta', a_self_pct: 10, b_self_pct: 15, a_incl_pct: 20, b_incl_pct: 30 }, // Δself +5, Δincl +10
    { name: 'alpha', a_self_pct: 20, b_self_pct: 10, a_incl_pct: 50, b_incl_pct: 25 }, // Δself -10, Δincl -25
    { name: 'beta', a_self_pct: 5, b_self_pct: 15, a_incl_pct: 15, b_incl_pct: 20 }, // Δself +10, Δincl +5
    { name: 'gamma', a_self_pct: 30, b_self_pct: 30, a_incl_pct: 40, b_incl_pct: 40 }, // Δself 0, Δincl 0
  ];

  it('defaults to abs_self descending with deterministic name tie-breaking and does not mutate input', () => {
    const snapshot = rows.map((r) => r.name);
    const sorted = sortDiffFunctions(rows);
    expect(rows.map((r) => r.name)).toEqual(snapshot);
    // alpha and beta both have |Δself| = 10 -> alpha before beta alphabetically
    expect(sorted.map((r) => r.name)).toEqual(['alpha', 'beta', 'zeta', 'gamma']);
  });

  it('sorts by signed self and inclusive deltas asc/desc', () => {
    expect(sortDiffFunctions(rows, 'delta_self', 'desc').map((r) => r.name)).toEqual([
      'beta',
      'zeta',
      'gamma',
      'alpha',
    ]);
    expect(sortDiffFunctions(rows, 'delta_self', 'asc').map((r) => r.name)).toEqual([
      'alpha',
      'gamma',
      'zeta',
      'beta',
    ]);
    expect(sortDiffFunctions(rows, 'delta_incl', 'desc').map((r) => r.name)).toEqual([
      'zeta',
      'beta',
      'gamma',
      'alpha',
    ]);
  });

  it('sorts by a_self, b_self, a_incl, b_incl, and name', () => {
    expect(sortDiffFunctions(rows, 'a_self', 'desc').map((r) => r.name)).toEqual([
      'gamma',
      'alpha',
      'zeta',
      'beta',
    ]);
    expect(sortDiffFunctions(rows, 'b_self', 'desc').map((r) => r.name)).toEqual([
      'gamma',
      'beta',
      'zeta',
      'alpha',
    ]);
    expect(sortDiffFunctions(rows, 'a_incl', 'asc').map((r) => r.name)).toEqual([
      'beta',
      'zeta',
      'gamma',
      'alpha',
    ]);
    expect(sortDiffFunctions(rows, 'b_incl', 'desc').map((r) => r.name)).toEqual([
      'gamma',
      'zeta',
      'alpha',
      'beta',
    ]);
    expect(sortDiffFunctions(rows, 'name', 'asc').map((r) => r.name)).toEqual([
      'alpha',
      'beta',
      'gamma',
      'zeta',
    ]);
    expect(sortDiffFunctions(rows, 'name', 'desc').map((r) => r.name)).toEqual([
      'zeta',
      'gamma',
      'beta',
      'alpha',
    ]);
  });
});

describe('filterDiffFunctions and run pick helpers', () => {
  it('filters case-insensitively by trimmed query', () => {
    const rows: DiffFunction[] = [
      { name: 'app_main', a_self_pct: 1, b_self_pct: 2, a_incl_pct: 10, b_incl_pct: 20 },
      { name: 'sensor_poll', a_self_pct: 5, b_self_pct: 3, a_incl_pct: 5, b_incl_pct: 3 },
    ];
    expect(filterDiffFunctions(rows, '   ').map((r) => r.name)).toEqual(['app_main', 'sensor_poll']);
    expect(filterDiffFunctions(rows, ' MAIN ').map((r) => r.name)).toEqual(['app_main']);
    expect(filterDiffFunctions(rows, 'missing')).toEqual([]);
  });

  it('toggles up to 2 run picks and resolves A/B slots', () => {
    let picked = toggleRunPick([], 'r1');
    expect(picked).toEqual(['r1']);
    expect(pickSlot(picked, 'r1')).toBe('A');
    expect(pickSlot(picked, 'r2')).toBeNull();

    picked = toggleRunPick(picked, 'r2');
    expect(picked).toEqual(['r1', 'r2']);
    expect(pickSlot(picked, 'r1')).toBe('A');
    expect(pickSlot(picked, 'r2')).toBe('B');

    // Third run is ignored while 2 are selected
    picked = toggleRunPick(picked, 'r3');
    expect(picked).toEqual(['r1', 'r2']);

    // Unchecking A promotes B to A
    picked = toggleRunPick(picked, 'r1');
    expect(picked).toEqual(['r2']);
    expect(pickSlot(picked, 'r2')).toBe('A');
  });
});
