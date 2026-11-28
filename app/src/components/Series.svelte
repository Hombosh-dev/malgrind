<script lang="ts">
  import { line, scaleLinear } from 'd3';
  import { onMount } from 'svelte';
  import {
    computeXRange,
    computeYRange,
    findNearestTimestamp,
    formatClockMs,
    normalizeSeriesPoints,
    pointAtTimestamp,
  } from '../lib/series';
  import type { SeriesLine } from '../lib/types';

  interface Props {
    series: readonly SeriesLine[];
    height?: number;
    referenceY?: number | null;
    referenceLabel?: string;
    formatY?: (value: number) => string;
    formatX?: (ms: number) => string;
    title?: string;
    emptyText?: string;
  }

  let {
    series,
    height = 200,
    referenceY = null,
    referenceLabel = 'Total',
    formatY = (v: number) => String(v),
    formatX = formatClockMs,
    title = '',
    emptyText = 'No data in this window.',
  }: Props = $props();

  let host = $state<HTMLDivElement | null>(null);
  let containerWidth = $state(480);
  let hoverT = $state<number | null>(null);

  onMount(() => {
    if (!host) return;
    containerWidth = host.clientWidth || 480;
    const resize = new ResizeObserver((entries) => {
      const w = entries[0]?.contentRect.width ?? host?.clientWidth ?? 0;
      if (w > 0) containerWidth = w;
    });
    resize.observe(host);
    return () => resize.disconnect();
  });

  const normalized = $derived(
    series.map((s) => ({
      ...s,
      points: normalizeSeriesPoints(s.points),
    })),
  );

  const hasData = $derived(normalized.some((s) => s.points.length > 0));
  const hasReference = $derived(
    referenceY !== null && referenceY !== undefined && Number.isFinite(referenceY) && referenceY > 0,
  );

  const svgWidth = $derived(Math.max(240, Math.floor(containerWidth)));
  const svgHeight = $derived(Math.max(84, height));
  const margin = { top: 10, right: 16, bottom: 22, left: 68 };
  const innerWidth = $derived(Math.max(40, svgWidth - margin.left - margin.right));
  const innerHeight = $derived(Math.max(24, svgHeight - margin.top - margin.bottom));

  const xDomain = $derived(computeXRange(normalized));
  const yDomain = $derived(
    computeYRange(
      normalized.flatMap((s) => s.points.map((p) => p[1])),
      hasReference ? (referenceY as number) : undefined,
    ),
  );

  const xScale = $derived(scaleLinear().domain(xDomain).range([0, innerWidth]));
  const yScale = $derived(scaleLinear().domain(yDomain).range([innerHeight, 0]));

  const lineGenerator = $derived(
    line<[number, number]>()
      .x((d) => xScale(d[0]))
      .y((d) => yScale(d[1])),
  );

  const paths = $derived(
    normalized.map((s) => ({
      label: s.label,
      color: s.color,
      points: s.points,
      d: s.points.length >= 2 ? (lineGenerator(s.points) ?? '') : '',
    })),
  );

  const yTicks = $derived.by(() => {
    const count = svgHeight < 140 ? 3 : 4;
    return yScale.ticks(count).map((value) => ({
      value,
      y: yScale(value),
      label: formatY(value),
    }));
  });

  const xTicks = $derived.by(() => {
    const count = Math.max(2, Math.min(6, Math.floor(innerWidth / 90)));
    const seen = new Set<string>();
    const out: { value: number; x: number; label: string }[] = [];
    for (const value of xScale.ticks(count)) {
      const label = formatX(value);
      if (seen.has(label)) continue;
      seen.add(label);
      out.push({ value, x: xScale(value), label });
    }
    return out;
  });

  const refLineY = $derived(hasReference ? yScale(referenceY as number) : null);

  const hoverRows = $derived.by(() => {
    if (hoverT === null) return [];
    const rows = normalized.map((s) => {
      const pt = pointAtTimestamp(s.points, hoverT as number);
      return {
        label: s.label,
        color: s.color,
        value: pt ? pt[1] : null,
      };
    });
    return rows.some((r) => r.value !== null) ? rows : [];
  });

  const hoverPlotX = $derived(hoverT !== null && hoverRows.length > 0 ? xScale(hoverT) : null);
  const tooltipOnRight = $derived(hoverPlotX !== null && hoverPlotX < innerWidth * 0.58);

  function handlePointerMove(e: PointerEvent & { currentTarget: SVGSVGElement }) {
    if (!hasData) return;
    const rect = e.currentTarget.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return;
    const svgX = ((e.clientX - rect.left) / rect.width) * svgWidth;
    const svgY = ((e.clientY - rect.top) / rect.height) * svgHeight;
    if (
      svgX < margin.left ||
      svgX > margin.left + innerWidth ||
      svgY < margin.top ||
      svgY > margin.top + innerHeight
    ) {
      hoverT = null;
      return;
    }
    const localX = Math.max(0, Math.min(innerWidth, svgX - margin.left));
    hoverT = findNearestTimestamp(normalized, xScale.invert(localX));
  }

  function clearHover() {
    hoverT = null;
  }
