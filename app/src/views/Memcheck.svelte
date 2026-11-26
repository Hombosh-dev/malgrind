<script lang="ts">
  import { untrack } from 'svelte';
  import { formatBytes } from '../lib/format';
  import {
    clampLeakAgeS,
    errorLabel,
    formatAgeMs,
    leakFrame,
    sortLeaks,
    type LeakSortKey,
    type SortDir,
  } from '../lib/memcheck';
  import { formatClockMs } from '../lib/series';
  import type { MemcheckData } from '../lib/types';

  interface Props {
    data: MemcheckData | null;
    leakAgeS: number;
    onLeakAgeChange(age: number): void;
  }
  let { data, leakAgeS = 30, onLeakAgeChange }: Props = $props();

  interface ColumnSpec {
    key: LeakSortKey;
    label: string;
    align: 'left' | 'right';
    defaultDir: SortDir;
  }

  const COLUMNS: readonly ColumnSpec[] = [
    { key: 'kind', label: 'Status', align: 'left', defaultDir: 'asc' },
    { key: 'bytes', label: 'Bytes', align: 'right', defaultDir: 'desc' },
    { key: 'count', label: 'Count', align: 'right', defaultDir: 'desc' },
    { key: 'oldest_ms', label: 'First seen', align: 'right', defaultDir: 'asc' },
    { key: 'frame', label: 'Location', align: 'left', defaultDir: 'asc' },
  ];

  let sortKey = $state<LeakSortKey>('kind');
  let sortDir = $state<SortDir>('asc');
  let expandedStacks = $state<Record<string, boolean>>({});
  let ageInput = $state(untrack(() => String(clampLeakAgeS(leakAgeS))));
  let ageFocused = false;

  $effect(() => {
    const normalized = clampLeakAgeS(leakAgeS);
    if (!ageFocused) ageInput = String(normalized);
  });

  const sortedLeaks = $derived(data ? sortLeaks(data.leaks, sortKey, sortDir) : []);
  const leakRows = $derived.by(() => {
    const seen = new Map<string, number>();
    return sortedLeaks.map((leak) => {
      const base = leak.stack.join('\0');
      const occ = seen.get(base) ?? 0;
      seen.set(base, occ + 1);
      const rowKey = occ === 0 ? base : `${base}#${occ}`;
      return { leak, rowKey };
    });
  });

  const definiteCount = $derived(
    data ? data.leaks.reduce((n, leak) => n + (leak.kind === 'definitely' ? 1 : 0), 0) : 0,
  );
  const possibleCount = $derived(
    data ? data.leaks.reduce((n, leak) => n + (leak.kind === 'possibly' ? 1 : 0), 0) : 0,
  );

  function toggleSort(col: ColumnSpec) {
    if (sortKey === col.key) {
      sortDir = sortDir === 'asc' ? 'desc' : 'asc';
    } else {
      sortKey = col.key;
      sortDir = col.defaultDir;
    }
  }

  function sortIndicator(key: LeakSortKey): string {
    if (sortKey !== key) return '';
    return sortDir === 'asc' ? '↑' : '↓';
  }

  function ariaSort(key: LeakSortKey): 'ascending' | 'descending' | 'none' {
    if (sortKey !== key) return 'none';
    return sortDir === 'asc' ? 'ascending' : 'descending';
  }

  function toggleExpand(rowKey: string) {
    expandedStacks[rowKey] = !expandedStacks[rowKey];
  }

  function handleAgeInput(value: string) {
    ageInput = value;
    const trimmed = value.trim();
    if (trimmed === '') return;
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) return;
    const clamped = clampLeakAgeS(parsed);
    if (clamped !== leakAgeS) onLeakAgeChange(clamped);
  }

  function handleAgeBlur() {
    ageFocused = false;
    const trimmed = ageInput.trim();
    if (trimmed === '') {
      ageInput = String(clampLeakAgeS(leakAgeS));
      return;
    }
    const parsed = Number(trimmed);
    const clamped = Number.isFinite(parsed) ? clampLeakAgeS(parsed) : clampLeakAgeS(leakAgeS);
    ageInput = String(clamped);
    if (clamped !== leakAgeS) onLeakAgeChange(clamped);
  }

  function traceFrames(stack: readonly string[]): string[] {
    return [...stack].reverse();
  }
