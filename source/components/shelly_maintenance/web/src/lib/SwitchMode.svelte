<script lang="ts">
  import { MODE_NAMES, set, type SwitchMode } from './api';
  import Message from './Message.svelte';
  import { app, kick, say } from './store.svelte';

  let { ch, two }: { ch: number; two: boolean } = $props();

  const now = $derived(app.state!.switch[ch - 1]);
  const relay = $derived(two ? `relay ${ch}` : 'the relay');
  const slot = $derived(ch === 2 ? 'switch2' : 'switch1');
  // In opener mode (from the next boot, if just saved) a toggle is a pulse.
  const opener = $derived(app.state!.profile?.next === 'opener');
  const options = $derived<{ v: SwitchMode; hint: string }[]>(
    opener
      ? [
          { v: 'toggle', hint: 'An ordinary on/off switch: any flip triggers the opener' },
          { v: 'push', hint: 'A button that springs back: each press triggers the opener once' },
          { v: 'follow', hint: 'Switching on triggers the opener; switching off does nothing' },
          { v: 'disabled', hint: 'The switch does nothing' },
        ]
      : [
          { v: 'toggle', hint: `An ordinary on/off switch: any flip toggles ${relay}` },
          { v: 'push', hint: `A button that springs back: each press toggles ${relay} once` },
          { v: 'follow', hint: `${two ? `Relay ${ch}` : 'The relay'} matches the switch: switch on, relay on` },
          { v: 'disabled', hint: 'The switch does nothing' },
        ],
  );

  // Follows the device until you pick another mode; back to it after a save or cancel.
  let picked = $state<SwitchMode | null>(null);
  const chosen = $derived(picked ?? now);
  const waiting = $derived(app.state!.pending?.kind === 'switch' && app.state!.pending.ch === ch);
  let wasWaiting = false;
  $effect(() => {
    if (wasWaiting && !waiting) picked = null;
    wasWaiting = waiting;
  });

  function save() {
    say(slot, '');
    if (chosen === now) return;
    set({ key: 'switch.mode', ch, value: chosen }).then((r) => {
      if (!r.ok) say(slot, r.msg, 'bad');
      kick();
    });
  }
</script>

<p class="muted small" style="margin-top:-4px">Now: <b>{MODE_NAMES[now] ?? now}</b></p>
<div class="modes" role="radiogroup">
  {#each options as o (o.v)}
    <label class="mode" class:sel={chosen === o.v}>
      <input type="radio" name={`sw${ch}`} value={o.v} checked={chosen === o.v} onchange={() => (picked = o.v)} />
      <span><b>{MODE_NAMES[o.v]}</b><br /><span class="muted small">{o.hint}</span></span>
    </label>
  {/each}
</div>
<div class="buttons">
  <button class="primary" disabled={chosen === now || waiting} onclick={save}>Save</button>
</div>
<Message name={slot} />

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
