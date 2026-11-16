<script lang="ts">
  import { onMount } from 'svelte';
  import TopBar from '../components/TopBar.svelte';
  import { formatDuration, formatWhen } from '../lib/format';
  import { sourceLabel } from '../lib/projects';
  import type { Sidecar } from '../lib/sidecar';
  import type { ProjectMeta, RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    id: string;
    onBack(): void;
    onProfile(project: ProjectMeta): void;
    onRun(run: RunMeta): void; // a stored run of this project, opened for replay
  }
  let { sidecar, id, onBack, onProfile, onRun }: Props = $props();

  let project = $state<ProjectMeta | null>(null);
  let runs = $state<RunMeta[]>([]);
  let error = $state('');
  let renaming = $state(false);
  let draft = $state('');

  const fail = (e: unknown) => (error = String(e instanceof Error ? e.message : e));

  async function commitRename() {
    const name = draft.trim();
    renaming = false;
    if (name && project && name !== project.name) await change({ name });
  }

  function focusAndSelect(input: HTMLInputElement) {
    input.focus();
    input.select();
  }

  onMount(async () => {
    try {
      project = (await sidecar.call<ProjectMeta[]>('projects')).find((p) => p.id === id) ?? null;
      if (!project) error = 'This project no longer exists.';
      runs = (await sidecar.call<RunMeta[]>('runs')).filter((r) => r.project_id === id);
    } catch (e) {
      fail(e);
    }
  });

  async function change(args: Record<string, unknown>) {
    error = '';
    try {
      Object.assign(project!, await sidecar.call('project_update', { project: id, ...args }));
    } catch (e) {
      fail(e);
    }
  }

  async function open(run: RunMeta) {
    try {
      onRun((await sidecar.call<{ run: RunMeta }>('open', { run: run.id })).run);
    } catch (e) {
      fail(e);
    }
  }
</script>

<div class="page">
  <TopBar onBack={onBack} backLabel="All projects" />
  {#if error}<div class="error" role="alert">{error}</div>{/if}

  {#if project}
    <section>
      <div class="head">
        <div>
          {#if renaming}
            <input class="rename" bind:value={draft} use:focusAndSelect aria-label="Project name"
              onkeydown={(e) => (e.key === 'Enter' ? commitRename() : e.key === 'Escape' ? (renaming = false) : null)}
              onblur={commitRename} />
          {:else}
            <h1>{project.name} <button class="icon" title="Rename the project" aria-label="Rename the project" onclick={() => ((renaming = true), (draft = project!.name))}>✎</button></h1>
          {/if}
          <p class="muted">
            <span class="tag">{sourceLabel(project)}</span>
            {#if project.source === 'git'}{project.url}{:else if project.dir}<code>{project.dir}</code>{:else}firmware: not provided{project.elf ? ' (ELF added)' : ''}{/if}
          </p>
          {#if project.problem}<p class="error">{project.problem}</p>{/if}
        </div>
        <button class="btn big" onclick={() => onProfile(project!)} disabled={!!project.problem && project.source !== 'none'}>Profile ▸</button>
      </div>

      {#if project.source === 'git' && !project.trusted}
        <div class="trust" role="alert">
          <b>This project came from GitHub.</b> Building it runs the scripts in its <code>platformio.ini</code> on your computer.
          Only build it if you trust whoever wrote it. Attaching to a board does not build anything.
          <div><button class="btn small" onclick={() => change({ trusted: true })}>Trust this project</button></div>
        </div>
      {/if}

      {#if project.envs.length > 1}
        <label class="env">Environment
          <select value={project.env} onchange={(e) => change({ env: e.currentTarget.value })}>{#each project.envs as env}<option>{env}</option>{/each}</select>
        </label>
      {/if}

      <h2>Runs <span class="count">{runs.length}</span></h2>
      {#if runs.length === 0}
        <p class="muted">Nothing recorded yet. Press Profile, choose the board and how to look at it.</p>
      {:else}
        <ul>
          {#each runs as run (run.id)}
            <li><button class="row" onclick={() => open(run)}>
              <b class="name">{run.name || run.id}</b>
              <span class="muted when">{formatWhen(run.started)}</span>
              <span class="tag">{run.arch ?? 'no data'}{run.ncores ? ` · ${run.ncores} cores` : ''}</span>
              <span class="tag">{formatDuration(run.duration_ms)}</span>
            </button></li>
          {/each}
        </ul>
      {/if}
    </section>
  {/if}
</div>

<style>
  .page { min-height: 100%; }
  .error { margin: 12px 18px; padding: 10px 14px; border: 1px solid var(--danger); color: var(--danger); border-radius: 8px; }
  section { max-width: 820px; margin: 0 auto; padding: 24px; }
  .head { display: flex; align-items: flex-start; justify-content: space-between; gap: 20px; }
  h1 { margin: 0 0 4px; font-size: 24px; }
  .icon { background: none; border: 0; padding: 2px 6px; font-size: 15px; color: var(--muted); vertical-align: middle; }
  .icon:hover { color: var(--fg); }
  .rename { width: 100%; margin: 0 0 6px; padding: 6px 10px; font: 600 22px var(--font); color: var(--fg); background: var(--panel); border: 1px solid var(--accent); border-radius: 8px; }
  h2 { margin: 28px 0 8px; font-size: 15px; }
  .btn.big { padding: 14px 32px; font-size: 16px; }
  .trust { margin: 16px 0; padding: 12px 14px; border: 1px solid var(--accent); border-radius: 10px; background: color-mix(in srgb, var(--accent) 8%, var(--panel)); }
  .trust .btn { margin-top: 10px; }
  .env { display: block; margin-top: 14px; }
  ul { list-style: none; margin: 0; padding: 0; border-top: 1px solid var(--line); }
  li { border-bottom: 1px solid var(--line); }
  .row { display: flex; align-items: center; gap: 14px; width: 100%; padding: 12px 6px; background: none; border: 0; text-align: left; }
  .row:hover { background: var(--panel); }
  .name { flex: 1; }
  .when { font-size: 12px; }
  .tag { font-size: 11px; padding: 2px 8px; border-radius: 5px; background: var(--line); margin-right: 6px; }
  .count { font-size: 11px; padding: 1px 7px; border-radius: 999px; background: var(--line); }
  code { font-family: var(--mono); background: var(--panel); padding: 1px 5px; border-radius: 4px; }
</style>
