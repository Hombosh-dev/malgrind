<script lang="ts">
  import { tick } from 'svelte';

  interface Props {
    lines: string[];
    busy: boolean;
  }
  let { lines, busy }: Props = $props();
  let box: HTMLElement;

  // keep the newest line in view
  $effect(() => {
    lines.length;
    tick().then(() => (box.scrollTop = box.scrollHeight));
  });
</script>

{#if busy}<div class="bar"><i></i></div>{/if}
<pre bind:this={box}>{lines.join('\n')}</pre>

<style>
  pre { margin: 0; height: 240px; overflow: auto; padding: 10px 12px; background: var(--bg); border: 1px solid var(--line); border-radius: 8px; font: 11px/1.5 var(--mono); }
  .bar { height: 6px; border-radius: 3px; background: var(--line); overflow: hidden; margin-bottom: 10px; }
  .bar i { display: block; height: 100%; width: 35%; background: linear-gradient(90deg, #ff9a3c, #ff5a1f); animation: slide 1.2s ease-in-out infinite; }
  @keyframes slide { from { margin-left: -35%; } to { margin-left: 100%; } }
</style>
