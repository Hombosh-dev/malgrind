<script lang="ts">
  import { open } from '@tauri-apps/plugin-dialog';
  import { getCurrentWebview } from '@tauri-apps/api/webview';
  import BuildLog from '../components/BuildLog.svelte';
  import TopBar from '../components/TopBar.svelte';
  import { classifyDrop, parseGithub } from '../lib/projects';
  import { baseName } from '../lib/format';
  import type { Sidecar } from '../lib/sidecar';
  import type { ProjectMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    onCancel(): void;
    onCreated(project: ProjectMeta): void;
  }
  let { sidecar, onCancel, onCreated }: Props = $props();

  let name = $state('');
  let link = $state('');
  let working = $state(false);
  let hovering = $state(false);
  let lines = $state<string[]>([]);
  let error = $state('');

  const repo = $derived(parseGithub(link));

  $effect(() => sidecar.on('build', (e) => lines.push(e.line)));

  // dropping a folder or an .elf anywhere on the window
  $effect(() => {
    let off: (() => void) | undefined;
    let gone = false;
    getCurrentWebview()
      .onDragDropEvent((event) => {
        const p = event.payload;
        if (p.type === 'enter' || p.type === 'over') hovering = true;
        else if (p.type === 'leave') hovering = false;
        else if (p.type === 'drop') {
          hovering = false;
          dropped(p.paths);
        }
      })
      .then((unlisten) => (gone ? unlisten() : (off = unlisten)))
      .catch(() => {}); // no drag and drop (a plain browser): the buttons still work
    return () => {
      gone = true;
      off?.();
    };
  });

  async function add(args: Record<string, unknown>) {
    if (working) return;
    working = true;
    error = '';
    lines = [];
    try {
      onCreated(await sidecar.call<ProjectMeta>('project_add', { ...args, name: name.trim() }));
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    working = false;
  }

  function dropped(paths: string[]) {
    const drop = classifyDrop(paths);
    if (drop.kind === 'unsupported') error = drop.why;
    else add(drop.kind === 'folder' ? { source: 'folder', dir: drop.path } : { source: 'none', elf: drop.path });
  }

  async function chooseFolder() {
    const picked = await open({ directory: true, multiple: false, title: 'Choose your PlatformIO project' });
    if (typeof picked === 'string') add({ source: 'folder', dir: picked });
  }

  async function chooseElf() {
    const picked = await open({ multiple: false, title: 'Firmware ELF (optional: gives function names)', filters: [{ name: 'ELF', extensions: ['elf'] }] });
    if (typeof picked === 'string') add({ source: 'none', elf: picked });
  }
</script>

<div class="page">
  <TopBar title="New project" onBack={onCancel} backLabel="Cancel" />

  <section>
    <div class="drop" class:hover={hovering} role="group" aria-label="Open a project">
      <div class="big">Drop a project folder here</div>
      <p class="muted">a PlatformIO project (ESP-IDF), or a firmware <code>.elf</code> for a board without code</p>
      <button class="btn" onclick={chooseFolder} disabled={working}>Choose folder…</button>
    </div>

    <div class="card">
      <h2>From GitHub</h2>
      <div class="line">
        <input bind:value={link} placeholder="https://github.com/owner/repository" spellcheck="false" aria-label="GitHub link"
          onkeydown={(e) => e.key === 'Enter' && repo && add({ source: 'git', url: link })} />
        <button class="btn" disabled={!repo || working} onclick={() => add({ source: 'git', url: link })}>Clone</button>
      </div>
      <p class="muted small">Public repositories only. It is downloaded once into malgrind's own folder; you decide later whether to trust it.</p>
    </div>

    <div class="card">
      <h2>No code, just a board</h2>
      <p class="muted small">The firmware on the board stays as it is. You can attach to it if it runs the malgrind agent.</p>
      <div class="line">
        <button class="btn ghost" disabled={working} onclick={() => add({ source: 'none' })}>Continue without code</button>
        <button class="btn ghost" disabled={working} onclick={chooseElf}>I have its .elf…</button>
      </div>
    </div>

    <label class="name">Name <span class="muted">(optional)</span>
      <input bind:value={name} placeholder={link && repo ? repo : 'taken from the folder'} aria-label="Project name" />
    </label>

    {#if working && lines.length > 0}<BuildLog {lines} busy={working} />{/if}
    {#if error}<p class="error" role="alert">{error}</p>{/if}
  </section>
</div>

<style>
  .page { min-height: 100%; }
  section { max-width: 720px; margin: 0 auto; padding: 24px; display: flex; flex-direction: column; gap: 16px; }
  .drop { padding: 36px 24px; text-align: center; border: 2px dashed var(--line); border-radius: 14px; background: var(--panel); }
  .drop.hover { border-color: var(--accent); background: color-mix(in srgb, var(--accent) 8%, var(--panel)); }
  .big { font-size: 20px; font-weight: 600; margin-bottom: 4px; }
  .card { padding: 16px 18px; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); }
  h2 { margin: 0 0 8px; font-size: 15px; }
  .line { display: flex; gap: 10px; align-items: center; }
  input { flex: 1; min-width: 0; padding: 8px 10px; font: 13px var(--mono); color: var(--fg); background: var(--bg); border: 1px solid var(--line); border-radius: 6px; }
  input:focus { outline: none; border-color: var(--accent); }
  .name { display: flex; align-items: center; gap: 10px; font-size: 13px; }
  .name input { font: inherit; }
  .small { font-size: 12px; margin: 8px 0 0; }
  .error { color: var(--danger); margin: 0; }
  code { font-family: var(--mono); background: var(--bg); padding: 1px 5px; border-radius: 4px; }
</style>
