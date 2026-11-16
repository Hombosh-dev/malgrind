<script lang="ts">
  import { onMount } from 'svelte';
  import BuildLog from '../components/BuildLog.svelte';
  import TopBar from '../components/TopBar.svelte';
  import { defaultRunName } from '../lib/format';
  import { boardTitle } from '../lib/projects';
  import type { Sidecar } from '../lib/sidecar';
  import type { BoardPort, BoardsInfo, Method, Probe, ProjectMeta, RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    project: ProjectMeta;
    onBack(): void;
    onRun(run: RunMeta, recording: boolean): void;
  }
  let { sidecar, project, onBack, onRun }: Props = $props();

  type Tile = BoardPort & { probe: Probe | null; error: string; chip: string; identifying: boolean };

  let step = $state<'board' | 'mode' | 'process'>('board');
  let tiles = $state<Tile[]>([]);
  let probes = $state<BoardsInfo['probes']>([]);
  let scanned = $state(false);
  let chosen = $state<Tile | null>(null);
  let lines = $state<string[]>([]);
  let building = $state(false);
  let error = $state('');
  let scanId = 0;

  // One tile per serial port, shown at once; each port is then listened to in turn (2 s, read-only) to see whether the agent runs.
  async function scan() {
    const mine = ++scanId;
    scanned = false;
    error = '';
    try {
      const info = await sidecar.call<BoardsInfo>('boards');
      if (mine !== scanId) return;
      probes = info.probes;
      tiles = info.ports.map((p) => ({ ...p, probe: null, error: '', chip: '', identifying: false }));
      scanned = true;
      for (const tile of tiles) {
        try {
          const probe = await sidecar.call<Probe>('probe', { port: tile.port, project: project.id });
          if (mine === scanId) tile.probe = probe;
        } catch (e) {
          if (mine === scanId) tile.error = String(e instanceof Error ? e.message : e);
        }
      }
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
      scanned = true;
    }
  }
  onMount(scan);

  $effect(() => sidecar.on('build', (e) => {
    lines.push(e.line);
    if (lines.length > 200) lines.splice(0, lines.length - 200);
  }));

  async function identify(tile: Tile) {
    tile.identifying = true;
    tile.chip = '';
    try {
      tile.chip = (await sidecar.call<{ chip: string }>('chip_id', { port: tile.port })).chip;
    } catch (e) {
      tile.chip = String(e instanceof Error ? e.message : e);
    }
    tile.identifying = false;
  }

  const labels: Record<Method['id'], { title: string; text: string }> = {
    attach: { title: 'Attach', text: 'Watch the firmware that is already running on the board. Needs the malgrind agent in it. Nothing is flashed.' },
    new_run: {
      title: 'Build & flash',
      text: 'Builds your project with the agent and flashes it. The firmware on the board is backed up first and can be restored.'
    },
    no_flash: { title: 'Debug probe', text: 'Profile without touching the firmware, through a JTAG/SWD probe.' }
  };
  const order: Method['id'][] = ['attach', 'new_run', 'no_flash'];

  const methods = $derived(
    order.map((id) => chosen?.probe?.methods.find((m) => m.id === id) ?? { id, available: false, why: 'unknown' })
  );

  function choose(tile: Tile) {
    chosen = tile;
    error = '';
    step = 'mode';
  }

  async function pick(method: Method) {
    if (!chosen || !method.available) return;
    error = '';
    const name = defaultRunName(project.name);
    if (method.id === 'attach') {
      try {
        const { run } = await sidecar.call<{ run: RunMeta }>('attach', { port: chosen.port, name, project: project.id });
        onRun(run, true);
      } catch (e) {
        error = String(e instanceof Error ? e.message : e);
      }
    } else if (method.id === 'new_run') {
      step = 'process';
      building = true;
      lines = [];
      try {
        const { run } = await sidecar.call<{ run: RunMeta }>('new_run', { project: project.id, port: chosen.port, name });
        onRun(run, true);
      } catch (e) {
        error = String(e instanceof Error ? e.message : e);
      }
      building = false;
    }
  }
</script>

