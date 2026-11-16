// Small pure helpers of the board tiles.
import type { BoardPort } from './types';

// Why this user cannot use the port, or null when they can.
export function accessProblem(board: Pick<BoardPort, 'access'>): string | null {
  if (board.access.ok) return null;
  return board.access.group
    ? `No permission to use this port: you are not in the group "${board.access.group}".`
    : 'No permission to use this port.';
}

// What tells two boards of the same kind apart: where they are plugged in, and the adapter's serial number.
export function boardDetail(board: Pick<BoardPort, 'location' | 'serial'>): string {
  return [board.location && `USB ${board.location}`, board.serial && `serial ${board.serial}`].filter(Boolean).join(' · ');
}
