<script lang="ts">
  import { app } from './store.svelte';

  let { name }: { name: string } = $props();
  const m = $derived(app.msgs[name]);
</script>

<div class="msg {m?.kind ?? ''}" role="status" aria-live="polite">{m?.text ?? ''}</div>
