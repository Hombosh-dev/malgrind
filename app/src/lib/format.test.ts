import { describe, expect, it } from 'vitest';
import { baseName, defaultRunName, formatDuration, formatWhen } from './format';

describe('format', () => {
  it('formats durations', () => {
    expect(formatDuration(0)).toBe('0 s');
    expect(formatDuration(45_400)).toBe('45 s');
    expect(formatDuration(12 * 60_000)).toBe('12 min');
    expect(formatDuration(65 * 60_000)).toBe('1 h 05 min');
  });
  it('keeps a timestamp it cannot read', () => {
    expect(formatWhen('not a date')).toBe('not a date');
    expect(formatWhen('2026-11-15T20:01:02Z')).toContain('2026');
  });
  it('names runs after the project folder', () => {
    expect(baseName('/home/me/water-sim/')).toBe('water-sim');
    expect(defaultRunName('/home/me/water-sim').startsWith('water-sim ')).toBe(true);
    expect(defaultRunName('').startsWith('board ')).toBe(true);
  });
});
