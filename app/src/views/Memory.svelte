<script lang="ts">
  import Series from '../components/Series.svelte';
  import { formatBytes } from '../lib/format';
  import { regionTitle } from '../lib/series';
  import type { HeapRegionSeries, MemoryData, SeriesLine } from '../lib/types';

  interface Props {
    data: MemoryData | null;
  }
  let { data }: Props = $props();

  function chartLines(region: HeapRegionSeries): SeriesLine[] {
    return [
      { label: 'Free', color: 'var(--mem-free)', points: region.series.map((p) => [p[0], p[1]]) },
      { label: 'Min free', color: 'var(--mem-min)', points: region.series.map((p) => [p[0], p[2]]) },
      { label: 'Largest block', color: 'var(--mem-largest)', points: region.series.map((p) => [p[0], p[3]]) },
    ];
  }

  function maxTotal(region: HeapRegionSeries): number | null {
    let best = 0;
    for (const pt of region.series) {
      if (Number.isFinite(pt[4]) && pt[4] > best) best = pt[4];
    }
    return best > 0 ? best : null;
  }
</script>

{#if !data || data.regions.length === 0}
  <p class="muted empty">No heap memory data in this window.</p>
{:else}
  <div class="memory">
    {#each data.regions as region (region.region)}
      {@const latest = region.series.length > 0 ? region.series[region.series.length - 1] : null}
      <section class="card">
        <div class="card-head">
          <h3>{regionTitle(region.name, region.region)}</h3>
          <div class="stats">
            <span><span class="muted">Free</span> <b>{latest ? formatBytes(latest[1]) : '-'}</b></span>
            <span><span class="muted">Min free</span> <b>{latest ? formatBytes(latest[2]) : '-'}</b></span>
            <span><span class="muted">Largest block</span> <b>{latest ? formatBytes(latest[3]) : '-'}</b></span>
            <span><span class="muted">Total</span> <b>{latest ? formatBytes(latest[4]) : '-'}</b></span>
          </div>
        </div>
        <Series
          height={200}
          series={chartLines(region)}
          referenceY={maxTotal(region)}
          referenceLabel="Total"
          formatY={formatBytes}
          emptyText="No samples recorded for this region in the current window."
        />
      </section>
    {/each}
  </div>
{/if}

<style>
  .memory {
    --mem-free: #1f9d55;
    --mem-min: #e6681c;
    --mem-largest: #1e6bc4;
    display: flex;
    flex-direction: column;
    gap: 16px;
  }
  @media (prefers-color-scheme: dark) {
    .memory {
      --mem-free: #37d67a;
      --mem-min: #ff9248;
      --mem-largest: #66b2ff;
    }
  }
  .empty {
    padding: 16px 0;
  }
  .card {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 10px;
    padding: 14px 16px;
    min-width: 0;
  }
  .card-head {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    flex-wrap: wrap;
    gap: 8px 18px;
    margin-bottom: 10px;
  }
  h3 {
    margin: 0;
    font-size: 15px;
    font-weight: 600;
  }
  .stats {
    display: flex;
    flex-wrap: wrap;
    gap: 6px 16px;
    font-size: 12.5px;
    font-variant-numeric: tabular-nums;
  }
  .stats b {
    font-family: var(--mono);
    font-weight: 600;
  }
</style>
