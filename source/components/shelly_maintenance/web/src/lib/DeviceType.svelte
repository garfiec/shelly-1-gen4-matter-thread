<script lang="ts">
  import { PROFILE_NAMES, set, type Profile } from './api';
  import Message from './Message.svelte';
  import { app, beforeSave, expectSaved, kick, say } from './store.svelte';

  const prof = $derived(app.state!.profile);
  const contact = $derived(prof.opener_contact);
  const HINTS: Record<Profile, string> = {
    light: 'The relay stays on or off until it is switched again. Your app shows a light',
    outlet: 'The relay stays on or off until it is switched again. Your app shows an outlet',
    opener: '',
    'light-switch': 'The wall switch controls other Matter devices through a binding; the button and your app switch the relay',
  };
  const options = $derived(
    prof.types.map((t) => ({
      ...t,
      hint:
        t.v !== 'opener'
          ? HINTS[t.v]
          : contact
            ? 'Each toggle closes the relay for about half a second, like pressing the opener’s wall button. SW reads a door contact sensor, shown in your app'
            : 'Each toggle closes the relay for about half a second, like pressing the opener’s wall button. Saving it sets the wall switch to Push button; going back restores the mode you had',
    })),
  );

  // Follows the device until you pick another type; back to it after a save or cancel.
  let picked = $state<Profile | null>(null);
  const chosen = $derived(picked ?? prof.next);
  const waiting = $derived(app.state!.pending?.kind === 'profile');
  // Choosing this type gives the device other Matter endpoints: it has to be
  // paired again, and the page must say so before saving.
  const repair = $derived(options.find((o) => o.v === chosen)?.repair ?? false);
  let understood = $state(false);
  let wasWaiting = false;
  $effect(() => {
    if (wasWaiting && !waiting) picked = null;
    wasWaiting = waiting;
  });
  $effect(() => {
    chosen;
    understood = false;
  });

  function save() {
    say('profile', '');
    if (chosen === prof.next || (repair && !understood)) return;
    const before = beforeSave();
    const want = chosen;
    set({ key: 'profile', value: want, repair }).then((r) => {
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
{#if repair && chosen !== prof.next}
  <div class="repair" role="note">
    <strong>This type means pairing the device again.</strong>
    It has other Matter endpoints, so your apps' pairing won't fit it. When you exit Wi-Fi mode, the device clears its
    pairings and its Thread network, and restarts ready to pair. Remove it from your apps first, then pair it again with
    the code on the Pairing tab. Your setup code, Wi-Fi mode password, wall switch modes and meter calibration are kept.
    <label class="ok"><input type="checkbox" bind:checked={understood} /> I understand the device must be paired again</label>
  </div>
{:else if options.some((o) => o.v === 'opener')}
  <p class="muted small">
    {#if contact}
      For a garage door, check that the relay has no voltage of its own (a dry contact). The door sensor on SW must be a
      dry contact too, wired as for the Shelly 1 Gen4 Opener.
    {:else}
      For a garage door, check that the relay has no voltage of its own (a dry contact), and don’t wire a door sensor to
      SW: on a mains-powered Shelly it carries mains voltage.
    {/if}
  </p>
{/if}
<div class="buttons">
  <button class="primary" disabled={chosen === prof.next || waiting || (repair && !understood)} onclick={save}>Save</button>
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
  .repair {
    background: var(--warn-bg);
    border: 1px solid var(--warn-line);
    border-left: 5px solid var(--warn-line);
    border-radius: 8px;
    padding: 10px 14px;
    margin: 12px 0 0;
    line-height: 1.45;
  }
  .repair strong { display: block; margin-bottom: 2px; }
  .ok { display: flex; gap: 8px; align-items: center; margin-top: 8px; font-weight: 600; cursor: pointer; }
  .ok input { accent-color: var(--accent); }
</style>
