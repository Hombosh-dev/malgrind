<script lang="ts">
  import { onMount } from 'svelte';
  import Setup from '../components/Setup.svelte';
  import TopBar from '../components/TopBar.svelte';
  import { formatDuration, formatWhen } from '../lib/format';
  import { sourceLabel } from '../lib/projects';
  import type { Sidecar } from '../lib/sidecar';
  import type { ProjectMeta, RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    onNewProject(): void;
    onProject(id: string): void;
    onRun(run: RunMeta): void; // a stored run, opened for replay
  }
  let { sidecar, onNewProject, onProject, onRun }: Props = $props();

  let projects = $state<ProjectMeta[]>([]);
  let runs = $state<RunMeta[]>([]);
  let loaded = $state(false);
  let tab = $state<'projects' | 'runs'>('projects');
  let error = $state('');
  let deleting = $state<string | null>(null); // id waiting for "yes, delete"
  let renaming = $state<string | null>(null);
  let draft = $state('');

  const fail = (e: unknown) => (error = String(e instanceof Error ? e.message : e));

  onMount(async () => {
    try {
      [projects, runs] = await Promise.all([sidecar.call<ProjectMeta[]>('projects'), sidecar.call<RunMeta[]>('runs')]);
    } catch (e) {
      fail(e);
    }
    loaded = true;
  });

  async function openRun(run: RunMeta) {
    error = '';
    try {
      onRun((await sidecar.call<{ run: RunMeta }>('open', { run: run.id })).run);
    } catch (e) {
      fail(e);
    }
  }

  async function commitRename(kind: 'project' | 'run', item: ProjectMeta | RunMeta) {
    const name = draft.trim();
    renaming = null;
    if (!name || name === item.name) return;
    try {
      Object.assign(item, await sidecar.call(kind === 'project' ? 'project_update' : 'update', { [kind === 'project' ? 'project' : 'run']: item.id, name }));
    } catch (e) {
      fail(e);
    }
  }

  async function remove(kind: 'project' | 'run', id: string) {
    deleting = null;
    try {
      await sidecar.call(kind === 'project' ? 'project_remove' : 'delete', { [kind === 'project' ? 'project' : 'run']: id });
      if (kind === 'project') projects = projects.filter((p) => p.id !== id);
      else runs = runs.filter((r) => r.id !== id);
    } catch (e) {
      fail(e);
    }
  }

  function focusAndSelect(input: HTMLInputElement) {
    input.focus();
    input.select();
  }

  const first = $derived(loaded && projects.length === 0 && runs.length === 0);
</script>

