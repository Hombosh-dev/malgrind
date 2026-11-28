import { describe, expect, it } from 'vitest';
import {
  computeXRange,
  computeYRange,
  findNearestTimestamp,
  formatClockMs,
  hasCap,
  normalizeSeriesPoints,
  pointAtTimestamp,
  regionTitle,
  sortTasks,
  taskStateLabel,
} from './series';
import { CAP_ALLOCS, CAP_ERRORS, CAP_HEAP, CAP_SAMPLES, CAP_TASKS, type TaskRow } from './types';

describe('hasCap', () => {
  it('checks capability bits safely and rejects undefined or zero', () => {
    expect(hasCap(undefined, CAP_HEAP)).toBe(false);
    expect(hasCap(0, CAP_SAMPLES)).toBe(false);
    expect(hasCap(Number.NaN, CAP_SAMPLES)).toBe(false);
    expect(hasCap(CAP_SAMPLES, CAP_SAMPLES)).toBe(true);
    expect(hasCap(CAP_SAMPLES, CAP_HEAP)).toBe(false);
    const v2 = CAP_SAMPLES | CAP_HEAP | CAP_TASKS;
    expect(hasCap(v2, CAP_SAMPLES)).toBe(true);
    expect(hasCap(v2, CAP_HEAP)).toBe(true);
    expect(hasCap(v2, CAP_TASKS)).toBe(true);
    expect(hasCap(v2, CAP_ALLOCS)).toBe(false);
    expect(hasCap(v2, CAP_ERRORS)).toBe(false);
  });
});

describe('regionTitle', () => {
  it('maps known heap region names and falls back to Region N', () => {
    expect(regionTitle('internal', 0)).toBe('Internal RAM');
    expect(regionTitle('external', 1)).toBe('External PSRAM');
    expect(regionTitle('iram', 2)).toBe('IRAM (exec)');
    expect(regionTitle('total', 255)).toBe('Total heap');
    expect(regionTitle(' INTERNAL ', 0)).toBe('Internal RAM');
    expect(regionTitle('region_7', 7)).toBe('Region 7');
    expect(regionTitle('', 9)).toBe('Region 9');
  });
});

describe('taskStateLabel', () => {
  it('maps known numeric states and falls back cleanly', () => {
    expect(taskStateLabel(0)).toBe('Running');
    expect(taskStateLabel(1)).toBe('Ready');
    expect(taskStateLabel(2)).toBe('Blocked');
    expect(taskStateLabel(3)).toBe('Suspended');
    expect(taskStateLabel(4)).toBe('Deleted');
    expect(taskStateLabel(99)).toBe('State 99');
  });
});

describe('computeYRange', () => {
  it('returns non-zero [0, 1] when empty, zero, or non-finite', () => {
    expect(computeYRange([])).toEqual([0, 1]);
    expect(computeYRange([0, 0, 0])).toEqual([0, 1]);
    expect(computeYRange([Number.NaN, Number.POSITIVE_INFINITY])).toEqual([0, 1]);
    expect(computeYRange([0], 0)).toEqual([0, 1]);
    expect(computeYRange([0], -100)).toEqual([0, 1]);
  });

  it('adds headroom above max value and optional positive ceilHint', () => {
    const [minA, maxA] = computeYRange([10, 50, 100]);
    expect(minA).toBe(0);
    expect(maxA).toBeGreaterThan(100);

    const [minB, maxB] = computeYRange([40, 80], 200);
    expect(minB).toBe(0);
    expect(maxB).toBeGreaterThan(200);

    const [minC, maxC] = computeYRange([300], 200);
    expect(minC).toBe(0);
    expect(maxC).toBeGreaterThan(300);
  });
});

describe('formatClockMs', () => {
  it('formats milliseconds as m:ss and clamps negative/non-finite inputs', () => {
    expect(formatClockMs(0)).toBe('0:00');
    expect(formatClockMs(-500)).toBe('0:00');
    expect(formatClockMs(Number.NaN)).toBe('0:00');
    expect(formatClockMs(999)).toBe('0:00');
    expect(formatClockMs(5_000)).toBe('0:05');
    expect(formatClockMs(65_000)).toBe('1:05');
    expect(formatClockMs(600_000)).toBe('10:00');
    expect(formatClockMs(3_661_000)).toBe('61:01');
  });
});