</script>

{#if !data}
  <p class="muted empty">No memory check data in this window.</p>
{:else}
  <div class="memcheck">
    <div class="summary-bar">
      <div class="metrics">
        <div class="metric">
          <span class="metric-label">Errors</span>
          <strong class="metric-val" class:bad={data.errors.length > 0}>{data.errors.length}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Definite leaks</span>
          <strong class="metric-val" class:bad={definiteCount > 0}>{definiteCount}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Possible leaks</span>
          <strong class="metric-val">{possibleCount}</strong>
        </div>
        <div class="metric">
          <span class="metric-label">Live heap</span>
          <strong class="metric-val">
            {data.live_allocs} {data.live_allocs === 1 ? 'alloc' : 'allocs'} · {formatBytes(data.live_bytes)}
          </strong>
        </div>
      </div>

      <label class="age-control">
        <span>Leak age (s)</span>
        <input
          type="number"
          min="0"
          step="1"
          inputmode="numeric"
          value={ageInput}
          onfocus={() => (ageFocused = true)}
          oninput={(e) => handleAgeInput(e.currentTarget.value)}
          onblur={handleAgeBlur}
        />
      </label>
    </div>

    {#if data.dropped > 0}
      <div class="dropped-banner" role="status">
        ⚠ {data.dropped} {data.dropped === 1 ? 'memory event was' : 'memory events were'} dropped on the target; leak and error counts may be incomplete.
      </div>
    {/if}

    <section class="panel">
      <div class="section-head">
        <h3>Errors</h3>
        <span class="muted count-label">{data.errors.length}</span>
      </div>

      {#if data.errors.length === 0}
        <p class="muted section-empty">No memory or stack errors recorded.</p>
      {:else}
        <div class="errors-list">
          {#each data.errors as err, i (`${i}:${err.t_ms}:${err.code}`)}
            {@const frames = traceFrames(err.stack)}
            <article class="error-card">
              <div class="error-head">
                <strong class="error-title">{errorLabel(err.code, err.kind)}</strong>
                <span class="error-meta">
                  {#if err.name}<span class="task-pill">{err.name}</span>{/if}
                  {#if err.kind === 'alloc_failed' && err.arg > 0}
                    <span class="arg-pill">Requested {formatBytes(err.arg)}</span>
                  {/if}
                  <span class="clock-pill">{formatClockMs(err.t_ms)}</span>
                </span>
              </div>
              {#if frames.length > 0}
                <ol class="stack-trace">
                  {#each frames as frame, idx (`${idx}:${frame}`)}
                    <li><span class="frame-idx">#{idx}</span><code>{frame}</code></li>
                  {/each}
                </ol>
              {:else}
                <p class="muted no-stack">No stack trace captured.</p>
              {/if}
            </article>
          {/each}
        </div>
      {/if}
    </section>

    <section class="panel">
      <div class="section-head">
        <h3>Leaks</h3>
        <span class="muted count-label">
          {leakRows.length} {leakRows.length === 1 ? 'call site' : 'call sites'}
        </span>
      </div>

      {#if leakRows.length === 0}
        <p class="muted section-empty">No live allocations remaining at this point.</p>
      {:else}
        <div class="table-wrap">
          <table>
            <thead>
              <tr>
                {#each COLUMNS as col (col.key)}
                  {@const indicator = sortIndicator(col.key)}
                  <th class={col.align} aria-sort={ariaSort(col.key)}>
                    <button
                      type="button"
                      class="sort-btn"
                      class:active={indicator !== ''}
                      onclick={() => toggleSort(col)}
                    >
                      <span>{col.label}</span>
                      {#if indicator}<span class="sort-ind">{indicator}</span>{/if}
                    </button>
                  </th>
                {/each}
              </tr>
            </thead>
            <tbody>
              {#each leakRows as { leak, rowKey } (rowKey)}
                {@const isExpanded = !!expandedStacks[rowKey]}
                {@const frame = leakFrame(leak.stack)}
                {@const frames = traceFrames(leak.stack)}
                <tr
                  class="leak-row"
                  class:definite-row={leak.kind === 'definitely'}
                  class:expanded={isExpanded}
                  onclick={() => toggleExpand(rowKey)}
                >
                  <td class="status-cell">
                    <span class="pill" class:definite={leak.kind === 'definitely'} class:possible={leak.kind === 'possibly'}>
                      {leak.kind === 'definitely' ? 'Definitely' : 'Possibly'}
                    </span>
                  </td>
                  <td class="num">{formatBytes(leak.bytes)}</td>
                  <td class="num">{leak.count}</td>
                  <td class="num" title={`First allocated at ${formatClockMs(leak.oldest_ms)} (${leak.oldest_ms} ms)`}>
                    {formatAgeMs(leak.oldest_ms)}
                  </td>
                  <td class="loc-cell">
                    <button
                      type="button"
                      class="loc-btn"
                      aria-expanded={isExpanded}
                      title={frame || 'No stack trace captured'}
                      onclick={(e) => {
                        e.stopPropagation();
                        toggleExpand(rowKey);
                      }}
                    >
                      <span class="chev" aria-hidden="true">{isExpanded ? '▾' : '▸'}</span>
                      <span class="frame-name" class:muted={!frame}>{frame || '(unknown stack)'}</span>
                      {#if leak.stack.length > 1}
                        <span class="depth-badge">{leak.stack.length} frames</span>
                      {/if}
                    </button>
                  </td>
                </tr>
                {#if isExpanded}
                  <tr class="trace-row">
                    <td colspan={COLUMNS.length}>
                      {#if frames.length > 0}
                        <ol class="stack-trace inline-trace">
                          {#each frames as traceFrame, idx (`${idx}:${traceFrame}`)}
                            <li><span class="frame-idx">#{idx}</span><code>{traceFrame}</code></li>
                          {/each}
                        </ol>
                      {:else}
                        <p class="muted no-stack">No stack frames captured for this allocation.</p>
                      {/if}
                    </td>
                  </tr>
                {/if}
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </section>
  </div>
{/if}

<style>
  .memcheck {
    --leak-possible: #b45309;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  @media (prefers-color-scheme: dark) {
    .memcheck {
      --leak-possible: #f59e0b;
    }
  }
  .empty {
    padding: 16px 0;
  }
  .summary-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 12px 18px;
    padding: 12px 16px;
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 10px;
  }
  .metrics {
    display: flex;
    flex-wrap: wrap;
    gap: 10px 22px;
  }
  .metric {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .metric-label {
    font-size: 11.5px;
    color: var(--muted);
  }
  .metric-val {
    font-size: 14px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .metric-val.bad {
    color: var(--danger);
  }
  .age-control {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    font-size: 12.5px;
    color: var(--muted);
  }
  .age-control input {
    width: 74px;
    padding: 5px 8px;
    font: 13px var(--mono);
    color: var(--fg);
    background: var(--bg);
    border: 1px solid var(--line);
    border-radius: 6px;
    text-align: right;
  }
  .age-control input:focus {
    outline: none;
    border-color: var(--accent);
  }
  .dropped-banner {
    padding: 9px 14px;
    border-radius: 8px;
    border: 1px solid color-mix(in srgb, var(--leak-possible) 45%, transparent);
    background: color-mix(in srgb, var(--leak-possible) 12%, var(--panel));
    color: var(--leak-possible);
    font-size: 13px;
    font-weight: 500;
  }
  .panel {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 12px 16px;
    min-width: 0;
  }
  .section-head {
    display: flex;
    align-items: baseline;
    gap: 8px;
    margin-bottom: 10px;
  }
  h3 {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
  }
  .count-label {
    font-size: 12px;
    font-variant-numeric: tabular-nums;
  }
  .section-empty {
    margin: 4px 0;
    font-size: 13px;
  }
  .errors-list {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .error-card {
    padding: 10px 12px;
    border-radius: 8px;
    border: 1px solid color-mix(in srgb, var(--danger) 40%, var(--line));
    border-left: 3px solid var(--danger);
    background: color-mix(in srgb, var(--danger) 6%, var(--bg));
  }
  .error-head {
    display: flex;
    align-items: center;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 6px 12px;
    margin-bottom: 6px;
  }
  .error-title {
    color: var(--danger);
    font-size: 13.5px;
    font-weight: 600;
  }
  .error-meta {
    display: inline-flex;
    align-items: center;
    flex-wrap: wrap;
    gap: 6px;
    font-size: 12px;
  }
  .task-pill,
  .arg-pill,
  .clock-pill {
    padding: 1px 7px;
    border-radius: 999px;
    font-family: var(--mono);
    font-size: 11.5px;
    background: var(--panel);
    border: 1px solid var(--line);
  }
  .arg-pill {
    color: var(--danger);
    border-color: color-mix(in srgb, var(--danger) 35%, var(--line));
  }
  .clock-pill {
    color: var(--muted);
  }
  .stack-trace {
    margin: 0;
    padding: 0;
    list-style: none;
    display: flex;
    flex-direction: column;
    gap: 3px;
    font-family: var(--mono);
    font-size: 12px;
  }
  .stack-trace li {
    display: flex;
    align-items: baseline;
    gap: 8px;
    min-width: 0;
  }
  .stack-trace code {
    overflow-wrap: anywhere;
  }
  .frame-idx {
    color: var(--muted);
    font-size: 11px;
    flex-shrink: 0;
    min-width: 24px;
  }
  .no-stack {
    margin: 0;
    font-size: 12px;
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
  th.left {
    text-align: left;
  }
  th.right {
    text-align: right;
  }
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
    padding: 7px 8px;
    border-bottom: 1px solid var(--line);
    text-align: right;
  }
  td.status-cell,
  td.loc-cell {
    text-align: left;
  }
  .num {
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  .leak-row {
    cursor: pointer;
  }
  .leak-row:hover {
    background: color-mix(in srgb, var(--accent) 8%, transparent);
  }
  .leak-row.definite-row {
    background: color-mix(in srgb, var(--danger) 5%, transparent);
  }
  .leak-row.definite-row:hover {
    background: color-mix(in srgb, var(--danger) 10%, transparent);
  }
  .leak-row.expanded td {
    border-bottom-color: transparent;
  }
  .pill {
    display: inline-block;
    padding: 2px 8px;
    border-radius: 999px;
    font-size: 11.5px;
    font-weight: 600;
    white-space: nowrap;
  }
  .pill.definite {
    color: var(--danger);
    background: color-mix(in srgb, var(--danger) 14%, var(--panel));
    border: 1px solid color-mix(in srgb, var(--danger) 35%, transparent);
  }
  .pill.possible {
    color: var(--leak-possible);
    background: color-mix(in srgb, var(--leak-possible) 12%, var(--panel));
    border: 1px solid color-mix(in srgb, var(--leak-possible) 30%, transparent);
  }
  .loc-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    width: 100%;
    padding: 0;
    background: none;
    border: 0;
    text-align: left;
    font: inherit;
    min-width: 0;
  }
  .chev {
    color: var(--muted);
    font-size: 11px;
    flex-shrink: 0;
  }
  .frame-name {
    font-family: var(--mono);
    font-size: 12px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .depth-badge {
    margin-left: auto;
    padding: 1px 6px;
    border-radius: 999px;
    background: var(--bg);
    border: 1px solid var(--line);
    color: var(--muted);
    font-size: 11px;
    white-space: nowrap;
    flex-shrink: 0;
  }
  .trace-row td {
    text-align: left;
    padding: 8px 12px 10px 32px;
    background: var(--bg);
  }
</style>
