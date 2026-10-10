<script lang="ts">
  import type { Snippet } from 'svelte';

  let { title = '', children }: { title?: string; children: Snippet } = $props();
</script>

<section class="card">
  {#if title}<h2>{title}</h2>{/if}
  {@render children()}
</section>

<style>
  .card {
    background: var(--card);
    border: 1px solid var(--line);
    border-radius: var(--radius);
    box-shadow: var(--shadow);
    padding: 16px;
    margin-bottom: 14px;
  }
</style>
