<script lang="ts">
  import { set, type WifiSecurity } from './api';
  import Card from './Card.svelte';
  import Message from './Message.svelte';
  import { app, kick, say } from './store.svelte';

  const s = $derived(app.state!);
  const w = $derived(s.wifi);
  const sticker = $derived(s.code.sticker);
  const digits = $derived(sticker ? sticker.replace(/-/g, '') : '');

  let typed = $state('');
  let show = $state(false);
  // What WPA2 and WPA3 accept: 8 to 63 printable ASCII characters.
  const valid = $derived(/^[\x20-\x7e]{8,63}$/.test(typed));

  function describe(sec: WifiSecurity): string {
    if (sec === 'own') return 'your own password';
    if (sec === 'sticker') return 'the setup code on the Shelly sticker';
    return 'none: an open network that anyone nearby can join';
  }

  function send(p: Promise<{ ok: boolean; msg: string }>) {
    say('wifi', '');
    p.then((r) => {
      if (r.ok) typed = '';
      else say('wifi', r.msg, 'bad');
      kick();
    });
  }
</script>

<Card title="Wi-Fi mode password">
  <dl class="facts">
    <dt>Network</dt><dd>{w.ssid}</dd>
    <dt>Now</dt><dd>{describe(w.now)}</dd>
    {#if w.next !== w.now}<dt>Next time</dt><dd>{describe(w.next)}</dd>{/if}
  </dl>
  {#if w.next === 'sticker'}
    <p class="muted small">
      The password is the sticker's 11 digits without dashes: <b class="num">{digits}</b>. Note it before you exit.
    </p>
  {:else if w.next === 'open'}
    <p class="muted small">
      This device has no setup code from a Shelly sticker, so the network stays open unless you set a password.
    </p>
  {/if}

  <label class="field" for="wifipw">Your own password: 8 to 63 characters, without accents or emoji.</label>
  <input
    type={show ? 'text' : 'password'}
    id="wifipw"
    autocomplete="new-password"
    autocapitalize="off"
    spellcheck="false"
    maxlength="63"
    bind:value={typed}
  />
  <label class="check">
    <input type="checkbox" bind:checked={show} />
    <span>Show it</span>
  </label>
  <div class="buttons">
    <button class="primary" disabled={!valid} onclick={() => send(set({ key: 'wifi.password', value: typed }))}>
      Use this password
    </button>
    {#if w.next === 'own'}
      <button onclick={() => send(set({ key: 'wifi.password.default' }))}>
        {sticker ? 'Use the sticker code instead' : 'Remove it: open network'}
      </button>
    {/if}
  </div>
  <p class="muted small" style="margin:10px 0 0">
    Saved after 3 presses of the device's button; it applies the next time Wi-Fi mode starts. A factory reset forgets
    your own password and goes back to {sticker ? 'the sticker code' : 'an open network'}.
  </p>
  <Message name="wifi" />
</Card>

<style>
  .check { display: flex; gap: 10px; align-items: center; color: var(--muted); margin: 8px 0 4px; }
  .check input { width: 18px; height: 18px; accent-color: var(--accent); }
</style>
