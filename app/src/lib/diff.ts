import type { DiffFunction, DiffNode } from './types';

export type DiffSortKey =
  | 'abs_self'
  | 'a_self'
  | 'b_self'
  | 'delta_self'
  | 'a_incl'
  | 'b_incl'
  | 'delta_incl'
  | 'name';

export type SortDir = 'asc' | 'desc';

const NEUTRAL_COLOR = '#e6e1d8';
const DELTA_NEUTRAL_THRESHOLD = 0.05;
const DELTA_CLAMP_PCT = 20;

// Soft tint at threshold -> saturated readable shade at clamp (keeps dark frame labels legible).
const POS_LOW: [number, number, number] = [247, 220, 215]; // #f7dcd7
const POS_HIGH: [number, number, number] = [239, 99, 81]; // #ef6351
const NEG_LOW: [number, number, number] = [215, 231, 247]; // #d7e7f7
const NEG_HIGH: [number, number, number] = [92, 162, 235]; // #5ca2eb

/** Normalized percentage share of `count` out of `total`, or 0 when `total <= 0`. */
export function nodeSharePct(count: number, total: number): number {
  if (!Number.isFinite(count) || !Number.isFinite(total) || total <= 0) return 0;
  return (100 * count) / total;
}

/** Normalized percentage-point change (`bPct - aPct`) for a differential flamegraph node. */
export function nodeDeltaPct(
  node: Pick<DiffNode, 'a' | 'b'>,
  totalA: number,
  totalB: number,
): number {
  return nodeSharePct(node.b, totalB) - nodeSharePct(node.a, totalA);
}

function lerpHex(from: [number, number, number], to: [number, number, number], t: number): string {
  const clamped = Math.max(0, Math.min(1, t));
  const hex = (i: number) =>
    Math.round(from[i] + (to[i] - from[i]) * clamped)
      .toString(16)
      .padStart(2, '0');
  return `#${hex(0)}${hex(1)}${hex(2)}`;
}

/** Map a percentage-point delta to neutral gray, red (grew in B), or blue (shrank in B). */
export function diffColor(deltaPct: number): string {
  if (!Number.isFinite(deltaPct) || Math.abs(deltaPct) < DELTA_NEUTRAL_THRESHOLD) {
    return NEUTRAL_COLOR;
  }
  const t = Math.min(1, Math.abs(deltaPct) / DELTA_CLAMP_PCT);
  return deltaPct > 0 ? lerpHex(POS_LOW, POS_HIGH, t) : lerpHex(NEG_LOW, NEG_HIGH, t);
}

/** Format a percentage-point delta as `+12.3%`, `-4.5%`, or `0.0%`. */
export function formatDelta(deltaPct: number): string {
  if (!Number.isFinite(deltaPct)) return '0.0%';
  const tenths = Math.round(Math.abs(deltaPct) * 10);
  if (tenths === 0) return '0.0%';
  const abs = (tenths / 10).toFixed(1);
  return deltaPct > 0 ? `+${abs}%` : `-${abs}%`;
}

function compareStrings(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

function metricValue(row: DiffFunction, key: Exclude<DiffSortKey, 'name'>): number {
  switch (key) {
    case 'abs_self':
      return Math.abs(row.b_self_pct - row.a_self_pct);
    case 'a_self':
      return row.a_self_pct;
    case 'b_self':
      return row.b_self_pct;
    case 'delta_self':
      return row.b_self_pct - row.a_self_pct;
    case 'a_incl':
      return row.a_incl_pct;
    case 'b_incl':
      return row.b_incl_pct;
    case 'delta_incl':
      return row.b_incl_pct - row.a_incl_pct;
  }
}

/** Return a sorted copy of `rows`, tie-breaking equal values deterministically by `name`. */
export function sortDiffFunctions(
  rows: readonly DiffFunction[],
  key: DiffSortKey = 'abs_self',
  dir: SortDir = 'desc',
): DiffFunction[] {
  const sign = dir === 'asc' ? 1 : -1;
  return [...rows].sort((x, y) => {
    if (key === 'name') return sign * compareStrings(x.name, y.name);
    const vx = metricValue(x, key);
    const vy = metricValue(y, key);
    if (vx !== vy) return sign * (vx - vy);
    return compareStrings(x.name, y.name);
  });
}

/** Case-insensitive substring filter on function names. */
export function filterDiffFunctions(rows: readonly DiffFunction[], query: string): DiffFunction[] {
  const q = query.trim().toLowerCase();
  if (!q) return [...rows];
  return rows.filter((r) => r.name.toLowerCase().includes(q));
}

/** Toggle a run id in a 2-slot `[A, B]` comparison selection. */
export function toggleRunPick(selected: readonly string[], id: string): string[] {
  if (selected.includes(id)) return selected.filter((x) => x !== id);
  if (selected.length >= 2) return [...selected];
  return [...selected, id];
}

/** Return `'A'`, `'B'`, or `null` for a run id in a comparison selection. */
export function pickSlot(selected: readonly string[], id: string): 'A' | 'B' | null {
  const idx = selected.indexOf(id);
  if (idx === 0) return 'A';
  if (idx === 1) return 'B';
  return null;
}
