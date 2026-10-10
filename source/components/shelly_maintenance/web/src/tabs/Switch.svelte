<script lang="ts">
  import Card from '../lib/Card.svelte';
  import DeviceType from '../lib/DeviceType.svelte';
  import SwitchMode from '../lib/SwitchMode.svelte';
  import { app } from '../lib/store.svelte';

  const s = $derived(app.state!);
  const two = $derived(s.caps.relays > 1);
</script>

{#if s.caps.profiles}
  <Card title="Device type">
    <DeviceType />
  </Card>
{/if}
{#if s.caps.switch_modes}
  {#each s.switch as _, i (i)}
    <Card title={two ? `Wall switch ${i + 1} (SW${i + 1})` : 'Wall switch'}>
      <SwitchMode ch={i + 1} {two} />
    </Card>
  {/each}
{/if}
<p class="muted small">
  A change waits for 3 presses of the device's button.{s.caps.switch_modes ? ' A switch mode then applies at once.' : ''}
</p>
