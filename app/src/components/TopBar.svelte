<script lang="ts">
  import type { Snippet } from 'svelte';

  interface Props {
    title?: string;
    onBack?: () => void;
    backLabel?: string;
    children?: Snippet; // buttons on the right
  }
  let { title = '', onBack, backLabel = 'Back', children }: Props = $props();
</script>

<header>
  <span class="logo">malgrind</span>
  {#if title}<span class="title">{title}</span>{/if}
  <span class="spacer"></span>
  {@render children?.()}
  {#if onBack}<button class="btn ghost small" onclick={onBack}>{backLabel}</button>{/if}
</header>

<style>
  header { display: flex; align-items: center; gap: 12px; padding: 10px 18px; border-bottom: 1px solid var(--line); background: var(--panel); }
  .logo { font-weight: 700; color: var(--accent); letter-spacing: 0.3px; }
  .title { font-weight: 600; }
  .spacer { flex: 1; }
</style>
