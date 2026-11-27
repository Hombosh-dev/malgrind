import { describe, expect, it } from 'vitest';
import { baseName, defaultRunName, formatBytes, formatDuration, formatRunStats, formatWhen, isProbeDeviceId } from './format';

describe('format', () => {
  it('formats durations', () => {
    expect(formatDuration(0)).toBe('0 s');
    expect(formatDuration(45_400)).toBe('45 s');
    expect(formatDuration(12 * 60_000)).toBe('12 min');
    expect(formatDuration(65 * 60_000)).toBe('1 h 05 min');
  });
  it('formats byte sizes across units and handles invalid inputs', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(-42)).toBe('0 B');
    expect(formatBytes(Number.NaN)).toBe('0 B');
    expect(formatBytes(Number.POSITIVE_INFINITY)).toBe('0 B');
    expect(formatBytes(512)).toBe('512 B');
    expect(formatBytes(1023)).toBe('1023 B');
    expect(formatBytes(1536)).toBe('1.5 KB');
    expect(formatBytes(12.3 * 1024 * 1024)).toBe('12.3 MB');
    expect(formatBytes(1.2 * 1024 * 1024 * 1024)).toBe('1.20 GB');
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
  it('formats run stats with null or missing overhead without null% or NaN%', () => {
    expect(formatRunStats({ samples_per_s: 120.5, dropped: 0, overhead_pct: null })).toBe(
      '120.5 samples/s · 0 dropped · — overhead',
    );
    expect(formatRunStats({ samples_per_s: 2000, dropped: 2, overhead_pct: 1.5 })).toBe(
      '2000 samples/s · 2 dropped · 1.5 % overhead',
    );
    expect(formatRunStats({ samples_per_s: Number.NaN, dropped: undefined, overhead_pct: undefined })).toBe(
      '– samples/s · 0 dropped · — overhead',
    );
  });
  it('detects probe device ids', () => {
    expect(isProbeDeviceId('probe-0483-374b-sn-1')).toBe(true);
    expect(isProbeDeviceId('usb-10c4-ea60-sn-1')).toBe(false);
    expect(isProbeDeviceId(undefined)).toBe(false);
  });
});
