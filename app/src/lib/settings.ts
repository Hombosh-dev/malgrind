// Small pure helpers of the run settings panel (the engine checks every value; this is for instant feedback).
import type { RunSettings } from './types';

export const defaultRate = 997;

// Settings that change nothing: every field empty or 0.
export function emptySettings(): RunSettings {
  return {
    env: '',
    extra_args: '',
    build_flags: '',
    rate_hz: 0,
    check_memory: false,
    elf: '',
    build_command: '',
    upload_command: '',
    target_cfg: '',
  };
}

// A message when the OpenOCD target config path is invalid, else null. Mirrors sanitize_target_cfg (host/src/settings.cpp).
export function targetCfgProblem(targetCfg: string): string | null {
  if (targetCfg === '') return null;
  for (let i = 0; i < targetCfg.length; i++) {
    const code = targetCfg.charCodeAt(i);
    if (code < 0x20 || code === 0x7f) return 'Target config must not contain control characters.';
  }
  const trimmed = targetCfg.trim();
  if (trimmed === '') return 'Target config must not be blank.';
  if (trimmed.startsWith('-')) return 'Target config must not start with "-".';
  return null;
}

// The value of the rate field as a number: a whole number from the field, 0 for "use the default" (an empty field).
// NaN for text that is not a number, so the panel can say so.
export function parseRate(text: string): number {
  const t = text.trim();
  if (t === '') return 0;
  return /^\d+$/.test(t) ? Number(t) : NaN;
}

// A message when the rate cannot be right, else null. The limits are the engine's (host/src/settings.hpp).
export function rateProblem(rate: number): string | null {
  if (Number.isNaN(rate)) return 'The rate must be a whole number.';
  if (rate !== 0 && (rate < 10 || rate > 5000)) return 'The rate must be between 10 and 5000 Hz.';
  return null;
}

// Whether the user typed their own command lines (they replace the detected ones).
export function hasOverrides(s: Pick<RunSettings, 'build_command' | 'upload_command'>): boolean {
  return s.build_command.trim() !== '' || s.upload_command.trim() !== '';
}

// What differs from the detected values, for a short "you changed: ..." line. Order follows the panel.
export function changes(s: RunSettings, detected: RunSettings): string[] {
  const out: string[] = [];
  if (s.env && s.env !== detected.env) out.push('environment');
  if ((s.target_cfg ?? '').trim() && (s.target_cfg ?? '').trim() !== (detected.target_cfg ?? '').trim()) {
    out.push('target config');
  }
  if (s.rate_hz && s.rate_hz !== (detected.rate_hz || defaultRate)) out.push('sampling rate');
  if (s.elf && s.elf !== detected.elf) out.push('firmware ELF');
  if (s.extra_args.trim()) out.push('extra arguments');
  if (s.build_flags.trim()) out.push('build flags');
  if (!!s.check_memory !== !!detected.check_memory) out.push('memory checks');
  if (s.build_command.trim()) out.push('build command');
  if (s.upload_command.trim()) out.push('upload command');
  return out;
}
