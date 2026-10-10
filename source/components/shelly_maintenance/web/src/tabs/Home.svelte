<script lang="ts">
  import { act } from '../lib/api';
  import Card from '../lib/Card.svelte';
  import Message from '../lib/Message.svelte';
  import { app, kick, say } from '../lib/store.svelte';
  import { num } from '../lib/format';

  const s = $derived(app.state!);
  const two = $derived(s.caps.relays > 1);
  const m = $derived(s.caps.meter && app.meter?.valid ? app.meter : null);

  function toggle(ch: number) {
    act({ action: 'toggle', ch }).then((r) => {
      say('relay', r.ok ? '' : r.msg, r.ok ? '' : 'bad');
      kick();
    });
  }
</script>

<Card title={two ? 'Relays' : 'Relay'}>
  {#each s.relays as on, i (i)}
    <div class="relay">
      <div class="dot" class:on aria-hidden="true"></div>
      <div class="name">
        {#if two}
          <div>Relay {i + 1}</div>
          <div class="state muted small">
            {on ? 'On' : 'Off'}{#if m}{' · '}<span class="num">{num(m.ch[i]?.w, 1, 'W')}</span>{/if}
          </div>
        {:else}
          <div>{on ? 'On' : 'Off'}</div>
          {#if m}<div class="state muted small num">{num(m.ch[i]?.w, 1, 'W')}</div>{/if}
        {/if}
      </div>
      <button onclick={() => toggle(i + 1)} aria-label={`Toggle ${two ? `relay ${i + 1}` : 'the relay'}`}>Toggle</button>
    </div>
  {/each}
  {#if m}
    <div class="muted small line num">{num(m.v, 1, 'V')} · {num(m.hz, 2, 'Hz')}</div>
  {:else if s.caps.meter_skipped}
    <div class="muted small line">The meter is off in this Wi-Fi mode boot. <a href="#meter">Why</a></div>
  {/if}
  <Message name="relay" />
</Card>

<Card title="Device">
  <dl class="facts">
    <dt>Model</dt><dd>{s.device.hw}</dd>
    <dt>Variant</dt><dd>{s.device.variant}</dd>
    <dt>Firmware</dt><dd>{s.device.version}</dd>
    <dt>MAC</dt><dd>{s.device.mac}</dd>
  </dl>
</Card>

<Card title="About Wi-Fi mode">
  <p class="muted small">
    Matter and Thread are off while the device is in this mode. It goes back to normal after an update, when you
    press <b>Exit</b>, on another 5 s hold of its button, or after 10 minutes with this page closed.
  </p>
  <p class="muted small" style="margin:0">
    This network is open: anyone nearby can use this page while the mode is on.
  </p>
</Card>

<style>
  .relay { display: flex; align-items: center; gap: 12px; padding: 6px 0; }
  .relay + .relay { border-top: 1px solid var(--line); padding-top: 10px; margin-top: 4px; }
  .dot { width: 12px; height: 12px; border-radius: 50%; background: var(--line); flex: none; }
  .dot.on { background: var(--on); box-shadow: 0 0 0 4px color-mix(in srgb, var(--on) 22%, transparent); }
  .name { flex: 1; min-width: 0; font-weight: 600; }
  .state { font-weight: 400; }
  .line { margin-top: 10px; }
</style>
