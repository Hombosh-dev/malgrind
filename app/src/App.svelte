<script lang="ts">
  import { onMount } from 'svelte';
  import { startSidecar, type Sidecar } from './lib/sidecar';
  import { installMenu } from './lib/menu';
  import type { Hello, RunMeta } from './lib/types';
  import RemoveAgent from './components/RemoveAgent.svelte';
  import NewRun from './screens/NewRun.svelte';
  import Run from './screens/Run.svelte';
  import Start from './screens/Start.svelte';

  let sidecar = $state<Sidecar | null>(null);
  let hello = $state<Hello | null>(null);
  let fatal = $state('');
  let screen = $state<'start' | 'newrun' | 'run'>('start');
  let run = $state<RunMeta | null>(null);
  let live = $state(false); // the run on screen is being recorded
  let removingPort = $state<string | null>(null);

  // `recording`: attached or built just now; otherwise opened from the history
  function show(r: RunMeta, recording: boolean) {
    run = r;
    live = recording;
    screen = 'run';
  }

  onMount(async () => {
    try {
      sidecar = await startSidecar();
      hello = await sidecar.call<Hello>('hello');
      await installMenu({
        newRun: () => (screen = 'newrun'),
        stop: () => screen === 'run' && sidecar?.call('stop').finally(() => (screen = 'start')),
        removeAgent: () => (removingPort = ''),
      });
    } catch (e) {
      fatal = String(e instanceof Error ? e.message : e);
    }
  });
</script>

{#if fatal}
  <main class="fatal">
    <h1>malgrind could not start its engine</h1>
    <p class="muted">{fatal}</p>
  </main>
{:else if !sidecar || !hello}
  <main class="fatal"><p class="muted">Starting…</p></main>
{:else if screen === 'start'}
  <Start
    {sidecar}
    {hello}
    onNewRun={() => (screen = 'newrun')}
    onRun={show}
    onRemoveAgent={(port) => (removingPort = port)}
  />
{:else if screen === 'newrun'}
  <NewRun {sidecar} {hello} onCancel={() => (screen = 'start')} onRun={show} />
{:else}
  {#key run?.id}
    <Run {sidecar} run={run!} {live} onClose={() => (screen = 'start')} onRemoveAgent={(port) => (removingPort = port)} />
  {/key}
{/if}

{#if sidecar && removingPort !== null}
  <RemoveAgent
    {sidecar}
    port={removingPort}
    onCancel={() => (removingPort = null)}
    onDone={() => ((removingPort = null), (screen = 'start'))}
  />
{/if}

<style>
  .fatal { padding: 48px 24px; text-align: center; }
</style>
