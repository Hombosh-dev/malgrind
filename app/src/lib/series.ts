import type { SeriesLine, TaskRow } from './types';

export type TaskSortKey = 'name' | 'cpu_pct' | 'prio' | 'core' | 'state' | 'stack_hwm';
export type SortDir = 'asc' | 'desc';

const REGION_TITLES: Record<string, string> = {
  internal: 'Internal RAM',
  external: 'External PSRAM',
  iram: 'IRAM (exec)',
  total: 'Total heap',
};

const TASK_STATE_LABELS: Record<number, string> = {
  0: 'Running',
  1: 'Ready',
  2: 'Blocked',
  3: 'Suspended',
  4: 'Deleted',
};

/** Whether the agent capability bitmask includes `cap`. */
export function hasCap(caps: number | undefined, cap: number): boolean {
  if (caps === undefined || !Number.isFinite(caps) || !Number.isFinite(cap) || cap <= 0) return false;
  return ((caps >>> 0) & (cap >>> 0)) !== 0;
}

/** Human-friendly display title for a heap region. */
export function regionTitle(name: string, region: number): string {
  const key = name.trim().toLowerCase();
  return REGION_TITLES[key] ?? `Region ${region}`;
}

/** Human-friendly label for a numeric task state code. */
export function taskStateLabel(state: number): string {
  return TASK_STATE_LABELS[state] ?? `State ${state}`;
}

/** Compute a `[0, max]` Y-axis domain with non-zero headroom, incorporating optional `ceilHint > 0`. */
export function computeYRange(values: readonly number[], ceilHint?: number): [number, number] {
  let max = 0;
  for (const v of values) {
    if (Number.isFinite(v) && v > max) max = v;
  }
  if (ceilHint !== undefined && Number.isFinite(ceilHint) && ceilHint > max) {
    max = ceilHint;
  }
  if (max <= 0) return [0, 1];
  return [0, max * 1.1];
}

/** Format a millisecond timestamp or duration as `m:ss`. */
export function formatClockMs(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '0:00';
  const totalSeconds = Math.floor(ms / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}:${String(seconds).padStart(2, '0')}`;
}

function compareStrings(a: string, b: string): number {
  return a < b ? -1 : a > b ? 1 : 0;
}

function tieBreakTask(a: TaskRow, b: TaskRow): number {
  const byName = compareStrings(a.name, b.name);
  return byName !== 0 ? byName : a.handle - b.handle;
}

/** Return a sorted copy of `rows` with deterministic `name` (then `handle`) tie-breaking. */
export function sortTasks(
  rows: readonly TaskRow[],
  key: TaskSortKey = 'cpu_pct',
  dir: SortDir = 'desc',
): TaskRow[] {
  const sign = dir === 'asc' ? 1 : -1;
  return [...rows].sort((a, b) => {
    if (key === 'name') {
      const cmp = compareStrings(a.name, b.name);
      return cmp !== 0 ? sign * cmp : a.handle - b.handle;
    }
    if (key === 'core') {
      if (a.core === null && b.core === null) return tieBreakTask(a, b);
      if (a.core === null) return 1;
      if (b.core === null) return -1;
      if (a.core !== b.core) return sign * (a.core - b.core);
      return tieBreakTask(a, b);
    }
    const va = a[key];
    const vb = b[key];
    if (va !== vb) return sign * (va - vb);
    return tieBreakTask(a, b);
  });
}

/** Filter to finite `[t, v]` pairs and return a copy sorted ascending by timestamp. */
export function normalizeSeriesPoints(points: readonly [number, number][]): [number, number][] {
  return points
    .filter(([t, v]) => Number.isFinite(t) && Number.isFinite(v))
    .map(([t, v]): [number, number] => [t, v])
    .sort((a, b) => a[0] - b[0]);
}

/** Compute a non-degenerate `[minT, maxT]` X-axis domain across all series. */
export function computeXRange(series: readonly Pick<SeriesLine, 'points'>[]): [number, number] {
  let minT = Number.POSITIVE_INFINITY;
  let maxT = Number.NEGATIVE_INFINITY;
  for (const line of series) {
    for (const [t, v] of line.points) {
      if (!Number.isFinite(t) || !Number.isFinite(v)) continue;
      if (t < minT) minT = t;
      if (t > maxT) maxT = t;
    }
  }
  if (!Number.isFinite(minT) || !Number.isFinite(maxT)) return [0, 1000];
  if (minT === maxT) {
    const start = Math.max(0, minT - 500);
    const end = start === minT ? minT + 1000 : minT + 500;
    return [start, end];
  }
  return [minT, maxT];
}

/** Find the timestamp across `series` closest to `targetX`, or `null` if empty. */
export function findNearestTimestamp(
  series: readonly Pick<SeriesLine, 'points'>[],
  targetX: number,
): number | null {
  if (!Number.isFinite(targetX)) return null;
  let bestT: number | null = null;
  let bestDist = Number.POSITIVE_INFINITY;
  for (const line of series) {
    for (const [t, v] of line.points) {
      if (!Number.isFinite(t) || !Number.isFinite(v)) continue;
      const dist = Math.abs(t - targetX);
      if (dist < bestDist || (dist === bestDist && bestT !== null && t < bestT)) {
        bestDist = dist;
        bestT = t;
      }
    }
  }
  return bestT;
}

/** Return the point matching `timestamp` (last match if duplicates exist), or `null`. */
export function pointAtTimestamp(
  points: readonly [number, number][],
  timestamp: number,
): [number, number] | null {
  let match: [number, number] | null = null;
  for (const pt of points) {
    if (pt[0] === timestamp && Number.isFinite(pt[1])) {
      match = pt;
    }
  }
  return match;
}