describe('sortTasks', () => {
  const rows: TaskRow[] = [
    { handle: 1, name: 'sensor', prio: 5, core: 1, state: 2, cpu_pct: 25.0, stack_hwm: 1024, warn: false },
    { handle: 2, name: 'blink', prio: 5, core: null, state: 1, cpu_pct: 25.0, stack_hwm: 384, warn: true },
    { handle: 3, name: 'idle0', prio: 0, core: 0, state: 0, cpu_pct: 45.5, stack_hwm: 768, warn: false },
    { handle: 4, name: 'worker', prio: 10, core: null, state: 3, cpu_pct: 4.5, stack_hwm: 2048, warn: false },
  ];

  it('sorts by cpu_pct desc with deterministic alphabetical name tie-break without mutating input', () => {
    const originalNames = rows.map((r) => r.name);
    const sorted = sortTasks(rows, 'cpu_pct', 'desc');
    expect(rows.map((r) => r.name)).toEqual(originalNames);
    // blink and sensor tie at 25.0 -> blink before sensor alphabetically
    expect(sorted.map((r) => r.name)).toEqual(['idle0', 'blink', 'sensor', 'worker']);
  });

  it('sorts by name, prio, state, and stack_hwm asc/desc', () => {
    expect(sortTasks(rows, 'name', 'asc').map((r) => r.name)).toEqual(['blink', 'idle0', 'sensor', 'worker']);
    expect(sortTasks(rows, 'name', 'desc').map((r) => r.name)).toEqual(['worker', 'sensor', 'idle0', 'blink']);
    // prio 5 tie between blink and sensor -> blink before sensor
    expect(sortTasks(rows, 'prio', 'desc').map((r) => r.name)).toEqual(['worker', 'blink', 'sensor', 'idle0']);
    expect(sortTasks(rows, 'stack_hwm', 'asc').map((r) => r.name)).toEqual(['blink', 'idle0', 'sensor', 'worker']);
    expect(sortTasks(rows, 'state', 'asc').map((r) => r.name)).toEqual(['idle0', 'blink', 'sensor', 'worker']);
  });

  it('places null core ("any") last in both asc and desc with deterministic tie-breaking', () => {
    expect(sortTasks(rows, 'core', 'asc').map((r) => r.name)).toEqual(['idle0', 'sensor', 'blink', 'worker']);
    expect(sortTasks(rows, 'core', 'desc').map((r) => r.name)).toEqual(['sensor', 'idle0', 'blink', 'worker']);
  });
});

describe('chart series helpers', () => {
  it('normalizes points by filtering non-finite entries and sorting by timestamp', () => {
    const raw: [number, number][] = [
      [3000, 30],
      [1000, 10],
      [2000, Number.NaN],
      [1500, 15],
    ];
    expect(normalizeSeriesPoints(raw)).toEqual([
      [1000, 10],
      [1500, 15],
      [3000, 30],
    ]);
  });

  it('computes non-degenerate X ranges for empty, single-point, and multi-point series', () => {
    expect(computeXRange([])).toEqual([0, 1000]);
    expect(computeXRange([{ points: [[0, 5]] }])).toEqual([0, 1000]);
    expect(computeXRange([{ points: [[5000, 5]] }])).toEqual([4500, 5500]);
    expect(
      computeXRange([
        { points: [[2000, 10], [8000, 20]] },
        { points: [[1000, 5], [6000, 15]] },
      ]),
    ).toEqual([1000, 8000]);
  });

  it('finds nearest timestamp and matching point cleanly', () => {
    const lines = [
      { points: [[1000, 10], [3000, 30], [5000, 50]] as [number, number][] },
      { points: [[1000, 4], [5000, 20]] as [number, number][] },
    ];
    expect(findNearestTimestamp([], 1000)).toBeNull();
    expect(findNearestTimestamp(lines, 2700)).toBe(3000);
    // Equidistant between 1000 and 3000 -> earlier timestamp 1000
    expect(findNearestTimestamp(lines, 2000)).toBe(1000);
    expect(pointAtTimestamp(lines[0].points, 3000)).toEqual([3000, 30]);
    expect(pointAtTimestamp(lines[1].points, 3000)).toBeNull();
  });
});
