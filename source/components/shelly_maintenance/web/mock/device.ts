// A pretend device for `npm run dev`: answers the page's API so the page can
// be worked on without hardware. Never part of the build.
//
//   /__mock?d=2pm       pick a device: light, mini, 1pm, 2pm, 2pm-skipped
//                       (the Mini has no sticker code: its Wi-Fi network is open;
//                       it alone offers the device type)
//   /__mock/press       the 3 button presses that save a waiting change
//   /__mock/offline     stop answering for 10 s (a restart)

import type { Plugin } from 'vite';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { readFileSync } from 'node:fs';

type Dev = 'light' | 'mini' | '1pm' | '2pm' | '2pm-skipped';

const DEVICES: Record<Dev, { hw: string; variant: string; version: string; relays: number; meter: boolean; skipped?: boolean }> = {
  light: { hw: 'S1G4', variant: 'light', version: '2.2.0', relays: 1, meter: false },
  mini: { hw: 'Mini1G4', variant: 'outlet', version: '1.1.0', relays: 1, meter: false },
  '1pm': { hw: 'S1PMG4', variant: 'outlet', version: '1.0.2', relays: 1, meter: true },
  '2pm': { hw: 'S2PMG4', variant: 'light', version: '1.0.1', relays: 2, meter: true },
  '2pm-skipped': { hw: 'S2PMG4', variant: 'light', version: '1.0.1', relays: 2, meter: false, skipped: true },
};

// A made-up code, never a real device's.
const STICKER = '2058-491-0137';

let dev: Dev = (process.env.MOCK as Dev) in DEVICES ? (process.env.MOCK as Dev) : '2pm';
let relays = [true, false];
let modes = ['toggle', 'follow'];
let code = { text: STICKER, source: 'sticker', new: false };
let custom = false;
let pending: null | { kind: string; value: string; ch: number; until: number; sticker: boolean; apply: () => void } = null;
let offlineUntil = 0;
// The owner's own Wi-Fi mode password, and what this boot's network uses.
let wifiOwn = '';
let wifiNow: string | null = null;

function wifiNext() {
  return wifiOwn ? 'own' : dev === 'mini' ? 'open' : 'sticker';
}
let saves = 0;
// The Mini's device type: this boot's, and the saved one.
const profileNow = 'outlet';
let profileNext = 'outlet';
let outletMode = 'toggle';

function state() {
  const d = DEVICES[dev];
  if (pending && Date.now() > pending.until) pending = null;
  return {
    device: { hw: d.hw, variant: d.variant, version: d.version, mac: '02:00:00:12:34:56' },
    caps: { relays: d.relays, switch_modes: true, meter: d.meter, meter_skipped: !!d.skipped, diag: true, hw: true, profiles: dev === 'mini' },
    relays: relays.slice(0, d.relays),
    switch: modes.slice(0, d.relays),
    code: dev === 'mini' ? { text: '3184-207-5562', source: 'generated', new: false, sticker: null } : { ...code, sticker: STICKER },
    profile: dev === 'mini' ? { now: profileNow, next: profileNext } : { now: 'outlet', next: 'outlet' },
    wifi: { ssid: `Shelly${d.hw.replace(/^S/, '').replace('Mini1G4', '1MiniG4')}-020000123456`, now: (wifiNow ??= wifiNext()), next: wifiNext() },
    saves,
    presses: presses(),
    pending: pending && {
      kind: pending.kind,
      value: pending.value,
      ch: pending.ch,
      secs: Math.max(0, Math.round((pending.until - Date.now()) / 1000)),
      sticker: pending.sticker,
    },
  };
}

function meter() {
  const d = DEVICES[dev];
  const jitter = () => Math.random() * 2 - 1;
  return {
    chip: d.relays > 1 ? 'ADE7953' : 'BL0942',
    valid: true,
    steady: true,
    v: 120.4 + jitter() * 0.3,
    hz: 60.0,
    raw_v: 1834221,
    cal: { v: 1, i: [1, 1], p: [custom ? 1.0123 : 1, 1], custom },
    ch: relays.slice(0, d.relays).map((on, i) => ({
      on,
      a: on ? 3.31 + jitter() * 0.02 : 0,
      w: on ? (i ? 60 : 389) + jitter() * 2 : 0,
      wh: i ? 1820 : 41250,
      raw_i: on ? 210331 : 12,
      raw_p: on ? 990120 : 0,
    })),
  };
}

const hw = {
  live: { relay: [true, false], relay_pin: [true, false], sw_pin: [false, true], button_pin: true, ntc_c: 31.2, ntc_mv: 1490, ntc_raw: 1850, die_c: 38.5 },
  last_boot: {
    boots: 14, reset_reason: 3, uptime_s: 86400, heap_free: 92160, heap_min: 61440, sw_edges: [4, 0],
    ntc_c: 33.1, die_c: 40.2, overtemp: false, meter: { state: 'running', regs: { '0x702': '0x0072' }, live_reads: 4120 },
  },
};

function body(req: IncomingMessage): Promise<any> {
  return new Promise((resolve) => {
    let t = '';
    req.on('data', (c) => (t += c));
    req.on('end', () => {
      try { resolve(JSON.parse(t || '{}')); } catch { resolve({}); }
    });
  });
}

function json(res: ServerResponse, v: unknown, status = 200) {
  res.statusCode = status;
  res.setHeader('Content-Type', 'application/json');
  res.end(JSON.stringify(v));
}

const ok = { ok: true, msg: '' };

// Like the device: saved at once behind a password, after 3 presses on an
// open network or for the Wi-Fi mode password.
function presses() {
  return (wifiNow ??= wifiNext()) === 'open';
}

