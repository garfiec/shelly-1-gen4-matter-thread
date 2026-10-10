<script lang="ts">
  import { act } from '../lib/api';
  import Card from '../lib/Card.svelte';
  import { app, kick } from '../lib/store.svelte';
  import { temp } from '../lib/format';

  const s = $derived(app.state!);
  const h = $derived(app.hw);
  const RESET_REASONS: Record<number, string> = {
    1: 'power on', 3: 'software restart', 4: 'crash', 5: 'interrupt watchdog', 6: 'task watchdog', 7: 'watchdog', 9: 'brownout',
  };
  const onOff = (a?: boolean[]) => (a ?? []).map((b) => (b ? 'on' : 'off')).join(' / ');
  const hiLo = (a?: boolean[]) => (a ?? []).map((b) => (b ? 'high' : 'low')).join(' / ');

  let cleared = $state(false);
  function clear() {
    act({ action: 'diag.clear' }).then(() => {
      cleared = true;
      kick();
    });
  }
</script>

{#if s.caps.diag}
  <Card title="Diagnostics log">
    <p class="muted small">What the device recorded in normal operation, kept across restarts but not power cuts.</p>
    <div class="buttons" style="margin-top:0">
      <a class="btn" href="/diag.txt" target="_blank">Open the log</a>
      <button onclick={clear}>Clear</button>
    </div>
    {#if cleared}<div class="msg good">Cleared.</div>{/if}
  </Card>
{/if}

{#if s.caps.hw}
  <Card title="Hardware data">
    <p class="muted small">What the board read, for bringing up its support. <a href="/api/hw" target="_blank">Raw data</a></p>
    {#if !h}
      <p class="muted" style="margin:0">Reading…</p>
    {:else}
      <dl class="facts small">
        {#if h.live}
          {@const l = h.live}
          <dt>Relays now</dt><dd>{onOff(l.relay)} (pins {hiLo(l.relay_pin)})</dd>
          <dt>Switch pins now</dt><dd>{hiLo(l.sw_pin)}</dd>
          <dt>Button pin now</dt><dd>{l.button_pin ? 'high' : 'low'}</dd>
          <dt>Board sensor now</dt><dd>{temp(l.ntc_c)} ({l.ntc_mv} mV, raw {l.ntc_raw})</dd>
          <dt>Chip sensor now</dt><dd>{temp(l.die_c)}</dd>
        {/if}
        {#if h.last_boot}
          {@const b = h.last_boot}
          <dt>Last normal run</dt>
          <dd>boot {b.boots}, {RESET_REASONS[b.reset_reason] ?? `reason ${b.reset_reason}`}, up {b.uptime_s} s</dd>
          <dt>Memory</dt><dd>{Math.round(b.heap_free / 1024)} KB free, lowest {Math.round(b.heap_min / 1024)} KB</dd>
          <dt>Switch changes seen</dt><dd>{(b.sw_edges ?? []).join(' / ')}</dd>
          <dt>Temperatures</dt><dd>board {temp(b.ntc_c)}, chip {temp(b.die_c)}{b.overtemp ? ', OVER TEMPERATURE' : ''}</dd>
          <dt>Power meter</dt>
          <dd>{b.meter.state}{b.meter.error ? ` (error ${b.meter.error})` : ''}{b.meter.regs?.['0x702'] ? `, version ${b.meter.regs['0x702']}` : ''}</dd>
        {:else}
          <dt>Last normal run</dt><dd>No data (a power cut clears it)</dd>
        {/if}
      </dl>
    {/if}
  </Card>
{/if}

<style>
  .btn {
    display: inline-flex;
    align-items: center;
    min-height: 44px;
    padding: 10px 16px;
    border-radius: 10px;
    border: 1px solid var(--line);
    text-decoration: none;
    font-weight: 550;
  }
</style>
