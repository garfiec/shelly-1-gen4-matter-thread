<script lang="ts">
  import { MODE_NAMES, PROFILE_NAMES, set, type Profile, type SwitchMode } from './api';
  import { app, kick, say } from './store.svelte';

  const s = $derived(app.state);
  const p = $derived(s?.pending ?? null);
  const two = $derived((s?.caps.relays ?? 1) > 1);

  const what = $derived.by(() => {
    if (!p) return '';
    if (p.kind === 'code') return p.sticker ? 'Back to the sticker code' : `New setup code ${p.value}`;
    if (p.kind === 'wifi')
      return p.value === 'own'
        ? 'New Wi-Fi mode password'
        : s?.code.sticker
          ? 'Wi-Fi mode password back to the sticker code'
          : 'No Wi-Fi mode password: an open network';
    if (p.kind === 'profile') return `Device type: ${PROFILE_NAMES[p.value as Profile] ?? p.value}`;
    if (p.kind === 'cal')
      return p.value === 'default'
        ? 'Back to the built-in meter calibration'
        : `New meter calibration${two ? `, measured on relay ${p.ch}` : ''}`;
    return `${two ? `Wall switch ${p.ch}` : 'Wall switch'}: ${MODE_NAMES[p.value as SwitchMode] ?? p.value}`;
  });
  const when = $derived(
    p?.kind === 'code'
      ? 'The new code applies once the device is back in normal operation.'
      : p?.kind === 'profile'
        ? 'It applies once the device is back in normal operation.'
        : p?.kind === 'wifi'
          ? 'It applies the next time Wi-Fi mode starts: this connection stays as it is.'
          : 'It applies as soon as you press.',
  );
  const left = $derived(p ? `${Math.floor(p.secs / 60)}:${String(p.secs % 60).padStart(2, '0')}` : '');

  function cancel() {
    const slot =
      p?.kind === 'switch' ? (p.ch === 2 ? 'switch2' : 'switch1') : p?.kind === 'cal' || p?.kind === 'wifi' || p?.kind === 'profile' ? p.kind : 'code';
    set({ key: 'cancel' }).then((r) => {
      if (!r.ok) say(slot, r.msg, 'bad');
      kick();
    });
  }
</script>

{#if p}
  <div class="pending" role="status" aria-live="polite">
    <div class="in">
    <div class="text">
      <div class="what">{what}</div>
      <div>Press the device's button <b>3 times</b> to save · <b class="num">{left}</b></div>
      <div class="note">Nothing changes until you press. {when}</div>
    </div>
    <button onclick={cancel}>Cancel</button>
    </div>
  </div>
{/if}

<style>
  .pending {
    background: var(--warn-bg);
    color: var(--fg);
    border-top: 1px solid var(--warn-line);
    border-bottom: 1px solid var(--warn-line);
  }
  .in {
    display: flex;
    gap: 12px;
    align-items: center;
    justify-content: space-between;
    max-width: 44rem;
    margin: 0 auto;
    padding: 10px 16px;
    border-left: 5px solid var(--warn-line);
  }
  .text { min-width: 0; }
  .what { font-weight: 650; overflow-wrap: anywhere; }
  .note { color: var(--muted); font-size: 0.88rem; }
  button { flex: none; }
</style>
