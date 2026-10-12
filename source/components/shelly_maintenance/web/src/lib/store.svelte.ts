// What the page knows about the device, refreshed by polling: the state
// every 2 s, the meter and hardware data only while a tab shows them.

import { getHw, getMeter, getState, MODE_NAMES, PROFILE_NAMES, type Hw, type Meter, type Pending, type State } from './api';

export type MsgKind = '' | 'good' | 'bad';
export interface Msg {
  text: string;
  kind: MsgKind;
}

export type TabId = 'home' | 'pairing' | 'switch' | 'meter' | 'update' | 'diag';

export interface Tab {
  id: TabId;
  label: string;
  shown: (s: State) => boolean;
}

export const TABS: Tab[] = [
  { id: 'home', label: 'Home', shown: () => true },
  { id: 'pairing', label: 'Pairing', shown: () => true },
  { id: 'switch', label: 'Switch', shown: (s) => s.caps.switch_modes || s.caps.profiles },
  { id: 'meter', label: 'Meter', shown: (s) => s.caps.meter || s.caps.meter_skipped },
  { id: 'update', label: 'Update', shown: () => true },
  { id: 'diag', label: 'Diagnostics', shown: (s) => s.caps.diag || s.caps.hw },
];

const STATE_MS = 2000;
const METER_MS = 2000;
const HW_MS = 10000;

export const app = $state({
  state: null as State | null,
  meter: null as Meter | null,
  hw: null as Hw | null,
  tab: 'home' as TabId,
  // No reply to the last few polls: the device restarted or the phone left its network.
  offline: false,
  // An upload is running: polling pauses so it has the device to itself.
  busy: false,
  // Set once Exit or an install restarts the device: the page stops polling.
  done: '' as '' | 'exit' | 'installed',
  // The firmware upload, kept here so switching tabs doesn't lose it. pct -1: none.
  up: { pct: -1, msg: '', kind: '' as MsgKind },
  msgs: { code: blank(), switch1: blank(), switch2: blank(), cal: blank(), relay: blank(), wifi: blank(), profile: blank() } as Record<string, Msg>,
});

function blank(): Msg {
  return { text: '', kind: '' };
}

export function say(slot: string, text: string, kind: MsgKind = '') {
  app.msgs[slot] = { text, kind };
}

// ---- Tabs, from the address's #hash, so Back works and a reload keeps the tab

export function tabShown(id: TabId): boolean {
  const t = TABS.find((x) => x.id === id);
  return !!t && (!app.state || t.shown(app.state));
}

function fromHash() {
  const id = location.hash.slice(1) as TabId;
  app.tab = tabShown(id) ? id : 'home';
  kick();
}

// ---- A change saved, at once or after 3 presses: say whether it was

let waiting: (Pending & { cal: string; saves: number; atOnce?: boolean }) | null = null;

// Before a set: the saves count and calibration as they are now, so a poll
// that lands while the request is out can't hide the save.
export function beforeSave() {
  return { saves: app.state?.saves ?? 0, cal: app.meter ? JSON.stringify(app.meter.cal) : '' };
}

// After a set the device accepted: if it saved at once (no presses needed),
// report it from the next state the way a pressed change is reported. A
// code's value is left empty: the saves count tells.
export function expectSaved(before: ReturnType<typeof beforeSave>, kind: Pending['kind'], value = '', ch = 1) {
  const s = app.state;
  if (!s || s.presses || kind === 'wifi') return;
  waiting = { kind, value, ch, secs: 0, sticker: false, cal: before.cal, saves: before.saves, atOnce: true };
}

function settle(s: State) {
  const p = s.pending;
  if (p) {
    waiting = { ...p, cal: app.meter ? JSON.stringify(app.meter.cal) : '', saves: s.saves };
    return;
  }
  if (!waiting) return;
  // A state fetched before the save landed: wait for the next one.
  if (waiting.atOnce && s.saves === waiting.saves) return;
  const w = waiting;
  waiting = null;
  if (w.kind === 'code') {
    if (w.value ? s.code.text === w.value : s.saves !== w.saves) say('code', `Saved. Pair with ${s.code.text} once you exit.`, 'good');
    else say('code', `The change was not saved. The code is still ${s.code.text}.`, 'bad');
  } else if (w.kind === 'switch') {
    const now = s.switch[w.ch - 1];
    const slot = w.ch === 2 ? 'switch2' : 'switch1';
    if (now === w.value) say(slot, `Saved: ${MODE_NAMES[now]}.`, 'good');
    else say(slot, `The change was not saved. The switch is still ${MODE_NAMES[now] ?? now}.`, 'bad');
  } else if (w.kind === 'profile') {
    const next = s.profile.next;
    // Saving a type also sets the wall switch mode that suits it.
    const sw = s.caps.switch_modes ? ` The wall switch is now ${MODE_NAMES[s.switch[0]] ?? s.switch[0]}.` : '';
    if (next === w.value) say('profile', `Saved: ${PROFILE_NAMES[next]}, once the device is back in normal operation.${sw}`, 'good');
    else say('profile', `The change was not saved. It is still ${PROFILE_NAMES[next] ?? next}.`, 'bad');
  } else if (w.kind === 'wifi') {
    if (s.saves === w.saves) say('wifi', 'The change was not saved.', 'bad');
    else say('wifi', 'Saved. It applies the next time Wi-Fi mode starts.', 'good');
  } else {
    getMeter()
      .then((m) => {
        app.meter = m;
        if (JSON.stringify(m.cal) === w.cal) say('cal', 'The calibration was not saved.', 'bad');
        else say('cal', m.cal.custom ? 'Saved: the new calibration is in use.' : 'Saved: back to the built-in calibration.', 'good');
      })
      .catch(() => {});
  }
}

// ---- Polling

let misses = 0;
let lastMeter = 0;
let lastHw = 0;
let timer: ReturnType<typeof setTimeout> | undefined;

export async function refresh() {
  if (app.busy || app.done) return;
  try {
    const s = await getState();
    misses = 0;
    app.offline = false;
    app.state = s;
    if (!tabShown(app.tab)) app.tab = 'home';
    settle(s);
  } catch {
    if (++misses >= 3) app.offline = true;
    return;
  }
  const now = Date.now();
  const s = app.state;
  if (s?.caps.meter && (app.tab === 'home' || app.tab === 'meter') && now - lastMeter >= METER_MS - 100) {
    lastMeter = now;
    getMeter().then((m) => (app.meter = m)).catch(() => {});
  }
  if (s?.caps.hw && app.tab === 'diag' && now - lastHw >= HW_MS - 100) {
    lastHw = now;
    getHw().then((h) => (app.hw = h)).catch(() => {});
  }
}

// A kick while a poll is in flight starts a new chain: only the newest one
// schedules the next poll, or each kick would add another 2 s loop.
let chain = 0;
function loop() {
  clearTimeout(timer);
  const mine = ++chain;
  refresh().finally(() => {
    if (mine === chain) timer = setTimeout(loop, STATE_MS);
  });
}

// Refresh now (after an action, or on a tab change that needs other data).
export function kick() {
  lastHw = 0;
  lastMeter = 0;
  loop();
}

export function start() {
  addEventListener('hashchange', fromHash);
  fromHash();
}
