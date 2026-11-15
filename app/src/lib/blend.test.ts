import { describe, expect, it } from 'vitest';
import { blend, type Node } from './blend';

const leaf = (name: string, value: number): Node => ({ name, value, children: [] });
const tree = (a: number, b: number): Node => ({ name: 'all', value: a + b, children: [leaf('a', a), leaf('b', b)] });
const sum = (n: Node) => n.children.reduce((s, c) => s + c.value, 0);

describe('blend', () => {
  it('starts from nothing: the first picture is k times the snapshot', () => {
    const shown = blend(null, tree(80, 20), 0, 0.25);
    expect(shown.value).toBe(25);
    expect(shown.children.map((c) => c.value)).toEqual([20, 5]);
  });

  it('keeps a parent equal to the sum of its children', () => {
    let shown: Node | null = null;
    for (const [a, b] of [[80, 20], [10, 90], [50, 50]]) shown = blend(shown, tree(a, b), 0);
    expect(shown!.value).toBeCloseTo(sum(shown!));
  });

  it('lets a frame that disappeared fade out and drops it below the floor', () => {
    let shown: Node | null = tree(0, 100);
    const next: Node = { name: 'all', value: 100, children: [leaf('b', 100)] }; // `a` is gone
    shown = blend(shown, next, 1);
    expect(shown.children.find((c) => c.name === 'a')).toBeUndefined(); // 0 is not above the floor
    shown = blend({ name: 'all', value: 100, children: [leaf('a', 100)] }, next, 1);
    expect(shown.children.find((c) => c.name === 'a')!.value).toBe(75); // fading, not gone
    for (let i = 0; i < 30; i++) shown = blend(shown, next, 1);
    expect(shown.children.map((c) => c.name)).toEqual(['b']);
  });

  it('converges to the snapshot', () => {
    let shown: Node | null = null;
    for (let i = 0; i < 60; i++) shown = blend(shown, tree(30, 70), 0);
    expect(shown!.value).toBeCloseTo(100, 1);
  });
});
