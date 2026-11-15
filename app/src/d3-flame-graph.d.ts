declare module 'd3-flame-graph' {
  // The parts of the chart API this app uses (the package ships no types).
  export interface FlameGraph {
    (selection: unknown): void;
    width(width: number): FlameGraph;
    cellHeight(height: number): FlameGraph;
    sort(on: boolean): FlameGraph;
    transitionDuration(ms: number): FlameGraph;
    update(data: unknown): void;
    resetZoom(): void;
    destroy(): void;
  }
  export function flamegraph(): FlameGraph;
}
