<script lang="ts">
  import { onMount } from 'svelte';
  import { select } from 'd3';
  import { flamegraph, type FlameGraph } from 'd3-flame-graph';
  import 'd3-flame-graph/dist/d3-flamegraph.css';
  import TopBar from '../components/TopBar.svelte';
  import {
    diffColor,
    filterDiffFunctions,
    formatDelta,
    nodeDeltaPct,
    nodeSharePct,
    sortDiffFunctions,
    type DiffSortKey,
    type SortDir,
  } from '../lib/diff';
  import { formatDuration } from '../lib/format';
  import type { Sidecar } from '../lib/sidecar';
  import type { DiffNode, DiffResult, RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    a: RunMeta;
    b: RunMeta;
    onBack(): void;
  }
  let { sidecar, a, b, onBack }: Props = $props();

  type TableColKey = Exclude<DiffSortKey, 'abs_self'>;
  const COLUMNS: { key: TableColKey; label: string; align: 'left' | 'right' }[] = [
    { key: 'name', label: 'Function', align: 'left' },
    { key: 'a_self', label: 'A self %', align: 'right' },
    { key: 'b_self', label: 'B self %', align: 'right' },
    { key: 'delta_self', label: 'Δ self', align: 'right' },
    { key: 'a_incl', label: 'A incl %', align: 'right' },
    { key: 'b_incl', label: 'B incl %', align: 'right' },
    { key: 'delta_incl', label: 'Δ incl', align: 'right' },
  ];

  let swapped = $state(false);
  const runA = $derived(swapped ? b : a);
  const runB = $derived(swapped ? a : b);

  let core = $state<'all' | number>('all');
  let diff = $state.raw<DiffResult | null>(null);
  let loading = $state(true);
  let error = $state('');

  let filter = $state('');
  let sortKey = $state<DiffSortKey>('abs_self');
  let sortDir = $state<SortDir>('desc');
  let hoveredFrame = $state('');

  let flameHost: HTMLElement;
  let chart: FlameGraph | null = null;
  let requestSeq = 0;
  let destroyed = false;

  const maxCores = $derived(Math.max(diff?.a.ncores ?? runA.ncores ?? 0, diff?.b.ncores ?? runB.ncores ?? 0));
  const cores = $derived(Array.from({ length: maxCores }, (_, i) => i));

  const filteredFunctions = $derived(diff ? filterDiffFunctions(diff.functions, filter) : []);
  const sortedFunctions = $derived(sortDiffFunctions(filteredFunctions, sortKey, sortDir));

  const flameWidth = () => Math.max(320, flameHost?.clientWidth || 320);

  function cloneTree(node: DiffNode): DiffNode {
    return { ...node, children: node.children.map(cloneTree) };
  }

  function formatNodeSummary(node: DiffNode, totalA: number, totalB: number): string {
    const aPct = nodeSharePct(node.a, totalA);
    const bPct = nodeSharePct(node.b, totalB);
    const delta = nodeDeltaPct(node, totalA, totalB);
    return `${node.name} · A ${aPct.toFixed(1)}% (${node.a}) → B ${bPct.toFixed(1)}% (${node.b}) · Δ ${formatDelta(delta)}`;
  }

  function renderFlame(res: DiffResult | null) {
    if (!flameHost) return;
    chart?.destroy();
    chart = null;
    flameHost.replaceChildren();
    hoveredFrame = '';

    if (!res || res.tree.value <= 0) return;

    chart = flamegraph()
      .width(flameWidth())
      .cellHeight(18)
      .sort(true)
      .transitionDuration(0)
      .resetHeightOnZoom(true)
      .setColorMapper((d) => diffColor(nodeDeltaPct(d.data as DiffNode, res.total_a, res.total_b)))
      .label((d) => formatNodeSummary(d.data as DiffNode, res.total_a, res.total_b))
      .setDetailsHandler((text) => {
        hoveredFrame = text ?? '';
      });

    select(flameHost).datum(cloneTree(res.tree)).call(chart as never);
  }

  function toggleSort(col: TableColKey) {
    if (col === 'delta_self') {
      if (sortKey === 'abs_self') {
        sortKey = 'delta_self';
        sortDir = 'desc';
      } else if (sortKey === 'delta_self' && sortDir === 'desc') {
        sortDir = 'asc';
      } else {
        sortKey = 'abs_self';
        sortDir = 'desc';
      }
      return;
    }
    if (sortKey === col) {
      sortDir = sortDir === 'desc' ? 'asc' : 'desc';
    } else {
      sortKey = col;
      sortDir = col === 'name' ? 'asc' : 'desc';
    }
  }

  function sortIndicator(col: TableColKey): string {
    if (col === 'delta_self' && sortKey === 'abs_self') return '|Δ| ↓';
    if (sortKey !== col) return '';
    return sortDir === 'asc' ? '↑' : '↓';
  }

  function ariaSort(col: TableColKey): 'ascending' | 'descending' | 'none' {
    if (col === 'delta_self' && sortKey === 'abs_self') return 'descending';
    if (sortKey !== col) return 'none';
    return sortDir === 'asc' ? 'ascending' : 'descending';
  }

  $effect(() => {
    const aId = runA.id;
    const bId = runB.id;
    const selectedCore = core;
    const seq = ++requestSeq;
    loading = true;
    error = '';

    const args: Record<string, unknown> = { a: aId, b: bId };
    if (selectedCore !== 'all') args.core = selectedCore;

    sidecar
      .call<DiffResult>('diff', args)
      .then((res) => {
        if (destroyed || seq !== requestSeq) return;
        diff = res;
        loading = false;
      })
      .catch((e) => {
        if (destroyed || seq !== requestSeq) return;
        diff = null;
        error = String(e instanceof Error ? e.message : e);
        loading = false;
      });
  });

  $effect(() => {
    renderFlame(diff);
  });

  onMount(() => {
    const resize = new ResizeObserver(() => {
      if (chart && flameHost) {
        chart.width(flameWidth());
        chart.update();
      }
    });
    resize.observe(flameHost);
    return () => {
      destroyed = true;
      resize.disconnect();
      chart?.destroy();
      chart = null;
    };
  });
