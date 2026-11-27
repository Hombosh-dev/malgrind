<script lang="ts">
  import type { RunMeta } from '../lib/types';

  interface Props {
    selected: RunMeta[];
    onCompare(a: RunMeta, b: RunMeta): void;
    onClear(): void;
  }
  let { selected, onCompare, onClear }: Props = $props();
</script>

<div class="compare-bar" class:ready={selected.length === 2}>
  {#if selected.length === 0}
    <span class="muted">Select any 2 runs to compare</span>
  {:else if selected.length === 1}
    <span class="summary">
      <span class="slot">A</span>
      <b class="label">{selected[0].name || selected[0].id}</b>
      <span class="muted">- select 1 more run as B</span>
    </span>
    <button class="btn ghost small" type="button" onclick={onClear}>Clear</button>
  {:else}
    <span class="summary">
      <span class="slot">A</span>
      <b class="label">{selected[0].name || selected[0].id}</b>
      <span class="muted">vs</span>
      <span class="slot target">B</span>
      <b class="label">{selected[1].name || selected[1].id}</b>
    </span>
    <div class="actions">
      <button class="btn ghost small" type="button" onclick={onClear}>Clear</button>
      <button class="btn small" type="button" onclick={() => onCompare(selected[0], selected[1])}>Compare</button>
    </div>
  {/if}
</div>

<style>
  .compare-bar {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 8px 14px;
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 8px;
    font-size: 13px;
  }
  .compare-bar.ready {
    border-color: color-mix(in srgb, var(--accent) 45%, var(--line));
  }
  .summary {
    display: flex;
    align-items: center;
    gap: 8px;
    min-width: 0;
    flex-wrap: wrap;
  }
  .label {
    min-width: 0;
    max-width: 240px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .slot {
    display: inline-grid;
    place-items: center;
    min-width: 20px;
    height: 20px;
    padding: 0 5px;
    border-radius: 5px;
    font-size: 11px;
    font-weight: 700;
    background: var(--line);
    color: var(--fg);
  }
  .slot.target {
    background: color-mix(in srgb, var(--accent) 22%, var(--panel));
    color: var(--accent);
  }
  .actions {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }
</style>
