<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import { save as saveDialog } from '@tauri-apps/plugin-dialog';
  import { bytesToBase64, defaultExportFilename, serializeFlameSvg, svgToPngBase64 } from '../lib/export';
  import { formatDuration } from '../lib/format';
  import { formatClockMs, hasCap } from '../lib/series';
  import type { Node as FlameNode } from '../lib/blend';
  import type { Sidecar } from '../lib/sidecar';
  import {
    CAP_HEAP,
    CAP_SAMPLES,
    CAP_TASKS,
    type FunctionsData,
    type MemoryData,
    type RunMeta,
    type RunStats,
    type TasksData,
  } from '../lib/types';
  import Flame from '../views/Flame.svelte';
  import Functions from '../views/Functions.svelte';
  import Memory from '../views/Memory.svelte';
  import Tasks from '../views/Tasks.svelte';

  interface Props {
    sidecar: Sidecar;
    run: RunMeta;
    live: boolean; // recording right now (false: a stored run)
    onClose(): void;
    onRemoveAgent(device: string): void;
  }
  let { sidecar, run, live: startsLive, onClose, onRemoveAgent }: Props = $props();

  // The parent remounts this screen for every run ({#key}), so the props are read once on purpose.
  let live = $state(untrack(() => startsLive));
  let view = $state<'flame' | 'functions' | 'memory' | 'tasks'>('flame');
  let core = $state<'all' | number>('all');
  let windowS = $state(untrack(() => startsLive) ? 10 : 0); // a stored run opens as the whole run
  let at = $state<number | null>(null); // null: the end of the run (following it, while live)
  let focus = $state('');
  let tree = $state<FlameNode | null>(null);
  let functions = $state<FunctionsData | null>(null);
  let memory = $state<MemoryData | null>(null);
  let tasks = $state<TasksData | null>(null);
  let stats = $state<RunStats>({});
  let targetCaps = $state<number | null>(null);
  let notices = $state<string[]>([]);
  let name = $state(untrack(() => run.name));
  let notes = $state(untrack(() => run.notes));
  let showNotes = $state(false);
  let showSettings = $state(false); // what the run was made with (only runs made with the settings panel have it)
  let showExport = $state(false);
  let exporting = $state(false);
  let exportNotice = $state('');
  let flame = $state<ReturnType<typeof Flame> | null | undefined>(null);
  let exportWrap = $state<HTMLDivElement | null>(null);
  let exportBtn = $state<HTMLButtonElement | null>(null);
  // the fields the user set; an empty one was the detected value
  const used = $derived(
    run.settings
      ? ([
          ['Environment', run.settings.env],
          ['Sampling rate', run.settings.rate_hz ? `${run.settings.rate_hz} Hz` : ''],
          ['Firmware ELF', run.settings.elf],
          ['Extra arguments', run.settings.extra_args],
          ['Build flags', run.settings.build_flags],
          ['Build command', run.settings.build_command],
          ['Upload command', run.settings.upload_command]
        ].filter(([, value]) => value) as [string, string][])
      : []
  );
  let error = $state('');
  let busy = false;
  let dirty = false;
  let destroyed = false;

  onMount(() => () => {
    destroyed = true;
  });

  const caps = $derived(
    stats.caps !== undefined || targetCaps !== null
      ? (stats.caps ?? 0) | (targetCaps ?? 0)
      : CAP_SAMPLES,
  );
  const showMemory = $derived(hasCap(caps, CAP_HEAP));
  const showTasks = $derived(hasCap(caps, CAP_TASKS));

  $effect(() => {
    if ((view === 'memory' && !showMemory) || (view === 'tasks' && !showTasks)) {
      view = 'flame';
    }
  });

  const duration = $derived(stats.duration_ms ?? run.duration_ms);
  const position = $derived(at ?? duration);
  const resetKey = $derived(`${core}|${windowS}|${at ?? 'end'}`);
  const imageExportDisabled = $derived(view !== 'flame' || !tree || tree.value <= 0);
  const imageExportTip = $derived(
    view !== 'flame'
      ? 'Switch to Flamegraph view to export SVG or PNG'
      : !tree || tree.value <= 0
        ? 'No samples in this window'
        : ''
  );

  type ExportFormat = 'folded' | 'speedscope' | 'svg' | 'png';
  const EXPORT_SPECS: Record<ExportFormat, { title: string; ext: string; filterName: string; extensions: string[] }> = {
    folded: { title: 'Export folded stacks', ext: 'folded', filterName: 'Folded stacks', extensions: ['folded', 'txt'] },
    speedscope: { title: 'Export Speedscope profile', ext: 'speedscope.json', filterName: 'Speedscope profile', extensions: ['json'] },
    svg: { title: 'Export flamegraph SVG', ext: 'svg', filterName: 'SVG image', extensions: ['svg'] },
    png: { title: 'Export flamegraph PNG', ext: 'png', filterName: 'PNG image', extensions: ['png'] },
  };

  $effect(() => {
    if (!showExport) return;
    const onPointerDown = (e: PointerEvent) => {
      if (exportWrap && e.target instanceof Node && !exportWrap.contains(e.target)) {
        showExport = false;
      }
    };
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        showExport = false;
        exportBtn?.focus();
      }
    };
    window.addEventListener('pointerdown', onPointerDown);
    window.addEventListener('keydown', onKeyDown);
    return () => {
      window.removeEventListener('pointerdown', onPointerDown);
      window.removeEventListener('keydown', onKeyDown);
    };
  });

  async function exportAs(format: ExportFormat) {
    showExport = false;
    if (exporting) return;
    error = '';
    exportNotice = '';

    const isImage = format === 'svg' || format === 'png';
    const svg = isImage ? (flame?.getSvg() ?? null) : null;
    if (isImage && (!svg || imageExportDisabled)) {
      error = imageExportTip || 'No flamegraph is available to export.';
      return;
    }

    const spec = EXPORT_SPECS[format];
    const baseLabel = name.trim() || run.name || run.id;
    exporting = true;
    try {
      const path = await saveDialog({
        title: spec.title,
        defaultPath: defaultExportFilename(baseLabel, spec.ext),
        filters: [{ name: spec.filterName, extensions: spec.extensions }],
      });
      if (!path) return;

      if (isImage && svg) {
        const base64 =
          format === 'svg'
            ? bytesToBase64(new TextEncoder().encode(serializeFlameSvg(svg)))
            : await svgToPngBase64(svg);
        await sidecar.call<{ bytes: number }>('write_file', { path, base64 });
      } else {
        const args: Record<string, unknown> = { run: run.id, format, path, window_s: windowS };
        if (core !== 'all') args.core = core;
        if (at !== null) args.at_ms = at;
        await sidecar.call<{ bytes: number }>('export', args);
      }
      const fileLabel = path.split(/[\\/]/).pop() || path;
      exportNotice = `Exported ${fileLabel}`;
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    } finally {
      exporting = false;
    }
  }

  async function poll(force = false) {
    if (destroyed) return;
    if (busy) {
      if (force) dirty = true;
      return;
    }
    busy = true;
    try {
      const timeArgs: Record<string, unknown> = { window_s: windowS };
      if (at !== null) timeArgs.at_ms = at;
      const args: Record<string, unknown> = { ...timeArgs };
      if (core !== 'all') args.core = core;
      const snapshot = await sidecar.call<{ tree: FlameNode | null; stats: RunStats }>('snapshot', args);
      if (destroyed) return;
      tree = snapshot.tree;
      stats = snapshot.stats ?? {};
      if (view === 'functions') {
        functions = await sidecar.call<FunctionsData>('functions', { ...args, focus });
      } else if (view === 'memory') {
        memory = await sidecar.call<MemoryData>('memory', timeArgs);
      } else if (view === 'tasks') {
        tasks = await sidecar.call<TasksData>('tasks', timeArgs);
      }
      if (destroyed) return;
      error = '';
    } catch (e) {
      if (!destroyed) error = String(e instanceof Error ? e.message : e);
    } finally {
      busy = false;
      if (dirty && !destroyed) {
        dirty = false;
        void poll(false);
      }
    }
  }

  // refresh at once when anything the picture depends on changes ...
  $effect(() => {
    void [view, core, windowS, at, focus];
    poll(true);
  });
  // ... and twice a second while recording
  $effect(() => {
    if (!live) return;
    const timer = setInterval(() => poll(false), 500);
    return () => clearInterval(timer);
  });

  $effect(() => {
    const offs = [
      sidecar.on('target', (e) => {
        if (typeof e?.caps === 'number') targetCaps = e.caps;
      }),
      sidecar.on('notice', (e) => (notices = [...notices, e.text])),
      sidecar.on('ended', () => (live = false)),
    ];
    return () => offs.forEach((off) => off());
  });

  async function close() {
    try {
      await sidecar.call('stop');
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
    onClose();
  }

  async function save(fields: { name?: string; notes?: string }) {
    try {
      const updated = await sidecar.call<RunMeta>('update', { run: run.id, ...fields });
      Object.assign(run, updated);
      if (fields.name !== undefined && name === fields.name) name = updated.name;
      if (fields.notes !== undefined && notes === fields.notes) notes = updated.notes;
    } catch (e) {
      error = String(e instanceof Error ? e.message : e);
    }
  }

  const cores = $derived(Array.from({ length: stats.ncores ?? run.ncores }, (_, i) => i));
</script>

<div class="page">
  <header>
    <span class="logo">🔥 malgrind</span>
    <input class="name" bind:value={name} onblur={() => save({ name })} onkeydown={(e) => e.key === 'Enter' && e.currentTarget.blur()} aria-label="Run name" />
    {#if live}<span class="live"><span class="dot"></span>LIVE</span>{:else}<span class="muted">{formatDuration(duration)}</span>{/if}
    {#if run.port}<span class="muted stat" title="The board this run was recorded from">{run.port}</span>{/if}
    <span class="spacer"></span>
    {#if stats.arch}
      <span class="stat muted">{stats.samples_per_s ?? '–'} samples/s · {stats.dropped ?? 0} dropped · {stats.overhead_pct == null ? '–' : stats.overhead_pct + ' %'} overhead</span>
    {/if}
    {#if run.settings}<button class="btn ghost small" onclick={() => (showSettings = !showSettings)}>Settings</button>{/if}
    <button class="btn ghost small notes-btn" class:active={showNotes} onclick={() => (showNotes = !showNotes)} aria-pressed={showNotes}>
      Notes{#if notes.trim().length > 0}<span class="notes-dot" title="This run has notes" aria-hidden="true"></span>{/if}
    </button>
    <div class="export-wrap" bind:this={exportWrap}>
      <button
        bind:this={exportBtn}
        class="btn ghost small"
        type="button"
        disabled={exporting}
        aria-haspopup="menu"
        aria-expanded={showExport}
        onclick={() => (showExport = !showExport)}
      >
        {exporting ? 'Exporting…' : 'Export ▾'}
      </button>
      {#if showExport}
        <div class="export-menu" role="menu" aria-label="Export run">
          <button type="button" role="menuitem" onclick={() => exportAs('folded')}>Folded stacks (.folded)</button>
          <button type="button" role="menuitem" onclick={() => exportAs('speedscope')}>Speedscope (.speedscope.json)</button>
          <button
            type="button"
            role="menuitem"
            disabled={imageExportDisabled}
            title={imageExportTip}
            onclick={() => exportAs('svg')}
          >
            Flamegraph SVG (.svg)
          </button>
          <button
            type="button"
            role="menuitem"
            disabled={imageExportDisabled}
            title={imageExportTip}
            onclick={() => exportAs('png')}
          >
            Flamegraph PNG (.png)
          </button>
        </div>
      {/if}
    </div>
    <button class="btn small" onclick={close}>{live ? 'Stop' : 'Close'}</button>
  </header>

  {#if showSettings && run.settings}
    <div class="used" aria-label="Settings of this run">
      {#if used.length === 0}<span class="muted">Made with everything as detected.</span>{/if}
      {#each used as [label, value]}<span><span class="muted">{label}</span> <code>{value}</code></span>{/each}
    </div>
  {/if}
  {#if showNotes}
    <textarea class="notes" bind:value={notes} onblur={() => save({ notes })} placeholder="Notes about this run…"></textarea>
  {/if}
  {#each notices as text, i}
    <div class="notice">{text}<button class="x" onclick={() => (notices = notices.filter((_, j) => j !== i))} aria-label="Dismiss">×</button></div>
  {/each}
  {#if exportNotice}
    <div class="notice">{exportNotice}<button class="x" onclick={() => (exportNotice = '')} aria-label="Dismiss">×</button></div>
  {/if}
  {#if !live && startsLive}<div class="notice">The board stopped sending data (unplugged, or reset?). The run is saved.</div>{/if}
  {#if error}<div class="notice bad" role="alert">{error}</div>{/if}

  <div class="body">
    <nav>
      <button class:on={view === 'flame'} onclick={() => (view = 'flame')}>Flamegraph</button>
      <button class:on={view === 'functions'} onclick={() => (view = 'functions')}>Functions</button>
      {#if showMemory}
        <button class:on={view === 'memory'} onclick={() => (view = 'memory')}>Memory</button>
      {/if}
      {#if showTasks}
        <button class:on={view === 'tasks'} onclick={() => (view = 'tasks')}>Tasks</button>
      {/if}
      <div class="filters">
        <label>Core
          <select bind:value={core}><option value="all">all</option>{#each cores as c}<option value={c}>{c}</option>{/each}</select>
        </label>
        <label>Window
          <select bind:value={windowS}>
            <option value={5}>5 s</option><option value={10}>10 s</option><option value={30}>30 s</option>
            <option value={60}>60 s</option><option value={0}>whole run</option>
          </select>
        </label>
      </div>
      {#if live}<button class="remove" onclick={() => onRemoveAgent(run.device_id || run.port)}>Remove agent…</button>{/if}
    </nav>

    <main>
      {#if view === 'flame'}
        <Flame bind:this={flame} {tree} {resetKey} />
      {:else if view === 'functions'}
        <Functions data={functions} {focus} onFocus={(f) => (focus = f)} />
      {:else if view === 'memory'}
        <Memory data={memory} />
      {:else}
        <Tasks data={tasks} />
      {/if}
    </main>
  </div>

  <footer>
    <span class="clock">{formatClockMs(position)}</span>
    <input type="range" min="0" max={Math.max(duration, 1)} value={position} oninput={(e) => (at = Number(e.currentTarget.value))} aria-label="Time in the run" />
    <span class="clock">{formatClockMs(duration)}</span>
    {#if at !== null}<button class="btn ghost small" onclick={() => (at = null)}>{live ? 'Back to live' : 'To the end'}</button>{/if}
  </footer>
</div>

<style>
  .page { display: flex; flex-direction: column; height: 100%; }
  header { display: flex; align-items: center; gap: 12px; padding: 8px 16px; border-bottom: 1px solid var(--line); background: var(--panel); }
  .logo { font-weight: 700; color: var(--accent); }
  .name { font: 600 14px var(--font); color: var(--fg); background: none; border: 1px solid transparent; border-radius: 6px; padding: 3px 8px; min-width: 200px; }
  .name:hover, .name:focus { border-color: var(--line); background: var(--bg); outline: none; }
  .live { display: inline-flex; align-items: center; gap: 6px; color: var(--live); font-weight: 600; font-size: 12px; }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--live); box-shadow: 0 0 8px var(--live); }
  .spacer { flex: 1; }
  .stat { font-size: 12px; font-variant-numeric: tabular-nums; }
  .used { display: flex; flex-wrap: wrap; gap: 6px 18px; padding: 8px 16px; font-size: 12.5px; background: var(--panel); border-bottom: 1px solid var(--line); }
  .used code { font-family: var(--mono); font-size: 12px; }
  .notes-btn { display: inline-flex; align-items: center; gap: 6px; }
  .notes-btn.active { background: color-mix(in srgb, var(--accent) 14%, transparent); }
  .notes-dot { width: 6px; height: 6px; border-radius: 50%; background: var(--accent); }
  .export-wrap { position: relative; }
  .export-menu {
    position: absolute; right: 0; top: calc(100% + 6px); min-width: 220px; padding: 4px;
    display: flex; flex-direction: column; background: var(--panel); border: 1px solid var(--line);
    border-radius: 8px; box-shadow: 0 8px 24px #00000026; z-index: 20;
  }
  .export-menu button {
    text-align: left; padding: 7px 10px; font-size: 12.5px; background: none; border: 0; border-radius: 6px; color: var(--fg);
  }
  .export-menu button:hover:not(:disabled) { background: color-mix(in srgb, var(--accent) 14%, transparent); }
  .export-menu button:disabled { opacity: 0.45; cursor: not-allowed; }
  .notes { margin: 0; width: 100%; min-height: 70px; padding: 10px 16px; font: inherit; color: var(--fg); background: var(--panel); border: 0; border-bottom: 1px solid var(--line); resize: vertical; }
  .notice { display: flex; justify-content: space-between; padding: 8px 16px; background: color-mix(in srgb, var(--accent) 12%, var(--panel)); border-bottom: 1px solid var(--line); font-size: 13px; }
  .notice.bad { background: color-mix(in srgb, var(--danger) 14%, var(--panel)); color: var(--danger); }
  .x { background: none; border: 0; color: var(--muted); font-size: 16px; }
  .body { flex: 1; display: flex; min-height: 0; }
  nav { width: 168px; display: flex; flex-direction: column; border-right: 1px solid var(--line); padding: 8px 0; }
  nav > button { text-align: left; padding: 9px 16px; background: none; border: 0; border-left: 3px solid transparent; color: var(--muted); }
  nav > button.on { color: var(--fg); background: var(--panel); border-left-color: var(--accent); }
  .filters { margin-top: 18px; padding: 0 16px; display: flex; flex-direction: column; gap: 10px; font-size: 12px; color: var(--muted); }
  .filters select { width: 100%; margin-top: 3px; }
  .remove { margin-top: auto; padding: 10px 16px; text-align: left; background: none; border: 0; border-top: 1px solid var(--line); color: var(--danger); }
  main { flex: 1; min-width: 0; overflow: auto; padding: 14px 18px; }
  footer { display: flex; align-items: center; gap: 12px; padding: 8px 16px; border-top: 1px solid var(--line); background: var(--panel); }
  footer input[type='range'] { flex: 1; accent-color: var(--accent); }
  .clock { font: 12px var(--mono); color: var(--muted); }
</style>
