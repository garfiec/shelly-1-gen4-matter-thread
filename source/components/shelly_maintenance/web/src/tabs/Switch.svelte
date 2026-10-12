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
    <Card title={two ? `Wall switch ${i + 1} (SW${i + 1})` : s.profile?.next === 'opener' && !s.profile.opener_contact ? 'Wall switch (garage door opener)' : 'Wall switch'}>
      <SwitchMode ch={i + 1} {two} />
    </Card>
  {/each}
{/if}
<p class="muted small">
  {s.presses ? "On this open network, a change waits for 3 presses of the device's button." : 'A change is saved when you press Save.'}
  {s.caps.switch_modes ? 'A switch mode applies at once.' : ''}
</p>
