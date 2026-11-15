<script lang="ts">
  export interface FunctionsData {
    total: number;
    functions: { name: string; self: number; total: number }[];
    callers: { name: string; count: number }[];
    callees: { name: string; count: number }[];
  }

  interface Props {
    data: FunctionsData | null;
    focus: string;
    onFocus(name: string): void;
  }
  let { data, focus, onFocus }: Props = $props();

  const percent = (n: number) => (data && data.total > 0 ? (100 * n) / data.total : 0);
  const shown = (n: number) => `${percent(n).toFixed(1)} %`;
</script>

{#if !data || data.total === 0}
  <p class="muted">No samples in this window.</p>
{:else}
  <div class="layout">
    <table>
      <thead><tr><th class="name">Function</th><th>Self</th><th>Inclusive</th></tr></thead>
      <tbody>
        {#each data.functions as f (f.name)}
          <tr class:on={f.name === focus} onclick={() => onFocus(f.name)}>
            <td class="name" title={f.name}>{f.name}</td>
            <td><span class="bar" style:width="{percent(f.self)}%"></span><span class="num">{shown(f.self)}</span></td>
            <td class="num">{shown(f.total)}</td>
          </tr>
        {/each}
      </tbody>
    </table>

    <aside>
      {#if focus}
        <h3 title={focus}>{focus}</h3>
        <h4>Called by</h4>
        {#each data.callers as c (c.name)}
          <button class="rel" onclick={() => onFocus(c.name)}><span>{c.name}</span><span class="muted">{shown(c.count)}</span></button>
        {:else}<p class="muted">nobody (it is a root, or only appears at the bottom)</p>{/each}
        <h4>Calls</h4>
        {#each data.callees as c (c.name)}
          <button class="rel" onclick={() => onFocus(c.name)}><span>{c.name}</span><span class="muted">{shown(c.count)}</span></button>
        {:else}<p class="muted">nothing: it only uses the CPU itself</p>{/each}
      {:else}
        <p class="muted">Click a function to see who calls it and what it calls.</p>
      {/if}
    </aside>
  </div>
{/if}

<style>
  .layout { display: grid; grid-template-columns: minmax(0, 1fr) 320px; gap: 18px; align-items: start; }
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th { text-align: right; font-weight: 600; color: var(--muted); padding: 4px 8px; border-bottom: 1px solid var(--line); }
  th.name, td.name { text-align: left; }
  td { padding: 4px 8px; text-align: right; position: relative; border-bottom: 1px solid var(--line); }
  td.name { font-family: var(--mono); font-size: 12px; max-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  tr { cursor: pointer; }
  tr:hover, tr.on { background: color-mix(in srgb, var(--accent) 12%, transparent); }
  .bar { position: absolute; left: 0; top: 4px; bottom: 4px; background: color-mix(in srgb, var(--accent) 35%, transparent); border-radius: 2px; }
  .num { position: relative; font-variant-numeric: tabular-nums; }
  aside { position: sticky; top: 0; padding: 12px 14px; background: var(--panel); border: 1px solid var(--line); border-radius: 10px; }
  h3 { margin: 0 0 8px; font: 600 13px var(--mono); overflow-wrap: anywhere; }
  h4 { margin: 14px 0 4px; font-size: 12px; text-transform: uppercase; letter-spacing: 0.4px; color: var(--muted); }
  .rel { display: flex; justify-content: space-between; gap: 10px; width: 100%; padding: 3px 4px; background: none; border: 0; text-align: left; font: 12px var(--mono); }
  .rel:hover { background: color-mix(in srgb, var(--accent) 12%, transparent); }
</style>
