import { describe, expect, it } from 'vitest';
import { accessProblem, boardDetail } from './boards';

describe('accessProblem', () => {
  it('is null when the port can be opened', () => {
    expect(accessProblem({ access: { ok: true, group: 'dialout', in_group: true, fixable: false } })).toBeNull();
  });
  it('names the group the user is missing', () => {
    expect(accessProblem({ access: { ok: false, group: 'dialout', in_group: false, fixable: true } })).toContain('"dialout"');
  });
  it('still says something when the group is unknown or not fixable', () => {
    expect(accessProblem({ access: { ok: false, group: '', in_group: false, fixable: false } })).toMatch(/^No permission/);
  });
});

describe('boardDetail', () => {
  it('tells identical boards apart by socket and serial number', () => {
    expect(boardDetail({ location: '3-3', serial: '0001' })).toBe('USB 3-3 · serial 0001');
    expect(boardDetail({ location: '3-4', serial: '' })).toBe('USB 3-4');
    expect(boardDetail({ location: '', serial: '' })).toBe('');
  });
});