</script>

<div class="page">
  <TopBar title="Compare runs" {onBack} backLabel="Back">
    <label class="core-filter">
      Core
      <select bind:value={core} aria-label="Core filter">
        <option value="all">all</option>
        {#each cores as c}
          <option value={c}>{c}</option>
        {/each}
      </select>
    </label>
    <button class="btn ghost small" type="button" onclick={() => (swapped = !swapped)}>Swap A ↔ B</button>
  </TopBar>

  <div class="summary-bar">
    <div class="run-pill">
      <span class="badge">A · Baseline</span>
      <b class="run-name" title={runA.name || runA.id}>{runA.name || runA.id}</b>
      <span class="muted meta">{formatDuration(runA.duration_ms)}</span>
      {#if diff}<span class="samples">{diff.total_a.toLocaleString()} samples</span>{/if}
    </div>
    <span class="arrow muted" aria-hidden="true">→</span>
    <div class="run-pill">
      <span class="badge target">B · Target</span>
      <b class="run-name" title={runB.name || runB.id}>{runB.name || runB.id}</b>
      <span class="muted meta">{formatDuration(runB.duration_ms)}</span>
      {#if diff}<span class="samples">{diff.total_b.toLocaleString()} samples</span>{/if}
    </div>
    {#if loading}<span class="muted status">Updating…</span>{/if}
  </div>

  {#if error}<div class="notice bad" role="alert">{error}</div>{/if}

  <main>
    <section class="card">
      <div class="section-head">
        <div class="title-group">
          <h2>Differential flamegraph</h2>
          <div class="legend">
            <span class="swatch pos" aria-hidden="true"></span>
            <span class="swatch neutral" aria-hidden="true"></span>
            <span class="swatch neg" aria-hidden="true"></span>
            <span class="muted">Red = grew in B · Blue = shrank in B · Width = B</span>
          </div>
        </div>
        <button
          class="btn ghost small"
          type="button"
          disabled={!diff || diff.tree.value <= 0}
          onclick={() => chart?.resetZoom()}
        >
          Reset zoom
        </button>
      </div>

      <p class="hover-line" class:muted={!hoveredFrame}>
        {hoveredFrame || 'Hover a frame for A %, B %, and Δ % · Click a frame to zoom'}
      </p>

      <div bind:this={flameHost} class="flame" class:hidden={!diff || diff.tree.value <= 0}></div>
      {#if loading && !diff}
        <p class="muted empty">Computing comparison…</p>
      {:else if diff && diff.tree.value <= 0}
        <p class="muted empty">
          No samples in run B{core !== 'all' ? ` on core ${core}` : ''} (frame widths come from B). Try Swap A ↔ B or another core.
        </p>
      {/if}
    </section>

    {#if diff}
      <section class="card">
        <div class="section-head">
          <h2>Functions <span class="count">{sortedFunctions.length}{filter.trim() ? ` / ${diff.functions.length}` : ''}</span></h2>
          <input
            type="search"
            class="filter-input"
            bind:value={filter}
            placeholder="Filter functions…"
            aria-label="Filter functions"
          />
        </div>

        {#if sortedFunctions.length === 0}
          <p class="muted empty">
            {filter.trim() ? `No functions match "${filter.trim()}".` : 'No function samples in either run for this filter.'}
          </p>
        {:else}
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  {#each COLUMNS as col (col.key)}
                    {@const indicator = sortIndicator(col.key)}
                    <th class={col.align} aria-sort={ariaSort(col.key)}>
                      <button type="button" class="sort-btn" class:active={indicator !== ''} onclick={() => toggleSort(col.key)}>
                        <span>{col.label}</span>
                        {#if indicator}<span class="sort-ind">{indicator}</span>{/if}
                      </button>
                    </th>
                  {/each}
                </tr>
              </thead>
              <tbody>
                {#each sortedFunctions as fn (fn.name)}
                  {@const dSelf = fn.b_self_pct - fn.a_self_pct}
                  {@const dIncl = fn.b_incl_pct - fn.a_incl_pct}
                  <tr>
                    <td class="name" title={fn.name}>{fn.name}</td>
                    <td class="num">{fn.a_self_pct.toFixed(1)}%</td>
                    <td class="num">{fn.b_self_pct.toFixed(1)}%</td>
                    <td class="num delta" class:pos={dSelf >= 0.05} class:neg={dSelf <= -0.05} class:muted={Math.abs(dSelf) < 0.05}>
                      {formatDelta(dSelf)}
                    </td>
                    <td class="num">{fn.a_incl_pct.toFixed(1)}%</td>
                    <td class="num">{fn.b_incl_pct.toFixed(1)}%</td>
                    <td class="num delta" class:pos={dIncl >= 0.05} class:neg={dIncl <= -0.05} class:muted={Math.abs(dIncl) < 0.05}>
                      {formatDelta(dIncl)}
                    </td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </section>
    {/if}
  </main>
</div>

<style>
  .page {
    --diff-pos: #c9362b;
    --diff-neg: #1e6bc4;
    display: flex;
    flex-direction: column;
    min-height: 100%;
  }
  @media (prefers-color-scheme: dark) {
    .page {
      --diff-pos: #ff7466;
      --diff-neg: #66b2ff;
    }
  }

  .core-filter {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 12px;
    color: var(--muted);
  }
  .core-filter select {
    font: inherit;
    color: var(--fg);
    background: var(--bg);
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 3px 8px;
  }

  .summary-bar {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 10px 14px;
    padding: 9px 18px;
    background: var(--panel);
    border-bottom: 1px solid var(--line);
    font-size: 13px;
  }
  .run-pill {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
  }
  .badge {
    font-size: 11px;
    font-weight: 700;
    padding: 2px 7px;
    border-radius: 5px;
    background: var(--line);
    color: var(--fg);
    white-space: nowrap;
  }
  .badge.target {
    background: color-mix(in srgb, var(--accent) 20%, var(--panel));
    color: var(--accent);
  }
  .run-name {
    max-width: 240px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .meta,
  .samples,
  .status {
    font-size: 12px;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .samples {
    padding: 1px 7px;
    border-radius: 999px;
    background: var(--bg);
    border: 1px solid var(--line);
  }
  .status {
    margin-left: auto;
  }

  .notice.bad {
    margin: 14px 18px 0;
    padding: 10px 14px;
    border-radius: 8px;
    border: 1px solid var(--danger);
    background: color-mix(in srgb, var(--danger) 14%, var(--panel));
    color: var(--danger);
    font-size: 13px;
  }

  main {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 16px;
    padding: 16px 18px 24px;
    min-width: 0;
  }
  .card {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 14px 16px;
    min-width: 0;
  }
  .section-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 10px 14px;
    margin-bottom: 8px;
  }
  .title-group {
    display: flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 8px 16px;
  }
  h2 {
    margin: 0;
    font-size: 15px;
  }
  .count {
    font-size: 11px;
    font-weight: 600;
    padding: 1px 7px;
    border-radius: 999px;
    background: var(--line);
    margin-left: 4px;
  }

  .legend {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 12px;
  }
  .swatch {
    width: 11px;
    height: 11px;
    border-radius: 3px;
    border: 1px solid #00000024;
  }
  .swatch.pos { background: #ef6351; }
  .swatch.neutral { background: #e6e1d8; }
  .swatch.neg { background: #5ca2eb; }

  .hover-line {
    margin: 0 0 10px;
    min-height: 1.45em;
    font: 12px/1.45 var(--mono);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .flame {
    width: 100%;
  }
  .flame.hidden {
    display: none;
  }
  /* d3-flame-graph draws its own labels in dark ink */
  .flame :global(.d3-flame-graph-label) {
    color: #1a1208;
  }
  .empty {
    margin: 8px 0;
    font-size: 13px;
  }

  .filter-input {
    width: 240px;
    max-width: 100%;
    padding: 5px 10px;
    font: inherit;
    font-size: 13px;
    color: var(--fg);
    background: var(--bg);
    border: 1px solid var(--line);
    border-radius: 6px;
  }
  .filter-input:focus {
    border-color: var(--accent);
    outline: none;
  }

  .table-wrap {
    overflow-x: auto;
  }
  table {
    width: 100%;
    border-collapse: collapse;
    font-size: 13px;
  }
  th {
    padding: 0;
    border-bottom: 1px solid var(--line);
    font-weight: 600;
    color: var(--muted);
  }
  th.left { text-align: left; }
  th.right { text-align: right; }
  .sort-btn {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    width: 100%;
    padding: 6px 8px;
    background: none;
    border: 0;
    color: inherit;
    font: inherit;
    justify-content: flex-end;
    white-space: nowrap;
  }
  th.left .sort-btn {
    justify-content: flex-start;
  }
  .sort-btn:hover,
  .sort-btn.active {
    color: var(--fg);
  }
  .sort-ind {
    color: var(--accent);
    font-size: 11px;
  }

  td {
    padding: 5px 8px;
    border-bottom: 1px solid var(--line);
    text-align: right;
  }
  td.name {
    text-align: left;
    font-family: var(--mono);
    font-size: 12px;
    max-width: 360px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .num {
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .delta.pos {
    color: var(--diff-pos);
    font-weight: 600;
  }
  .delta.neg {
    color: var(--diff-neg);
    font-weight: 600;
  }
  tbody tr:hover {
    background: color-mix(in srgb, var(--accent) 8%, transparent);
  }
</style>
