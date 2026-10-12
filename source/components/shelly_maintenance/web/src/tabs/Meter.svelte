<script lang="ts">
  import { set } from '../lib/api';
  import Card from '../lib/Card.svelte';
  import Message from '../lib/Message.svelte';
  import { app, beforeSave, expectSaved, kick, say } from '../lib/store.svelte';
  import { num } from '../lib/format';

  const s = $derived(app.state!);
  const two = $derived(s.caps.relays > 1);
  const m = $derived(app.meter);

  let ch = $state(1);
  let v = $state('');
  let a = $state('');
  let w = $state('');

  // Empty keeps that value as it is; a comma works as the decimal point.
  function parse(t: string): number | null | undefined {
    const x = t.trim().replace(',', '.');
    if (!x) return null;
    const n = Number(x);
    return Number.isFinite(n) && n > 0 ? n : undefined;
  }

  function calibrate() {
    say('cal', '');
    const pv = parse(v), pa = parse(a), pw = parse(w);
    if (pv === undefined || pa === undefined || pw === undefined) {
      say('cal', 'Enter numbers only, like 117.4.', 'bad');
      return;
    }
    if (pv === null && pa === null && pw === null) {
      say('cal', 'Enter at least one reading from your meter.', 'bad');
      return;
    }
    const before = beforeSave();
    set({ key: 'cal', ch: two ? ch : 1, v: pv, a: pa, w: pw }).then((r) => {
      if (!r.ok) say('cal', r.msg, 'bad');
      else expectSaved(before, 'cal');
      kick();
    });
  }

  function reset() {
    say('cal', '');
    const before = beforeSave();
    set({ key: 'cal.reset' }).then((r) => {
      if (!r.ok) say('cal', r.msg, 'bad');
      else expectSaved(before, 'cal');
      kick();
    });
  }

  const ratios = (i: number) =>
    m ? `voltage ×${m.cal.v.toFixed(4)}, current ×${m.cal.i[i].toFixed(4)}, power ×${m.cal.p[i].toFixed(4)}` : '';
</script>

{#if s.caps.meter_skipped}
  <Card title="Power meter">
    <p class="muted" style="margin:0">
      Off in Wi-Fi mode until the next power cycle: a Wi-Fi mode boot stopped unexpectedly while it ran. Overcurrent and
      overvoltage protection are off here too, so the relays started off; metering and protection still run in normal
      operation.
    </p>
  </Card>
{/if}

{#if s.caps.meter}
  <Card title="Live readings">
    {#if !m}
      <p class="muted" style="margin:0">Reading the meter…</p>
    {:else if !m.valid}
      <p class="muted" style="margin:0">No reading from the meter.</p>
    {:else}
      <div class="tiles two">
        <div class="tile"><div class="label">Voltage</div><div class="value num">{num(m.v, 1, 'V')}</div></div>
        <div class="tile"><div class="label">Frequency</div><div class="value num">{num(m.hz, 2, 'Hz')}</div></div>
      </div>
      {#each m.ch as c, i (i)}
        <h3>{two ? `Relay ${i + 1}` : 'Load'} <span class="muted small">· {c.on ? 'on' : 'off'}</span></h3>
        <div class="tiles">
          <div class="tile"><div class="label">Power</div><div class="value num">{num(c.w, 1, 'W')}</div></div>
          <div class="tile"><div class="label">Current</div><div class="value num">{num(c.a, 3, 'A')}</div></div>
          <div class="tile"><div class="label">Energy</div><div class="value num">{(c.wh / 1000).toFixed(2)} kWh</div></div>
        </div>
      {/each}
      <dl class="facts small" style="margin-top:14px">
        <dt>Calibration</dt>
        <dd>
          {#if m.cal.custom}
            {#each m.ch as _, i (i)}<div>{two ? `Relay ${i + 1}: ` : ''}{ratios(i)}</div>{/each}
          {:else}Built-in{/if}
        </dd>
        <dt>Raw counts</dt>
        <dd>V {m.raw_v}{#each m.ch as c, i (i)}, I{i + 1} {c.raw_i}, P{i + 1} {c.raw_p}{/each}</dd>
      </dl>
    {/if}
  </Card>

  <Card title="Calibrate">
    <p class="muted small">
      Optional: the meter comes calibrated. To match your own plug-in meter, run a steady load of at least 50 W through
      {two ? 'the relay you choose' : 'the relay'} (a heater, a kettle or an old-style bulb, not LED lights), wait a few
      seconds, and enter what your meter shows. Leave a box empty to keep that value as it is.
    </p>
    {#if two}
      <div class="seg" role="radiogroup" aria-label="Relay">
        {#each [1, 2] as n (n)}
          <label class:sel={ch === n}><input type="radio" name="calch" value={n} bind:group={ch} /> Relay {n}</label>
        {/each}
      </div>
    {/if}
    <div class="fields">
      <div><label class="field" for="calv">Volts</label><input type="text" id="calv" inputmode="decimal" autocomplete="off" placeholder="117.4" bind:value={v} /></div>
      <div><label class="field" for="cala">Amps</label><input type="text" id="cala" inputmode="decimal" autocomplete="off" placeholder="3.31" bind:value={a} /></div>
      <div><label class="field" for="calw">Watts</label><input type="text" id="calw" inputmode="decimal" autocomplete="off" placeholder="389" bind:value={w} /></div>
    </div>
    <div class="buttons">
      <button class="primary" onclick={calibrate}>Calibrate</button>
      <button onclick={reset}>Back to the built-in calibration</button>
    </div>
    {#if app.state?.presses}
      <p class="muted small" style="margin:10px 0 0">On this open network, either waits for 3 presses of the device's button before it is saved.</p>
    {/if}
    <Message name="cal" />
  </Card>
{/if}

<style>
  .tiles { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
  .tiles.two { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .tile { background: var(--field); border: 1px solid var(--line); border-radius: 10px; padding: 8px 10px; min-width: 0; }
  .label { color: var(--muted); font-size: 0.8rem; }
  .value { font-size: clamp(0.98rem, 4.2vw, 1.25rem); font-weight: 650; white-space: nowrap; }
  .fields { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 0 10px; }
  .seg { display: inline-flex; border: 1px solid var(--line); border-radius: 10px; overflow: hidden; margin-bottom: 4px; }
  .seg label { padding: 8px 14px; cursor: pointer; min-height: 40px; display: flex; align-items: center; }
  .seg label + label { border-left: 1px solid var(--line); }
  .seg label.sel { background: var(--accent-soft); color: var(--accent); font-weight: 600; }
  .seg input { position: absolute; opacity: 0; pointer-events: none; }
</style>
