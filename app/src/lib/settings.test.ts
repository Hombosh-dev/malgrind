import { describe, expect, it } from 'vitest';
import { changes, emptySettings, hasOverrides, parseRate, rateProblem, targetCfgProblem } from './settings';

describe('parseRate', () => {
  it('reads a whole number, and an empty field as "default"', () => {
    expect(parseRate('500')).toBe(500);
    expect(parseRate(' 997 ')).toBe(997);
    expect(parseRate('')).toBe(0);
  });
  it('gives NaN for anything else', () => {
    for (const bad of ['fast', '5e2', '-5', '1.5', '10 0']) expect(parseRate(bad), bad).toBeNaN();
  });
});

describe('rateProblem', () => {
  it('accepts the default and the engine limits', () => {
    for (const ok of [0, 10, 997, 5000]) expect(rateProblem(ok), String(ok)).toBeNull();
  });
  it('explains what is wrong', () => {
    expect(rateProblem(9)).toContain('between 10 and 5000');
    expect(rateProblem(5001)).toContain('between 10 and 5000');
    expect(rateProblem(NaN)).toContain('whole number');
  });
});

describe('overrides and changes', () => {
  const detected = { ...emptySettings(), env: 'esp32dev', rate_hz: 997 };
  it('sees typed command lines, not blanks', () => {
    expect(hasOverrides(emptySettings())).toBe(false);
    expect(hasOverrides({ build_command: '  ', upload_command: '' })).toBe(false);
    expect(hasOverrides({ build_command: '', upload_command: 'pio run -t upload' })).toBe(true);
  });
  it('includes check_memory false in emptySettings and detects memory check changes', () => {
    expect(emptySettings().check_memory).toBe(false);
    expect(changes({ ...emptySettings(), check_memory: undefined }, detected)).toEqual([]);
    expect(changes({ ...emptySettings(), check_memory: true }, detected)).toEqual(['memory checks']);
    expect(changes({ ...emptySettings(), check_memory: false }, { ...detected, check_memory: true })).toEqual([
      'memory checks',
    ]);
  });
  it('lists only what differs from what was detected', () => {
    expect(changes(emptySettings(), detected)).toEqual([]);
    expect(changes({ ...emptySettings(), env: 'esp32dev', rate_hz: 997 }, detected)).toEqual([]); // same as detected
    expect(
      changes(
        {
          ...emptySettings(),
          env: 'b',
          target_cfg: 'target/esp32s3.cfg',
          rate_hz: 500,
          extra_args: '-v',
          check_memory: true,
          build_command: 'x',
        },
        detected,
      ),
    ).toEqual(['environment', 'target config', 'sampling rate', 'extra arguments', 'memory checks', 'build command']);
  });
});

describe('targetCfgProblem', () => {
  it('accepts empty or clean target configs', () => {
    expect(targetCfgProblem('')).toBeNull();
    expect(targetCfgProblem('target/stm32f4x.cfg')).toBeNull();
    expect(targetCfgProblem('  board/esp32s3.cfg  ')).toBeNull();
  });
  it('rejects control chars, \\x1a, blanks, and leading hyphens', () => {
    expect(targetCfgProblem('   ')).toContain('blank');
    expect(targetCfgProblem('-c shutdown')).toContain('"-"');
    expect(targetCfgProblem('target/a.cfg\x1a')).toContain('control');
    expect(targetCfgProblem('target/a.cfg\n')).toContain('control');
  });
});
