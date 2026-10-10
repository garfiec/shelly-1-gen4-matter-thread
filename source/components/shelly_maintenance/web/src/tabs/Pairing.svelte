<script lang="ts">
  import { set } from '../lib/api';
  import Card from '../lib/Card.svelte';
  import Message from '../lib/Message.svelte';
  import { app, kick, say } from '../lib/store.svelte';

  const s = $derived(app.state!);
  const c = $derived(s.code);
  const source = $derived(
    c.source === 'yours'
      ? 'Your code'
      : c.source === 'sticker'
        ? 'From the Shelly sticker'
        : c.source === 'test'
          ? 'The shared test code: this device could not save its own code'
          : c.new
            ? 'Made by this device just now'
            : 'Made by this device',
  );
  const resetNote = $derived(
    c.sticker
      ? c.source === 'sticker'
        ? 'A factory reset keeps it.'
        : 'A factory reset goes back to the sticker code.'
      : c.source === 'yours'
        ? 'A factory reset forgets it.'
        : 'A factory reset keeps it.',
  );
  let qrOk = $state(true);
  let typed = $state('');

  function send(p: Promise<{ ok: boolean; msg: string }>) {
    say('code', '');
    p.then((r) => {
      if (!r.ok) say('code', r.msg, 'bad');
      kick();
    });
  }
</script>

<Card title="Matter setup code">
  <div class="pair">
    {#if qrOk}
      <!-- The QR changes only with the code: the query makes the browser fetch the new one. -->
      <img class="qr" src={`/qr.svg?c=${encodeURIComponent(c.text)}`} alt="QR code of the setup code" onerror={() => (qrOk = false)} />
    {/if}
    <div>
      <div class="code num">{c.text}</div>
      <div class="muted">{source}</div>
      {#if c.sticker && c.source !== 'sticker'}
        <div class="muted small sticker">On the Shelly sticker: <span class="num">{c.sticker}</span></div>
      {/if}
    </div>
  </div>
  <p class="muted small help">
    Scan the QR or type the code in your Matter app once you exit this mode. {resetNote}
    Anyone who joins this network can see it while the mode is on.
  </p>
</Card>

<Card title="Use another code">
  <label class="field" for="newcode">An 11-digit code. Typing the sticker's code goes back to it.</label>
  <input
    type="text"
    id="newcode"
    inputmode="numeric"
    autocomplete="off"
    placeholder="1234-567-8901"
    maxlength="30"
    bind:value={typed}
  />
  <div class="buttons">
    <button class="primary" disabled={!typed.trim()} onclick={() => send(set({ key: 'code', value: typed }))}>Use this code</button>
    <button onclick={() => send(set({ key: 'code.random' }))}>Make a new random code</button>
  </div>
  <p class="muted small" style="margin:10px 0 0">Either waits for 3 presses of the device's button before it is saved.</p>
  <Message name="code" />
</Card>

<style>
  .pair { display: flex; gap: 16px; align-items: center; flex-wrap: wrap; }
  .qr {
    width: 168px;
    height: 168px;
    image-rendering: pixelated;
    background: #fff;
    border-radius: 8px;
    padding: 6px;
    flex: none;
  }
  .code { font-size: 1.9rem; font-weight: 650; letter-spacing: 0.03em; line-height: 1.2; }
  .sticker { margin-top: 6px; }
  .help { margin: 14px 0 0; }
</style>
