// "45 s", "12 min", "1 h 05 min"
export function formatDuration(ms: number): string {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s} s`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min`;
  return `${Math.floor(m / 60)} h ${String(m % 60).padStart(2, '0')} min`;
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
