<script lang="ts">
  import { select } from 'd3';
  import { flamegraph, type FlameGraph } from 'd3-flame-graph';
  import 'd3-flame-graph/dist/d3-flamegraph.css';
  import { onMount } from 'svelte';
  import { blend, type Node } from '../lib/blend';

  interface Props {
    tree: Node | null; // newest snapshot
    resetKey: string; // a change (core, window, time) restarts the smoothing
  }
  let { tree, resetKey }: Props = $props();

  let host: HTMLElement;
  let chart: FlameGraph | null = null;
  let shown: Node | null = null;
  let drawnKey = '';

  const width = () => Math.max(320, host.clientWidth);

  export function getSvg(): SVGSVGElement | null {
    return host?.querySelector('svg') ?? null;
  }

  function draw() {
    if (!tree || tree.value <= 0) {
      host.replaceChildren();
      chart = null;
      shown = null;
      return;
    }
    if (resetKey !== drawnKey) shown = null;
    drawnKey = resetKey;
    shown = blend(shown, tree, tree.value * 0.0005);
    if (!chart) {
      // no animation: a transition would still be running when the next update arrives, which looks like jitter
      chart = flamegraph().width(width()).cellHeight(18).sort(true).transitionDuration(0);
      select(host).datum(shown).call(chart as never);
    } else {
      chart.update(shown);
    }
  }

  $effect(() => {
    void [tree, resetKey];
    if (host) draw();
  });

  onMount(() => {
    const resize = new ResizeObserver(() => {
      if (chart) {
        chart.width(width());
        chart.update(shown);
      }
    });
    resize.observe(host);
    return () => resize.disconnect();
  });
</script>

<div bind:this={host} class="flame"></div>
{#if !tree || tree.value <= 0}<p class="muted empty">No samples in this window.</p>{/if}

<style>
  .flame { width: 100%; }
  .empty { padding: 16px 0; }
  /* d3-flame-graph draws in its own light theme: keep its text readable in both themes */
  .flame :global(.d3-flame-graph-label) { color: #1a1208; }
</style>
