declare module 'd3-flame-graph' {
  // The parts of the chart API this app uses (the package ships no types).
  export interface FlameGraphNode {
    data: any;
    value?: number;
    x0?: number;
    x1?: number;
  }
  export type ColorMapper = (node: FlameGraphNode, originalColor: string) => string;
  export type LabelHandler = (node: FlameGraphNode) => string;
  export type DetailsHandler = (text: string | null) => void;

  export interface FlameGraph {
    (selection: unknown): void;
    width(width: number): FlameGraph;
    cellHeight(height: number): FlameGraph;
    minFrameSize(size: number): FlameGraph;
    sort(on: boolean): FlameGraph;
    transitionDuration(ms: number): FlameGraph;
    resetHeightOnZoom(on: boolean): FlameGraph;
    setColorMapper(fn: ColorMapper): FlameGraph;
    setLabelHandler(fn: LabelHandler): FlameGraph;
    label(fn: LabelHandler): FlameGraph;
    setDetailsHandler(fn: DetailsHandler): FlameGraph;
    update(data?: unknown): void;
    resetZoom(): void;
    destroy(): void;
  }
  export function flamegraph(): FlameGraph;
}
