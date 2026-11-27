import type { RunStats } from './types';

// "45 s", "12 min", "1 h 05 min"
export function formatDuration(ms: number): string {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min`;
  return `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, '0')} min`;
}

// "125.3 samples/s · 0 dropped · — overhead" (handles null/missing overhead cleanly without "null%" or "NaN%")
export function formatRunStats(stats: Pick<RunStats, 'samples_per_s' | 'dropped' | 'overhead_pct'>): string {
  const rate =
    stats.samples_per_s !== undefined && Number.isFinite(stats.samples_per_s) ? String(stats.samples_per_s) : '–';
  const dropped = stats.dropped !== undefined && Number.isFinite(stats.dropped) ? stats.dropped : 0;
  const overhead =
    stats.overhead_pct !== undefined && stats.overhead_pct !== null && Number.isFinite(stats.overhead_pct)
      ? `${stats.overhead_pct} %`
      : '—';
  return `${rate} samples/s · ${dropped} dropped · ${overhead} overhead`;
}

export function isProbeDeviceId(deviceId?: string | null): boolean {
  return (deviceId ?? '').startsWith('probe-');
}

// "0 B", "512 B", "1.5 KB", "12.3 MB", "1.20 GB"
export function formatBytes(bytes: number): string {
  if (!Number.isFinite(bytes) || bytes <= 0) return '0 B';
  if (bytes < 1024) return `${Math.floor(bytes)} B`;
  const kb = bytes / 1024;
  if (kb < 1024) return `${kb.toFixed(1)} KB`;
  const mb = kb / 1024;
  if (mb < 1024) return `${mb.toFixed(1)} MB`;
  return `${(mb / 1024).toFixed(2)} GB`;
}

// Local date and time of an ISO timestamp, "15 Nov 2026, 22:01"
export function formatWhen(iso: string): string {
  const date = new Date(iso);
  return Number.isNaN(date.getTime())
    ? iso
    : date.toLocaleString(undefined, { day: 'numeric', month: 'short', year: 'numeric', hour: '2-digit', minute: '2-digit' });
}

// Last folder of a path, for naming runs after their project.
export function baseName(path: string): string {
  return path.replace(/\/+$/, '').split('/').pop() ?? '';
}

export function defaultRunName(project: string, now = new Date()): string {
  const stamp = now.toLocaleString(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  return `${baseName(project) || 'board'} ${stamp}`;
}
