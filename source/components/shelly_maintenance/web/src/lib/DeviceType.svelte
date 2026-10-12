<script lang="ts">
  import { PROFILE_NAMES, set, type Profile } from './api';
  import Message from './Message.svelte';
  import { app, beforeSave, expectSaved, kick, say } from './store.svelte';

  const prof = $derived(app.state!.profile);
  const options: { v: Profile; hint: string }[] = [
    { v: 'outlet', hint: 'The relay stays on or off until it is switched again' },
    { v: 'opener', hint: 'Each toggle closes the relay for about half a second, like pressing the opener’s wall button. Saving it sets the wall switch to Push button; going back to Outlet restores the mode you had' },
  ];

  // Follows the device until you pick another type; back to it after a save or cancel.
  let picked = $state<Profile | null>(null);
  const chosen = $derived(picked ?? prof.next);
  const waiting = $derived(app.state!.pending?.kind === 'profile');
  let wasWaiting = false;
  $effect(() => {
    if (wasWaiting && !waiting) picked = null;
    wasWaiting = waiting;
  });

  function save() {
    say('profile', '');
    if (chosen === prof.next) return;
    const before = beforeSave();
    const want = chosen;
    set({ key: 'profile', value: want }).then((r) => {
      if (!r.ok) say('profile', r.msg, 'bad');
      else expectSaved(before, 'profile', want);
      kick();
    });
  }
</script>

<p class="muted small" style="margin-top:-4px">
  Now: <b>{PROFILE_NAMES[prof.now] ?? prof.now}</b>
  {#if prof.next !== prof.now}· after you exit Wi-Fi mode: <b>{PROFILE_NAMES[prof.next] ?? prof.next}</b>{/if}
</p>
<div class="modes" role="radiogroup">
  {#each options as o (o.v)}
    <label class="mode" class:sel={chosen === o.v}>
      <input type="radio" name="profile" value={o.v} checked={chosen === o.v} onchange={() => (picked = o.v)} />
      <span><b>{PROFILE_NAMES[o.v]}</b><br /><span class="muted small">{o.hint}</span></span>
    </label>
  {/each}
</div>
<p class="muted small">
  Home Assistant keeps the same switch entity and pairing either way. For a garage door, check that the relay has
  no voltage of its own (a dry contact), and don’t wire a door sensor to SW: on a mains-powered Shelly it carries
  mains voltage.
</p>
<div class="buttons">
  <button class="primary" disabled={chosen === prof.next || waiting} onclick={save}>Save</button>
</div>
<Message name="profile" />

<style>
  .modes { display: grid; gap: 8px; }
  .mode {
    display: flex;
    gap: 10px;
    align-items: flex-start;
    padding: 10px 12px;
    border: 1px solid var(--line);
    border-radius: 10px;
    cursor: pointer;
  }
  .mode.sel { border-color: var(--accent); background: var(--accent-soft); }
  .mode input { margin-top: 4px; accent-color: var(--accent); }
</style>