<div class="page">
  <TopBar
    title={project.name}
    onBack={step === 'board' ? onBack : step === 'mode' ? () => (step = 'board') : building ? undefined : () => (step = 'mode')}
    backLabel="Back"
  >
    <ol class="steps">
      {#each ['Board', 'How', 'Run'] as label, i}
        <li class:on={['board', 'mode', 'process'].indexOf(step) === i} class:done={['board', 'mode', 'process'].indexOf(step) > i}>{label}</li>
      {/each}
    </ol>
  </TopBar>

  {#if step === 'board'}
    <section>
      <div class="intro">
        <div>
          <h1>Which board?</h1>
          <p class="muted">Boards that are plugged in. Nothing is sent to a board while you look.</p>
        </div>
        <button class="btn ghost small" onclick={scan}>Rescan</button>
      </div>

      {#if scanned && tiles.length === 0 && probes.length === 0}
        <p class="empty">No board found. Connect it with a USB cable, then press Rescan.</p>
      {/if}

      <div class="tiles">
        {#each tiles as tile (tile.port)}
          <div class="tile" class:wait={!tile.probe && !tile.error}>
            <button class="pick" disabled={!tile.probe} onclick={() => choose(tile)} aria-label={`Use ${boardTitle(tile)} on ${tile.port}`}>
              <span class="chipicon" aria-hidden="true">▦</span>
              <span class="t"><b>{boardTitle(tile)}</b><span class="muted">{tile.port}</span></span>
              <span class="status">
                {#if tile.error}<span class="bad">{tile.error}</span>
                {:else if !tile.probe}Listening for the agent…
                {:else if tile.probe.busy}<span class="bad">Busy: another program uses this port</span>
                {:else if tile.probe.agent}<span class="good">● malgrind agent running · {tile.probe.arch} · {tile.probe.ncores} cores · {tile.probe.cpu_mhz} MHz</span>
                {:else}No malgrind agent detected{/if}
              </span>
            </button>
            <div class="foot">
              {#if tile.chip}<span class="muted chip">{tile.chip}</span>{/if}
              <button class="link" onclick={() => identify(tile)} disabled={tile.identifying || !tile.probe || tile.probe.busy} title="Asks the chip for its name. This resets the board.">
                {tile.identifying ? 'Asking the chip…' : 'Identify chip (resets the board)'}
              </button>
            </div>
          </div>
        {/each}
        {#each probes as probe}
          <div class="tile off" aria-disabled="true">
            <div class="pick">
              <span class="chipicon" aria-hidden="true">⌁</span>
              <span class="t"><b>{probe.name}</b><span class="muted">debug probe</span></span>
              <span class="status">Found. Profiling through a probe is in development.</span>
            </div>
          </div>
        {/each}
      </div>
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {:else if step === 'mode' && chosen}
    <section>
      <h1>How do you want to look at it?</h1>
      <p class="muted">{boardTitle(chosen)} · {chosen.port}</p>
      <div class="cards">
        {#each methods as m (m.id)}
          <button class="card" class:off={!m.available} disabled={!m.available} onclick={() => pick(m)}>
            <b>{labels[m.id].title}</b>
            {#if m.id === 'no_flash'}<span class="soon">in development</span>{/if}
            <span class="muted text">{labels[m.id].text}</span>
            {#if !m.available && m.id !== 'no_flash'}<span class="why">{m.why}</span>{/if}
          </button>
        {/each}
      </div>
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {:else}
    <section>
      <h1>{building ? 'Backing up the board, building with the agent, flashing…' : 'It did not finish'}</h1>
      <p class="muted">The board's flash is saved first (about a minute the first time), so "Remove agent" can put your firmware back exactly.</p>
      <BuildLog {lines} busy={building} />
      {#if error}<p class="error" role="alert">{error}</p>{/if}
    </section>
  {/if}
</div>

<style>
  .page { min-height: 100%; }
  section { max-width: 900px; margin: 0 auto; padding: 24px; }
  h1 { margin: 0 0 4px; font-size: 22px; }
  .intro { display: flex; justify-content: space-between; align-items: flex-start; gap: 16px; }
  .empty { padding: 28px; text-align: center; border: 1px dashed var(--line); border-radius: 12px; color: var(--muted); }
  .steps { display: flex; gap: 6px; list-style: none; margin: 0 8px 0 0; padding: 0; }
  .steps li { padding: 2px 10px; border-radius: 999px; background: var(--line); color: var(--muted); font-size: 12px; }
  .steps li.done { color: var(--live); }
  .steps li.on { background: var(--accent); color: var(--accent-fg); font-weight: 600; }
  .tiles { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 16px; margin-top: 18px; }
  .tile { display: flex; flex-direction: column; border: 1.5px solid var(--line); border-radius: 16px; background: var(--panel); overflow: hidden; }
  .tile:not(.off):hover { border-color: var(--accent); box-shadow: 0 6px 24px #ff5a1f22; }
  .tile.off { opacity: 0.55; }
  .tile.wait { opacity: 0.8; }
  .pick { display: flex; flex-direction: column; gap: 8px; padding: 20px; text-align: left; background: none; border: 0; flex: 1; }
  button.pick:disabled { cursor: default; }
  .chipicon { font-size: 34px; color: var(--accent); line-height: 1; }
  .t { display: flex; flex-direction: column; }
  .t b { font-size: 16px; }
  .status { font-size: 12.5px; color: var(--muted); }
  .good { color: var(--live); }
  .bad { color: var(--danger); }
  .foot { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 6px 14px 10px; min-height: 36px; }
  .chip { font-size: 12px; }
  .link { background: none; border: 0; padding: 2px 0; color: var(--muted); font-size: 12px; text-decoration: underline; margin-left: auto; }
  .link:hover:not(:disabled) { color: var(--fg); }
  .link:disabled { opacity: 0.5; cursor: default; }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 16px; margin-top: 20px; }
  .card { display: flex; flex-direction: column; gap: 8px; padding: 20px; text-align: left; border: 1.5px solid var(--line); border-radius: 16px; background: var(--panel); }
  .card:not(:disabled):hover { border-color: var(--accent); box-shadow: 0 6px 24px #ff5a1f22; }
  .card b { font-size: 17px; }
  .card.off { opacity: 0.55; cursor: default; }
  .text { font-size: 13px; }
  .why { font-size: 12.5px; color: var(--danger); }
  .soon { align-self: flex-start; font-size: 11px; padding: 2px 8px; border-radius: 999px; background: var(--line); color: var(--muted); }
  .error { color: var(--danger); }
</style>