function stage(kind: string, value: string, ch: number, apply: () => void, sticker = false) {
  if (kind !== 'wifi' && !presses()) {
    apply();
    saves++;
    pending = null;
    return;
  }
  pending = { kind, value, ch, until: Date.now() + 120000, sticker, apply };
}

export function mockDevice(): Plugin {
  return {
    name: 'shelly-mock-device',
    apply: 'serve',
    configureServer(server) {
      server.middlewares.use(handle);
    },
    // `npm run build && npx vite preview`: the built page against the same pretend device.
    configurePreviewServer(server) {
      server.middlewares.use(handle);
    },
  };
}

async function handle(req: IncomingMessage, res: ServerResponse, next: () => void) {
  const url = new URL(req.url ?? '/', 'http://x');
  const p = url.pathname;
  if (p === '/__mock') {
    const d = url.searchParams.get('d') as Dev;
    if (d in DEVICES) dev = d;
    wifiNow = null;
    res.statusCode = 302;
    res.setHeader('Location', '/');
    return res.end();
  }
  if (p === '/__mock/press') {
    if (pending) saves++;
    pending?.apply();
    pending = null;
    return json(res, ok);
  }
  if (p === '/__mock/offline') {
    offlineUntil = Date.now() + 10000;
    return json(res, ok);
  }
  if (p.startsWith('/api/') && Date.now() < offlineUntil) {
    req.socket.destroy();
    return;
  }
  if (p === '/recovery') {
    res.setHeader('Content-Type', 'text/html');
    return res.end(readFileSync(new URL('../../recovery.html', import.meta.url)));
  }
  if (p === '/api/state') return json(res, state());
  if (p === '/api/meter') return DEVICES[dev].meter ? json(res, meter()) : json(res, { ok: false, msg: 'This device has no meter' }, 404);
  if (p === '/api/hw') return json(res, hw);
  if (p === '/qr.svg') {
    res.setHeader('Content-Type', 'image/svg+xml');
    return res.end('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 29 29"><rect width="29" height="29" fill="#fff"/><path d="M4 4h7v7H4zM18 4h7v7h-7zM4 18h7v7H4zM13 13h3v3h-3zM18 18h2v2h-2zM22 20h3v5h-3z"/></svg>');
  }
  if (p === '/diag.txt') {
    res.setHeader('Content-Type', 'text/plain');
    return res.end('Diagnostics log (mock)\nboot 14: Matter up\n');
  }
  if (p === '/update' && req.method === 'POST') {
    let n = 0;
    req.on('data', (c) => (n += c.length));
    req.on('end', () =>
      json(res, n < 1000
        ? { ok: false, msg: 'That is not a firmware file for this device' }
        : { ok: true, msg: 'Installed light version 1.0.2. The device restarts now and confirms the update after a minute' }),
    );
    return;
  }
  if (p === '/api/action' && req.method === 'POST') {
    const b = await body(req);
    if (b.action === 'toggle') {
      relays[b.ch - 1] = !relays[b.ch - 1];
      return json(res, ok);
    }
    if (b.action === 'diag.clear' || b.action === 'exit') return json(res, ok);
    return json(res, { ok: false, msg: 'Unknown action' }, 400);
  }
  if (p === '/api/set' && req.method === 'POST') {
    const b = await body(req);
    switch (b.key) {
      case 'code': {
        const digits = String(b.value).replace(/[\s-]/g, '');
        if (!/^\d{11}$/.test(digits)) return json(res, { ok: false, msg: 'Enter the 11-digit code, like 1234-567-8901' }, 400);
        const text = `${digits.slice(0, 4)}-${digits.slice(4, 7)}-${digits.slice(7)}`;
        const back = text === STICKER;
        stage('code', text, 0, () => (code = { text, source: back ? 'sticker' : 'yours', new: false }), back);
        return json(res, ok);
      }
      case 'profile':
        if (dev !== 'mini' || !['outlet', 'opener'].includes(b.value))
          return json(res, { ok: false, msg: 'This device has no device type to choose, or that type is unknown' }, 400);
        stage('profile', b.value, 0, () => {
          // Like the device: opener sets Push button, outlet restores the mode it had.
          if (b.value !== profileNext) {
            if (b.value === 'opener') {
              outletMode = modes[0];
              modes[0] = 'push';
            } else {
              modes[0] = outletMode;
            }
          }
          profileNext = b.value;
        });
        return json(res, ok);
      case 'code.random':
        stage('code', '3184-207-5562', 0, () => (code = { text: '3184-207-5562', source: 'generated', new: true }));
        return json(res, ok);
      case 'switch.mode':
        stage('switch', b.value, b.ch, () => (modes[b.ch - 1] = b.value));
        return json(res, ok);
      case 'cal':
        stage('cal', 'new', b.ch, () => (custom = true));
        return json(res, ok);
      case 'cal.reset':
        stage('cal', 'default', 1, () => (custom = false));
        return json(res, ok);
      case 'wifi.password': {
        const v = String(b.value ?? '');
        if (!/^[\x20-\x7e]{8,63}$/.test(v))
          return json(res, { ok: false, msg: 'Use 8 to 63 characters: letters, digits, spaces and symbols, without accents or emoji' }, 400);
        stage('wifi', 'own', 0, () => (wifiOwn = v));
        return json(res, ok);
      }
      case 'wifi.password.default':
        stage('wifi', 'default', 0, () => (wifiOwn = ''));
        return json(res, ok);
      case 'cancel':
        pending = null;
        return json(res, ok);
    }
    return json(res, { ok: false, msg: 'Unknown setting' }, 400);
  }
  next();
}
