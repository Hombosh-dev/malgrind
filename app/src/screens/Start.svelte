<script lang="ts">
  import { onMount } from 'svelte';
  import type { Sidecar } from '../lib/sidecar';
  import type { Hello, Probe, RunMeta } from '../lib/types';
  import { defaultRunName, formatDuration, formatWhen } from '../lib/format';
  import ProjectPicker from '../components/ProjectPicker.svelte';
  import { remembered } from '../lib/prefs';

  interface Props {
    sidecar: Sidecar;
    hello: Hello;
    onNewRun(): void;
    onRun(run: RunMeta, recording: boolean): void; // a run to show: attached live (true), or opened from history (false)
    onRemoveAgent(port: string): void;
  }
  let { sidecar, hello, onNewRun, onRun, onRemoveAgent }: Props = $props();

  let runs = $state<RunMeta[]>([]);
  let ports = $state<string[]>([]);
  let port = $state('');
  let probe = $state<Probe | null>(null);
  let loaded = $state(false);
  let probing = $state(false);
  let error = $state('');
  let deleting = $state<string | null>(null); // run waiting for "yes, delete"
  let renaming = $state<string | null>(null);
  let draft = $state('');
  // the project the running firmware was built from: only needed for function names
  let project = $state(remembered('project'));
  let projectEnv = $state('');
  let projectValid = $state(false);

  async function scan() {
    probing = true;
    try {
      ports = await sidecar.call<string[]>('ports');
      if (!ports.includes(port)) port = ports[0] ?? '';
      probe = port ? await sidecar.call<Probe>('probe', { port }) : null;
    } catch (e) {
      error = String(e);
    }
    probing = false;
  }

  async function load() {
    try {
      runs = await sidecar.call<RunMeta[]>('runs');
    } catch (e) {
      error = String(e);
    }
    loaded = true;
    await scan();
  }
  onMount(load);

  async function attach() {
    error = '';
    try {
      const chosen = projectValid ? project : '';
      const { run } = await sidecar.call<{ run: RunMeta }>('attach', { port, name: defaultRunName(chosen), project: chosen });
      onRun(run, true);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
      await scan();
    }
  }

  async function open(run: RunMeta) {
    error = '';
    try {
      onRun((await sidecar.call<{ run: RunMeta }>('open', { run: run.id })).run, false);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  async function commitRename(run: RunMeta) {
    const name = draft.trim();
    renaming = null;
    if (!name || name === run.name) return;
    try {
      Object.assign(run, await sidecar.call<RunMeta>('update', { run: run.id, name }));
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  async function remove(run: RunMeta) {
    deleting = null;
    try {
      await sidecar.call('delete', { run: run.id });
      runs = runs.filter((r) => r.id !== run.id);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  function focusAndSelect(input: HTMLInputElement) {
    input.focus();
    input.select();
  }

  const methodLabels = {
    attach: 'Attach to the running firmware',
    new_run: 'Build with the agent and flash (the board is backed up first)',
    no_flash: 'Profile without flashing (debug probe)'
  };

  const boardLine = $derived(
    probe?.agent ? `${probe.arch} · ${probe.ncores} cores · ${probe.cpu_mhz} MHz · ${port}` : port ? port : 'no board found'
  );
</script>

<div class="page">
  <header>
    <span class="logo">🔥 malgrind</span>
    {#if runs.length > 0}<span class="title">Runs</span>{/if}
    <span class="spacer"></span>
    <span class="board muted">{boardLine}</span>
    <button class="btn ghost small" onclick={scan} disabled={probing}>{probing ? 'Scanning…' : 'Rescan'}</button>
    {#if runs.length > 0}<button class="btn small" onclick={onNewRun}>+ New run</button>{/if}
  </header>

  {#if error}<div class="error" role="alert">{error}</div>{/if}

  {#if ports.length > 1}
    <div class="picker">
      <label>Port <select bind:value={port} onchange={scan}>{#each ports as p}<option>{p}</option>{/each}</select></label>
    </div>
  {/if}

  {#if probe && port}
    <ul class="methods" aria-label="Ways to profile this board">
      {#each probe.methods as m (m.id)}
        <li class:off={!m.available}>
          <span class="mark" aria-hidden="true">{m.available ? '✔' : '✘'}</span>
          <b>{methodLabels[m.id]}</b>
          {#if !m.available}<span class="muted">{m.why}</span>{/if}
        </li>
      {/each}
    </ul>
  {/if}

  {#if probe?.agent}
    <section class="live-card">
      <span class="dot"></span>
      <div class="grow">
        <b>Your firmware is running</b><br />
        <span class="muted">malgrind agent found · {probe.ncores} cores · {probe.cpu_mhz} MHz</span>
      </div>
      <button class="btn" onclick={attach}>Attach live</button>
      <button class="btn ghost" onclick={onNewRun}>+ New run</button>
      <button class="link danger-link" onclick={() => onRemoveAgent(port)}>Remove agent…</button>
      <div class="project">
        <span class="muted">Your project, for function names:</span>
        <ProjectPicker {sidecar} bind:dir={project} bind:env={projectEnv} bind:valid={projectValid} />
      </div>
    </section>
  {:else if probe?.busy}
    <div class="notice">Port {port} is busy: another program is using it (a serial monitor, or another malgrind).</div>
  {/if}

  {#if !loaded}
    <p class="empty muted">Loading…</p>
  {:else if runs.length === 0}
    <section class="empty">
      <h1>No runs yet</h1>
      <p class="muted">
        {#if ports.length === 0}Plug in a board, then profile your firmware.{:else}Profile your firmware on the connected board.{/if}
      </p>
      {#if !probe?.agent}<button class="btn big" onclick={onNewRun}>+ New run</button>{/if}
      {#if hello.pio === null}
        <p class="hint">New run needs PlatformIO: <code>pip install platformio</code>. Attaching to a running board works without it.</p>
      {/if}
    </section>
  {:else}
    <ul class="runs">
      {#each runs as run (run.id)}
        <li>
          {#if renaming === run.id}
            <input
              class="rename"
              bind:value={draft}
              onkeydown={(e) => (e.key === 'Enter' ? commitRename(run) : e.key === 'Escape' ? (renaming = null) : null)}
              onblur={() => commitRename(run)}
              use:focusAndSelect
            />
          {:else}
            <button class="row" onclick={() => open(run)}>
              <b class="name">{run.name || run.id}</b>
              <span class="muted when">{formatWhen(run.started)}</span>
              <span class="tag">{run.arch ?? 'no data'}{run.ncores ? ` · ${run.ncores} cores` : ''}</span>
              <span class="tag">{run.live ? 'recording' : formatDuration(run.duration_ms)}</span>
            </button>
            <button class="icon" title="Rename" onclick={() => ((renaming = run.id), (draft = run.name))}>✎</button>
            {#if deleting === run.id}
              <button class="link danger-link" onclick={() => remove(run)}>Delete?</button>
              <button class="link" onclick={() => (deleting = null)}>No</button>
            {:else}
              <button class="icon" title="Delete" onclick={() => (deleting = run.id)}>🗑</button>
            {/if}
          {/if}
        </li>
      {/each}
    </ul>
  {/if}
</div>

<style>
  .page { display: flex; flex-direction: column; min-height: 100%; }
  header { display: flex; align-items: center; gap: 12px; padding: 10px 18px; border-bottom: 1px solid var(--line); background: var(--panel); }
  .logo { font-weight: 700; color: var(--accent); letter-spacing: 0.3px; }
  .title { font-weight: 600; }
  .spacer, .grow { flex: 1; }
  .board { font-size: 12px; }
  .error { margin: 12px 18px; padding: 10px 14px; border: 1px solid var(--danger); color: var(--danger); border-radius: 8px; }
  .notice { margin: 14px 18px; padding: 10px 14px; border: 1px solid var(--line); border-radius: 8px; color: var(--muted); }
  .picker { padding: 10px 18px; }
  .live-card { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin: 18px; padding: 14px 18px; border: 1px solid var(--live); border-radius: 10px; background: color-mix(in srgb, var(--live) 10%, var(--panel)); }
  .methods { list-style: none; margin: 14px 18px 0; padding: 0; display: flex; flex-direction: column; gap: 4px; font-size: 13px; border: 0; }
  .methods li { display: flex; flex-wrap: wrap; gap: 8px; padding: 0; border: 0; }
  .methods .mark { width: 1.2em; color: var(--live); }
  .methods .off .mark { color: var(--danger); }
  .project { flex-basis: 100%; display: flex; flex-direction: column; gap: 6px; font-size: 12px; }
  .dot { width: 10px; height: 10px; border-radius: 50%; background: var(--live); box-shadow: 0 0 10px var(--live); }
  .empty { margin: auto; padding: 48px 24px; text-align: center; }
  .empty h1 { margin: 0 0 6px; font-size: 22px; }
  .btn.big { padding: 14px 36px; font-size: 16px; margin-top: 12px; }
  .hint { margin-top: 24px; color: var(--muted); font-size: 12px; }
  code { font-family: var(--mono); background: var(--panel); padding: 1px 5px; border-radius: 4px; }
  .runs { list-style: none; margin: 0; padding: 0; }
  li { display: flex; align-items: center; gap: 6px; padding: 0 12px 0 0; border-bottom: 1px solid var(--line); }
  .row { flex: 1; display: flex; align-items: center; gap: 14px; padding: 12px 18px; background: none; border: 0; text-align: left; }
  .row:hover { background: var(--panel); }
  .name { flex: 1; }
  .when { font-size: 12px; }
  .tag { font-size: 11px; padding: 2px 8px; border-radius: 5px; background: var(--line); }
  .icon, .link { background: none; border: 0; padding: 4px 8px; color: var(--muted); }
  .icon:hover, .link:hover { color: var(--fg); }
  .danger-link { color: var(--danger); }
  .rename { flex: 1; margin: 8px 18px; padding: 6px 10px; font: inherit; background: var(--panel); color: var(--fg); border: 1px solid var(--accent); border-radius: 6px; }
</style>
