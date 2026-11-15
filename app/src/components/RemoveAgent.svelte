<script lang="ts">
  import BuildLog from './BuildLog.svelte';
  import ProjectPicker from './ProjectPicker.svelte';
  import type { Sidecar } from '../lib/sidecar';

  interface Props {
    sidecar: Sidecar;
    port: string;
    onDone(): void;
    onCancel(): void;
  }
  let { sidecar, port, onDone, onCancel }: Props = $props();

  let dir = $state('');
  let env = $state('');
  let valid = $state(false);
  let working = $state(false);
  let started = $state(false);
  let lines = $state<string[]>([]);
  let error = $state('');

  $effect(() => sidecar.on('build', (e) => lines.push(e.line)));

  async function remove() {
    working = started = true;
    error = '';
    lines = [];
    try {
      await sidecar.call('remove_agent', { dir, env, port });
      onDone();
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    working = false;
  }
</script>

<div class="backdrop" role="presentation">
  <div class="dialog" role="dialog" aria-modal="true" aria-labelledby="remove-title">
    <h3 id="remove-title">Remove the agent from the board?</h3>
    <p class="muted">
      Rebuilds your project without malgrind and flashes it. The board runs your original firmware again. Your code is not touched.
      Live profiling stops.
    </p>
    <p class="muted">Port <code>{port}</code></p>
    <ProjectPicker {sidecar} bind:dir bind:env bind:valid />
    {#if started}<BuildLog {lines} busy={working} />{/if}
    {#if error}<p class="error" role="alert">{error}</p>{/if}
    <div class="actions">
      <button class="btn danger" disabled={!valid || working} onclick={remove}>{working ? 'Flashing…' : 'Remove & flash'}</button>
      <button class="btn ghost" disabled={working} onclick={onCancel}>Cancel</button>
    </div>
  </div>
</div>

<style>
  .backdrop { position: fixed; inset: 0; display: grid; place-items: center; background: #0008; }
  .dialog { width: min(560px, 92vw); padding: 20px 22px; background: var(--panel); border: 1px solid var(--line); border-radius: 12px; }
  h3 { margin: 0 0 8px; }
  .actions { display: flex; gap: 10px; margin-top: 16px; }
  .error { color: var(--danger); }
  code { font-family: var(--mono); }
</style>
