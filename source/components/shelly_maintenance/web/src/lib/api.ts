// The device's JSON API in Wi-Fi mode (maintenance.cpp). Every request is
// short: the device's web server has only 4 sockets.

export type SwitchMode = 'toggle' | 'push' | 'follow' | 'disabled';
export type CodeSource = 'sticker' | 'yours' | 'generated' | 'test';

// The Wi-Fi mode network's password: the owner's own, the sticker's setup
// code, or none (a device without a sticker code).
export type WifiSecurity = 'own' | 'sticker' | 'open';

// What the relay is for, on a board that offers the choice (caps.profiles):
// an outlet that stays on or off, or a garage door opener's short pulse.
export type Profile = 'outlet' | 'opener';

export interface Pending {
  kind: 'code' | 'switch' | 'cal' | 'wifi' | 'profile';
  // code: the new code; switch: the mode; cal: "new" or "default"; wifi: "own" or "default";
  // profile: the device type
  value: string;
  ch: number; // 1-based relay, for switch and cal
  secs: number; // left to press the button 3 times
  sticker: boolean; // a code change back to the sticker's
}

export interface State {
  device: { hw: string; variant: string; version: string; mac: string };
  // What this build has: the page shows only the tabs and cards it supports.
  caps: { relays: number; switch_modes: boolean; meter: boolean; meter_skipped: boolean; diag: boolean; hw: boolean; profiles: boolean };
  relays: boolean[];
  switch: SwitchMode[];
  code: { text: string; source: CodeSource; new: boolean; sticker: string | null };
  // now: this boot's device type; next: the saved one, from the next normal boot.
  profile: { now: Profile; next: Profile };
  pending: Pending | null;
  // now: this boot's network; next: the next time Wi-Fi mode starts.
  wifi: { ssid: string; now: WifiSecurity; next: WifiSecurity };
  // Changes saved this boot, at once or with 3 presses.
  saves: number;
  // Whether a change waits for 3 presses (on an open network). The Wi-Fi
  // mode password always does.
  presses: boolean;
}

export interface MeterChannel {
  on: boolean;
  a: number | null;
  w: number | null;
  wh: number;
  raw_i: number;
  raw_p: number;
}

export interface Meter {
  chip: string;
  valid: boolean;
  steady: boolean;
  v: number | null;
  hz: number | null;
  raw_v: number;
  cal: { v: number; i: number[]; p: number[]; custom: boolean };
  ch: MeterChannel[];
}

export interface Hw {
  live?: {
    relay: boolean[];
    relay_pin: boolean[];
    sw_pin: boolean[];
    button_pin: boolean;
    ntc_c: number | null;
    ntc_mv: number;
    ntc_raw: number;
    die_c: number | null;
  };
  last_boot?: {
    boots: number;
    reset_reason: number;
    uptime_s: number;
    heap_free: number;
    heap_min: number;
    sw_edges: number[];
    ntc_c: number | null;
    die_c: number | null;
    overtemp: boolean;
    meter: { state: string; error?: number; regs?: Record<string, string>; live_reads?: number };
  } | null;
}

export interface Result {
  ok: boolean;
  msg: string;
}

// Settings: saved at once, or after 3 presses of the button on an open
// network or for the Wi-Fi mode password (B-26).
export type Setting =
  | { key: 'code'; value: string }
  | { key: 'code.random' }
  | { key: 'switch.mode'; ch: number; value: SwitchMode }
  | { key: 'cal'; ch: number; v: number | null; a: number | null; w: number | null }
  | { key: 'cal.reset' }
  | { key: 'wifi.password'; value: string }
  | { key: 'wifi.password.default' }
  | { key: 'profile'; value: Profile }
  | { key: 'cancel' };

// Things that happen at once.
export type Action = { action: 'toggle'; ch: number } | { action: 'diag.clear' } | { action: 'exit' };

const NO_REPLY: Result = { ok: false, msg: 'No response from the device' };

// A request to a device that went away can hang instead of failing: give up
// after 5 s so the page notices it's offline.
async function getJson<T>(url: string): Promise<T> {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), 5000);
  const r = await fetch(url, { cache: 'no-store', signal: ctl.signal }).finally(() => clearTimeout(t));
  if (!r.ok) throw new Error(`${url}: ${r.status}`);
  return (await r.json()) as T;
}

async function postJson(url: string, body: unknown): Promise<Result> {
  try {
    const r = await fetch(url, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    return (await r.json()) as Result;
  } catch {
    return NO_REPLY;
  }
}

export const getState = () => getJson<State>('/api/state');
export const getMeter = () => getJson<Meter>('/api/meter');
export const getHw = () => getJson<Hw>('/api/hw');
export const set = (s: Setting) => postJson('/api/set', s);
export const act = (a: Action) => postJson('/api/action', a);

// The firmware upload: the raw .ota file as the body, unchanged from the
// first page (and the same as /recovery uses).
export function upload(file: File, allowOlder: boolean, progress: (pct: number) => void): Promise<Result> {
  return new Promise((resolve) => {
    const x = new XMLHttpRequest();
    x.open('POST', '/update' + (allowOlder ? '?allow_older=1' : ''));
    x.upload.onprogress = (e) => {
      if (e.lengthComputable) progress(Math.round((100 * e.loaded) / e.total));
    };
    x.onload = () => {
      try {
        resolve(JSON.parse(x.responseText) as Result);
      } catch {
        resolve({ ok: false, msg: 'Unexpected reply from the device' });
      }
    };
    x.onerror = () => resolve({ ok: false, msg: 'The upload was interrupted. Nothing was installed.' });
    x.send(file);
  });
}

export const MODEL_NAMES: Record<string, string> = {
  S1G4: 'Shelly 1 Gen4',
  Mini1G4: 'Shelly 1 Mini Gen4',
  S1PMG4: 'Shelly 1PM Gen4',
  S2PMG4: 'Shelly 2PM Gen4',
};

export const PROFILE_NAMES: Record<Profile, string> = {
  outlet: 'Outlet',
  opener: 'Garage door opener',
};

export const MODE_NAMES: Record<SwitchMode, string> = {
  toggle: 'Toggle',
  push: 'Push button',
  follow: 'Follow',
  disabled: 'Disabled',
};
