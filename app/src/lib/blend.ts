// Samples arrive in bursts and a short window holds only a few dozen workload cycles, so raw shares wobble.
// Every node value moves a fraction `k` of the way to the newest snapshot (exponential smoothing). It is linear,
// so a parent stays the sum of its children. A node that vanishes fades out; below `floor` it is dropped.
export interface Node {
  name: string;
  value: number;
  children: Node[];
}

export function blend(prev: Node | null, next: Node | null, floor: number, k = 0.25): Node {
  const pv = prev ? prev.value : 0;
  const nv = next ? next.value : 0;
  const previous = new Map((prev?.children ?? []).map((c) => [c.name, c]));
  const newest = new Map((next?.children ?? []).map((c) => [c.name, c]));
  const children = [...new Set([...previous.keys(), ...newest.keys()])]
    .map((name) => blend(previous.get(name) ?? null, newest.get(name) ?? null, floor, k))
    .filter((c) => c.value > floor);
  return { name: (next ?? prev)!.name, value: pv + k * (nv - pv), children };
}