</script>

<div class="series">
  {#if title || normalized.length > 0 || hasReference}
    <div class="head">
      {#if title}<span class="title" {title}>{title}</span>{/if}
      <div class="legend" aria-label="Chart legend">
        {#each normalized as s (s.label)}
          <span class="leg-item">
            <span class="swatch" style:background={s.color} aria-hidden="true"></span>
            <span>{s.label}</span>
          </span>
        {/each}
        {#if hasReference}
          <span class="leg-item">
            <span class="swatch dashed" aria-hidden="true"></span>
            <span>{referenceLabel} ({formatY(referenceY as number)})</span>
          </span>
        {/if}
      </div>
    </div>
  {/if}

  <div class="chart-wrap" bind:this={host}>
    {#if !hasData}
      <p class="muted empty">{emptyText}</p>
    {:else}
      <svg
        width={svgWidth}
        height={svgHeight}
        viewBox="0 0 {svgWidth} {svgHeight}"
        role="img"
        aria-label={title || 'Time series chart'}
        class:interactive={hasData}
        onpointermove={handlePointerMove}
        onpointerleave={clearHover}
        onpointercancel={clearHover}
      >
        <g transform="translate({margin.left},{margin.top})">
          {#each yTicks as tick (tick.value)}
            <line class="grid" x1={0} x2={innerWidth} y1={tick.y} y2={tick.y} />
            <text class="tick y-tick" x={-8} y={tick.y} dy="0.32em" text-anchor="end">{tick.label}</text>
          {/each}

          <line class="axis" x1={0} x2={innerWidth} y1={innerHeight} y2={innerHeight} />
          {#each xTicks as tick (tick.value)}
            <line class="tick-mark" x1={tick.x} x2={tick.x} y1={innerHeight} y2={innerHeight + 4} />
            <text class="tick x-tick" x={tick.x} y={innerHeight + 15} text-anchor="middle">{tick.label}</text>
          {/each}

          {#if refLineY !== null}
            <line class="ref-line" x1={0} x2={innerWidth} y1={refLineY} y2={refLineY} />
            <text
              class="ref-label"
              x={innerWidth - 4}
              y={refLineY < 14 ? refLineY + 11 : refLineY - 4}
              text-anchor="end"
            >
              {referenceLabel}
            </text>
          {/if}

          {#each paths as p (p.label)}
            {#if p.d}
              <path class="line" d={p.d} stroke={p.color} />
            {:else if p.points.length === 1}
              <circle cx={xScale(p.points[0][0])} cy={yScale(p.points[0][1])} r="3.5" fill={p.color} />
            {/if}
          {/each}

          {#if hoverPlotX !== null && hoverT !== null}
            <line class="crosshair" x1={hoverPlotX} x2={hoverPlotX} y1={0} y2={innerHeight} />
            {#each hoverRows as row (row.label)}
              {#if row.value !== null}
                <circle class="hover-dot" cx={hoverPlotX} cy={yScale(row.value)} r="3.5" fill={row.color} />
              {/if}
            {/each}
          {/if}
        </g>
      </svg>

      {#if hoverPlotX !== null && hoverT !== null}
        <div
          class="tooltip"
          style:left={tooltipOnRight ? `${Math.min(svgWidth - 16, margin.left + hoverPlotX + 10)}px` : undefined}
          style:right={!tooltipOnRight ? `${Math.max(8, svgWidth - (margin.left + hoverPlotX) + 10)}px` : undefined}
        >
          <div class="tt-time">{formatX(hoverT)}</div>
          {#each hoverRows as row (row.label)}
            {#if row.value !== null}
              <div class="tt-row">
                <span class="swatch" style:background={row.color} aria-hidden="true"></span>
                <span class="tt-label">{row.label}</span>
                <span class="tt-val">{formatY(row.value)}</span>
              </div>
            {/if}
          {/each}
        </div>
      {/if}
    {/if}
  </div>
</div>

<style>
  .series {
    display: flex;
    flex-direction: column;
    gap: 6px;
    min-width: 0;
  }
  .head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 6px 14px;
  }
  .title {
    font: 600 12.5px var(--mono);
    color: var(--fg);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    max-width: 100%;
  }
  .legend {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px 14px;
    font-size: 12px;
    color: var(--muted);
    margin-left: auto;
  }
  .leg-item {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    white-space: nowrap;
  }
  .swatch {
    width: 12px;
    height: 2.5px;
    border-radius: 2px;
    display: inline-block;
    flex-shrink: 0;
  }
  .swatch.dashed {
    height: 0;
    border-top: 2px dashed var(--muted);
  }
  .chart-wrap {
    position: relative;
    width: 100%;
    min-width: 0;
  }
  svg {
    display: block;
    width: 100%;
    overflow: visible;
    user-select: none;
  }
  .grid {
    stroke: var(--line);
    stroke-opacity: 0.7;
    stroke-width: 1;
  }
  .axis {
    stroke: var(--line);
    stroke-width: 1;
  }
  .tick-mark {
    stroke: var(--line);
    stroke-width: 1;
  }
  .tick {
    fill: var(--muted);
    font: 11px var(--mono);
    font-variant-numeric: tabular-nums;
  }
  .ref-line {
    stroke: var(--muted);
    stroke-width: 1.25;
    stroke-dasharray: 4 4;
    stroke-opacity: 0.85;
  }
  .ref-label {
    fill: var(--muted);
    font: 10.5px var(--mono);
  }
  .line {
    fill: none;
    stroke-width: 2;
    stroke-linejoin: round;
    stroke-linecap: round;
  }
  .crosshair {
    stroke: var(--muted);
    stroke-width: 1;
    stroke-dasharray: 3 3;
    pointer-events: none;
  }
  .hover-dot {
    stroke: var(--panel);
    stroke-width: 1.5;
    pointer-events: none;
  }
  svg.interactive {
    cursor: crosshair;
  }
  .tooltip {
    position: absolute;
    top: 6px;
    pointer-events: none;
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 8px;
    padding: 6px 9px;
    box-shadow: 0 6px 18px #00000024;
    font-size: 11.5px;
    z-index: 5;
    white-space: nowrap;
  }
  .tt-time {
    font: 600 11px var(--mono);
    color: var(--muted);
    margin-bottom: 3px;
  }
  .tt-row {
    display: flex;
    align-items: center;
    gap: 6px;
    line-height: 1.45;
  }
  .tt-label {
    color: var(--muted);
  }
  .tt-val {
    margin-left: auto;
    padding-left: 8px;
    font: 600 11.5px var(--mono);
    font-variant-numeric: tabular-nums;
    color: var(--fg);
  }
  .empty {
    margin: 8px 0;
    font-size: 13px;
  }
</style>
