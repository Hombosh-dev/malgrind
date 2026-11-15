<script lang="ts">
  import { open } from '@tauri-apps/plugin-dialog';
  import { onMount } from 'svelte';
  import type { Sidecar } from '../lib/sidecar';
  import type { ProjectInfo } from '../lib/types';
  import { remember, remembered } from '../lib/prefs';

  interface Props {
    sidecar: Sidecar;
    dir: string; // the project folder; empty until one is chosen
    env: string;
    valid: boolean; // dir holds a usable PlatformIO project
  }
  let { sidecar, dir = $bindable(), env = $bindable(), valid = $bindable() }: Props = $props();

  let envs = $state<string[]>([]);
  let error = $state('');

  async function check(path: string) {
    valid = false;
    error = '';
    if (!path) return;
    try {
      const info = await sidecar.call<ProjectInfo>('project', { dir: path });
      dir = info.dir;
      envs = info.envs;
      env = info.envs.includes(env) ? env : info.default_env;
      valid = true;
      remember('project', info.dir);
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  async function choose() {
    const picked = await open({ directory: true, multiple: false, defaultPath: dir || undefined, title: 'Choose your PlatformIO project' });
    if (typeof picked === 'string') await check(picked);
  }

  onMount(() => {
    dir ||= remembered('project');
    check(dir);
  });
</script>

<div class="picker">
  <div class="line">
    <button class="btn ghost" onclick={choose}>{dir ? 'Change folder…' : 'Choose folder…'}</button>
    <input class="path" bind:value={dir} placeholder="/path/to/your/platformio/project" spellcheck="false"
      onchange={() => check(dir)} aria-label="Project folder" />
  </div>
  {#if valid && envs.length > 1}
    <label>Environment <select bind:value={env}>{#each envs as e}<option>{e}</option>{/each}</select></label>
  {:else if valid}
    <span class="muted">Environment {env}</span>
  {/if}
  {#if error}<p class="error">{error}</p>{/if}
</div>

<style>
  .picker { display: flex; flex-direction: column; gap: 10px; }
  .line { display: flex; align-items: center; gap: 12px; }
  .path { flex: 1; min-width: 0; padding: 6px 10px; font: 12px var(--mono); color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; }
  .path:focus { outline: none; border-color: var(--accent); }
  .error { color: var(--danger); margin: 0; }
</style>