<div class="page">
  <TopBar />
  <Setup {sidecar} />
  {#if error}<div class="error" role="alert">{error}</div>{/if}

  {#if !loaded}
    <p class="empty muted">Loading…</p>
  {:else if first}
    <section class="welcome">
      <h1>Profile the firmware on your board</h1>
      <p class="muted">Open a project, or just a board whose code you do not have.</p>
      <button class="btn big" onclick={onNewProject}>+ New project</button>
    </section>
  {:else}
    <section class="top">
      <button class="newcard" onclick={onNewProject}>
        <span class="plus">+</span>
        <span><b>New project</b><br /><span class="muted">a folder, a GitHub link, or only a board</span></span>
      </button>
    </section>

    <div class="tabs" role="tablist">
      <button role="tab" aria-selected={tab === 'projects'} class:on={tab === 'projects'} onclick={() => (tab = 'projects')}>Projects <span class="count">{projects.length}</span></button>
      <button role="tab" aria-selected={tab === 'runs'} class:on={tab === 'runs'} onclick={() => (tab = 'runs')}>Runs <span class="count">{runs.length}</span></button>
    </div>

    {#if tab === 'projects'}
      {#if projects.length === 0}<p class="empty muted">No projects yet. Runs made before projects existed are under Runs.</p>{/if}
      <ul class="rows">
        {#each projects as project (project.id)}
          <li>
            {#if renaming === project.id}
              <input class="rename" bind:value={draft} use:focusAndSelect aria-label="Project name"
                onkeydown={(e) => (e.key === 'Enter' ? commitRename('project', project) : e.key === 'Escape' ? (renaming = null) : null)}
                onblur={() => commitRename('project', project)} />
            {:else}
              <button class="row" onclick={() => onProject(project.id)}>
                <b class="name">{project.name}</b>
                {#if project.problem}<span class="tag warn" title={project.problem}>folder not found</span>{/if}
                <span class="tag">{sourceLabel(project)}</span>
                <span class="muted when">{project.run_count ? `${project.run_count} run${project.run_count === 1 ? '' : 's'} · ${formatWhen(project.last_run ?? '')}` : 'no runs yet'}</span>
              </button>
              <button class="icon" title="Rename" onclick={() => ((renaming = project.id), (draft = project.name))}>✎</button>
              {#if deleting === project.id}
                <button class="link danger-link" onclick={() => remove('project', project.id)}>Delete? (its runs stay)</button>
                <button class="link" onclick={() => (deleting = null)}>No</button>
              {:else}
                <button class="icon" title="Delete" onclick={() => (deleting = project.id)}>🗑</button>
              {/if}
            {/if}
          </li>
        {/each}
      </ul>
    {:else}
      {#if runs.length === 0}<p class="empty muted">No runs yet.</p>{/if}
      <ul class="rows">
        {#each runs as run (run.id)}
          <li>
            {#if renaming === run.id}
              <input class="rename" bind:value={draft} use:focusAndSelect aria-label="Run name"
                onkeydown={(e) => (e.key === 'Enter' ? commitRename('run', run) : e.key === 'Escape' ? (renaming = null) : null)}
                onblur={() => commitRename('run', run)} />
            {:else}
              <button class="row" onclick={() => openRun(run)}>
                <b class="name">{run.name || run.id}</b>
                <span class="muted when">{formatWhen(run.started)}</span>
                <span class="tag">{run.arch ?? 'no data'}{run.ncores ? ` · ${run.ncores} cores` : ''}</span>
                <span class="tag">{run.live ? 'recording' : formatDuration(run.duration_ms)}</span>
              </button>
              <button class="icon" title="Rename" onclick={() => ((renaming = run.id), (draft = run.name))}>✎</button>
              {#if deleting === run.id}
                <button class="link danger-link" onclick={() => remove('run', run.id)}>Delete?</button>
                <button class="link" onclick={() => (deleting = null)}>No</button>
              {:else}
                <button class="icon" title="Delete" onclick={() => (deleting = run.id)}>🗑</button>
              {/if}
            {/if}
          </li>
        {/each}
      </ul>
    {/if}
  {/if}
</div>

<style>
  .page { display: flex; flex-direction: column; min-height: 100%; }
  .error { margin: 12px 18px; padding: 10px 14px; border: 1px solid var(--danger); color: var(--danger); border-radius: 8px; }
  .welcome { margin: auto; padding: 48px 24px; text-align: center; }
  .welcome h1 { margin: 0 0 6px; font-size: 24px; }
  .empty { padding: 28px 24px; text-align: center; }
  .btn.big { padding: 14px 36px; font-size: 16px; margin-top: 14px; }
  .top { padding: 22px 18px 6px; }
  .newcard { display: flex; align-items: center; gap: 16px; width: 100%; padding: 18px 22px; text-align: left; background: var(--panel); border: 1.5px dashed var(--accent); border-radius: 12px; }
  .newcard:hover { background: color-mix(in srgb, var(--accent) 8%, var(--panel)); }
  .plus { display: grid; place-items: center; width: 44px; height: 44px; border-radius: 10px; font-size: 26px; color: #fff; background: linear-gradient(135deg, #ff9a3c, #ff5a1f); }
  .tabs { display: flex; gap: 4px; padding: 14px 18px 0; border-bottom: 1px solid var(--line); }
  .tabs button { padding: 8px 14px; background: none; border: 0; border-bottom: 2px solid transparent; color: var(--muted); font-weight: 600; }
  .tabs button.on { color: var(--fg); border-bottom-color: var(--accent); }
  .count { font-size: 11px; padding: 1px 7px; border-radius: 999px; background: var(--line); margin-left: 4px; }
  .rows { list-style: none; margin: 0; padding: 0; }
  li { display: flex; align-items: center; gap: 6px; padding: 0 12px 0 0; border-bottom: 1px solid var(--line); }
  .row { flex: 1; display: flex; align-items: center; gap: 14px; padding: 12px 18px; background: none; border: 0; text-align: left; }
  .row:hover { background: var(--panel); }
  .name { flex: 1; }
  .when { font-size: 12px; }
  .tag { font-size: 11px; padding: 2px 8px; border-radius: 5px; background: var(--line); }
  .tag.warn { color: var(--danger); }
  .icon, .link { background: none; border: 0; padding: 4px 8px; color: var(--muted); }
  .icon:hover, .link:hover { color: var(--fg); }
  .danger-link { color: var(--danger); }
  .rename { flex: 1; margin: 8px 18px; padding: 6px 10px; font: inherit; background: var(--panel); color: var(--fg); border: 1px solid var(--accent); border-radius: 6px; }
</style>
