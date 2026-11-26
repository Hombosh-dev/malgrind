<script lang="ts">
  import { formatBytes } from '../lib/format';
  import { sortTasks, taskStateLabel, type SortDir, type TaskSortKey } from '../lib/series';
  import type { TasksData } from '../lib/types';

  interface Props {
    data: TasksData | null;
  }
  let { data }: Props = $props();

  interface ColumnSpec {
    key: TaskSortKey;
    label: string;
    align: 'left' | 'right';
    defaultDir: SortDir;
  }

  const COLUMNS: readonly ColumnSpec[] = [
    { key: 'name', label: 'Name', align: 'left', defaultDir: 'asc' },
    { key: 'cpu_pct', label: 'CPU %', align: 'right', defaultDir: 'desc' },
    { key: 'prio', label: 'Priority', align: 'right', defaultDir: 'desc' },
    { key: 'core', label: 'Core', align: 'right', defaultDir: 'asc' },
    { key: 'state', label: 'State', align: 'left', defaultDir: 'asc' },
    { key: 'stack_hwm', label: 'Min stack free', align: 'right', defaultDir: 'asc' },
  ];

  let sortKey = $state<TaskSortKey>('cpu_pct');
  let sortDir = $state<SortDir>('desc');

  const sortedTasks = $derived(data ? sortTasks(data.tasks, sortKey, sortDir) : []);
  const warnCount = $derived(sortedTasks.reduce((n, row) => n + (row.warn ? 1 : 0), 0));

  function toggleSort(col: ColumnSpec) {
    if (sortKey === col.key) {
      sortDir = sortDir === 'asc' ? 'desc' : 'asc';
    } else {
      sortKey = col.key;
      sortDir = col.defaultDir;
    }
  }

  function sortIndicator(key: TaskSortKey): string {
    if (sortKey !== key) return '';
    return sortDir === 'asc' ? '↑' : '↓';
  }

  function ariaSort(key: TaskSortKey): 'ascending' | 'descending' | 'none' {
    if (sortKey !== key) return 'none';
    return sortDir === 'asc' ? 'ascending' : 'descending';
  }

  function cpuBarWidth(pct: number): number {
    if (!Number.isFinite(pct) || pct <= 0) return 0;
    return Math.min(100, pct);
  }
</script>

{#if !data || data.tasks.length === 0}
  <p class="muted empty">No task data in this window.</p>
{:else}
  <div class="tasks-card">
    <div class="summary">
      <span class="muted">{sortedTasks.length} {sortedTasks.length === 1 ? 'task' : 'tasks'}</span>
      {#if warnCount > 0}
        <span class="warn-summary" role="status">⚠ {warnCount} low stack (&lt; 512 B)</span>
      {/if}
    </div>

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
          {#each sortedTasks as row (`${row.handle}:${row.name}`)}
            <tr class:warn={row.warn}>
              <td class="name" title={row.name}>{row.name}</td>
              <td class="cpu">
                <span class="bar" style:width="{cpuBarWidth(row.cpu_pct)}%"></span>
                <span class="num">{row.cpu_pct.toFixed(1)}%</span>
              </td>
              <td class="num">{row.prio}</td>
              <td class="num" class:muted={row.core === null}>{row.core === null ? 'any' : row.core}</td>
              <td class="state">{taskStateLabel(row.state)}</td>
              <td class="num stack" class:warn-cell={row.warn}>
                {#if row.warn}
                  <span class="warn-badge" title="Stack watermark below 512 B" aria-label="Low stack warning">⚠</span>
                {/if}
                <span>{formatBytes(row.stack_hwm)}</span>
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  </div>
{/if}

<style>
  .empty {
    padding: 16px 0;
  }
  .tasks-card {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 12px 16px;
  }
  .summary {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-bottom: 8px;
    font-size: 12.5px;
  }
  .warn-summary {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    padding: 2px 8px;
    border-radius: 999px;
    background: color-mix(in srgb, var(--danger) 14%, var(--panel));
    color: var(--danger);
    font-weight: 600;
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
    padding: 6px 8px;
    border-bottom: 1px solid var(--line);
    text-align: right;
  }
  td.name {
    text-align: left;
    font-family: var(--mono);
    font-size: 12px;
    max-width: 280px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  td.state {
    text-align: left;
    white-space: nowrap;
  }
  td.cpu {
    position: relative;
    min-width: 90px;
  }
  .bar {
    position: absolute;
    left: 0;
    top: 5px;
    bottom: 5px;
    background: color-mix(in srgb, var(--accent) 28%, transparent);
    border-radius: 2px;
  }
  .num {
    position: relative;
    font-variant-numeric: tabular-nums;
    white-space: nowrap;
  }
  tbody tr:hover {
    background: color-mix(in srgb, var(--accent) 8%, transparent);
  }
  tbody tr.warn {
    background: color-mix(in srgb, var(--danger) 9%, transparent);
  }
  tbody tr.warn:hover {
    background: color-mix(in srgb, var(--danger) 15%, transparent);
  }
  td.stack.warn-cell {
    color: var(--danger);
    font-weight: 600;
  }
  .warn-badge {
    margin-right: 4px;
    color: var(--danger);
  }
</style>
