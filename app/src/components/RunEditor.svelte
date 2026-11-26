<script lang="ts">
  import { untrack } from 'svelte';
  import type { Sidecar } from '../lib/sidecar';
  import type { RunMeta } from '../lib/types';

  interface Props {
    sidecar: Sidecar;
    run: RunMeta;
    onSave(updated: RunMeta): void;
    onClose(): void;
    onError(message: string): void;
  }
  let { sidecar, run, onSave, onClose, onError }: Props = $props();

  let draftName = $state(untrack(() => run.name));
  let draftNotes = $state(untrack(() => run.notes ?? ''));
  let submitting = $state(false);
  let closed = false;
  let inFlight: Promise<boolean> | null = null;

  const cleanName = () => draftName.trim() || run.name;

  function focusAndSelect(input: HTMLInputElement) {
    input.focus();
    input.select();
  }

  async function persist(): Promise<boolean> {
    const name = cleanName();
    const notes = draftNotes;
    onError('');
    const req = sidecar
      .call<RunMeta>('update', { run: run.id, name, notes })
      .then(
        (updated) => {
          onSave(updated);
          return true;
        },
        (e) => {
          onError(String(e instanceof Error ? e.message : e));
          return false;
        },
      )
      .finally(() => {
        if (inFlight === req) inFlight = null;
      });
    inFlight = req;
    return req;
  }

  function handleNotesBlur() {
    if (closed) return;
    void persist();
  }

  async function saveAndClose() {
    if (closed || submitting) return;
    submitting = true;
    try {
      let ok = inFlight ? await inFlight : await persist();
      if (ok && (cleanName() !== run.name || draftNotes !== (run.notes ?? ''))) {
        ok = await persist();
      }
      if (!ok) return;
      closed = true;
      onClose();
    } finally {
      submitting = false;
    }
  }

  function cancel() {
    if (closed) return;
    closed = true;
    onClose();
  }

  function handleKeydown(e: KeyboardEvent) {
    if (e.key === 'Escape') {
      e.preventDefault();
      e.stopPropagation();
      cancel();
    }
  }
</script>

<!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
<form class="editor" onsubmit={(e) => { e.preventDefault(); void saveAndClose(); }} onkeydown={handleKeydown}>
  <label class="field">
    <span class="label">Name</span>
    <input
      class="input"
      bind:value={draftName}
      use:focusAndSelect
      aria-label="Run name"
      placeholder={run.id}
    />
  </label>
  <label class="field">
    <span class="label">Notes</span>
    <textarea
      class="textarea"
      bind:value={draftNotes}
      onblur={handleNotesBlur}
      aria-label="Run notes"
      placeholder="Notes about this run…"
      rows="3"
    ></textarea>
  </label>
  <div class="actions">
    <button type="submit" class="btn small" disabled={submitting}>{submitting ? 'Saving…' : 'Save'}</button>
    <button type="button" class="btn ghost small" onmousedown={(e) => e.preventDefault()} onclick={cancel}>Cancel</button>
  </div>
</form>

<style>
  .editor {
    flex: 1;
    display: flex;
    flex-direction: column;
    gap: 10px;
    padding: 12px 18px;
    background: var(--panel);
  }
  .field {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .label {
    font-size: 12px;
    font-weight: 600;
    color: var(--muted);
  }
  .input,
  .textarea {
    width: 100%;
    padding: 6px 10px;
    font: inherit;
    color: var(--fg);
    background: var(--bg);
    border: 1px solid var(--line);
    border-radius: 6px;
  }
  .input:focus,
  .textarea:focus {
    border-color: var(--accent);
    outline: none;
  }
  .textarea {
    resize: vertical;
    min-height: 64px;
  }
  .actions {
    display: flex;
    gap: 8px;
  }
</style>
