<script lang="ts">
  import { onMount } from 'svelte';
  import BuildLog from '../components/BuildLog.svelte';
  import ProjectPicker from '../components/ProjectPicker.svelte';
  import { defaultRunName } from '../lib/format';
  import type { Sidecar } from '../lib/sidecar';
  import type { Hello, RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    hello: Hello;
    onCancel(): void;
    onRun(run: RunMeta, recording: boolean): void;
  }
  let { sidecar, hello, onCancel, onRun }: Props = $props();

  const steps = ['Project', 'Board', 'Build & flash', 'Live'];
  let step = $state(0);
  let dir = $state('');
  let env = $state('');
  let valid = $state(false);
  let ports = $state<string[]>([]);
  let port = $state('');
  let chip = $state('');
  let identifying = $state(false);
  let lines = $state<string[]>([]);
  let building = $state(false);
  let error = $state('');

  onMount(async () => {
    ports = await sidecar.call<string[]>('ports');
    port = ports[0] ?? '';
  });

  $effect(() => {
    const off = sidecar.on('build', (e) => {
      lines.push(e.line);
      if (lines.length > 200) lines.splice(0, lines.length - 200);
    });
    return off;
  });

  async function identify() {
    identifying = true;
    chip = '';
    try {
      chip = (await sidecar.call<{ chip: string }>('chip_id', { port })).chip;
    } catch (e) {
      chip = String(e instanceof Error ? e.message : e);
    }
    identifying = false;
  }

  async function start() {
    step = 2;
    building = true;
    error = '';
    lines = [];
    try {
      const { run } = await sidecar.call<{ run: RunMeta }>('new_run', { dir, env, port, name: defaultRunName(dir) });
      step = 3;
      onRun(run, true);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    building = false;
  }
</script>

<div class="page">
  <header>
    <span class="logo">🔥 malgrind</span>
    <span class="title">New run</span>
    <span class="spacer"></span>
    <button class="btn ghost small" onclick={onCancel} disabled={building}>Cancel</button>
  </header>

  <ol class="steps">
    {#each steps as name, i}<li class:done={i < step} class:on={i === step}>{i < step ? '✓ ' : ''}{name}</li>{/each}
  </ol>

  <section>
    {#if hello.pio === null}
      <div class="problem">
        PlatformIO was not found. Install it with <code>pip install platformio</code> and restart malgrind.
        (Attaching to a board that already runs the agent works without it.)
      </div>
    {:else if step === 0}
      <h2>Your project</h2>
      <p class="muted">A PlatformIO project for an ESP32 board (ESP-IDF framework). Your code is not changed.</p>
      <ProjectPicker {sidecar} bind:dir bind:env bind:valid />
      <div class="actions"><button class="btn" disabled={!valid} onclick={() => (step = 1)}>Next</button></div>
    {:else if step === 1}
      <h2>The board</h2>
      {#if ports.length === 0}
        <p class="problem">No serial port found. Connect the board and wait a moment, then press Back and Next again.</p>
      {:else if ports.length === 1}
        <p>Found <code>{port}</code></p>
      {:else}
        <label>Port <select bind:value={port}>{#each ports as p}<option>{p}</option>{/each}</select></label>
      {/if}
      {#if port}
        <p><button class="btn ghost small" onclick={identify} disabled={identifying}>{identifying ? 'Identifying…' : 'Identify chip'}</button>
          <span class="muted">resets the board</span></p>
        {#if chip}<p class="muted">{chip}</p>{/if}
      {/if}
      <div class="actions">
        <button class="btn ghost" onclick={() => (step = 0)}>Back</button>
        <button class="btn" disabled={!port} onclick={start}>Build &amp; flash</button>
      </div>
    {:else}
      <h2>{building ? 'Building with the malgrind agent and flashing…' : 'The build did not finish'}</h2>
      <BuildLog {lines} busy={building} />
      {#if error}
        <p class="error" role="alert">{error}</p>
        <div class="actions"><button class="btn ghost" onclick={() => (step = 1)}>Back</button></div>
      {/if}
    {/if}
  </section>
</div>

<style>
  .page { min-height: 100%; }
  header { display: flex; align-items: center; gap: 12px; padding: 10px 18px; border-bottom: 1px solid var(--line); background: var(--panel); }
  .logo { font-weight: 700; color: var(--accent); }
  .title { font-weight: 600; }
  .spacer { flex: 1; }
  .steps { display: flex; gap: 8px; list-style: none; margin: 0; padding: 18px 24px 0; }
  .steps li { padding: 4px 12px; border-radius: 999px; background: var(--line); color: var(--muted); }
  .steps li.done { color: var(--live); }
  .steps li.on { background: var(--accent); color: var(--accent-fg); font-weight: 600; }
  section { max-width: 760px; padding: 24px; }
  h2 { margin: 0 0 6px; font-size: 18px; }
  .actions { display: flex; gap: 10px; margin-top: 20px; }
  .problem { padding: 12px 14px; border: 1px solid var(--line); border-radius: 8px; }
  .error { color: var(--danger); }
  code { font-family: var(--mono); background: var(--panel); padding: 1px 5px; border-radius: 4px; }
</style>
