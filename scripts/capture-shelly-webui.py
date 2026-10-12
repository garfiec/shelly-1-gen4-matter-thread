#!/usr/bin/env python3
#
# Copyright 2026 AUTOMATOUS.IO
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
"""
Capture a stock Shelly Gen2+ device's web UI and its read-only RPC data,
to catalog its features before the stock firmware is replaced.

READ ONLY. It calls only RPC methods whose verb is Get... or List...
(GetStatus, ListMethods; never Set, Reset, Update, Reboot, Scan, ...), and downloads
the files the web UI is made of. It changes nothing on the device.

Standard library only, so it runs offline on the Shelly's own Wi-Fi:

    python3 scripts/capture-shelly-webui.py              # device at 192.168.33.1
    python3 scripts/capture-shelly-webui.py 192.168.1.50 # device on your network

It writes shelly-capture-<id>-<time>/ and a .zip of it next to where you
run it: ui/ (the web UI files), rpc/ (one JSON file per call) and
summary.json (what was captured and what failed).
"""
import gzip
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from collections import deque

HOST = re.sub(r"^https?://", "", sys.argv[1]).rstrip("/") if len(sys.argv) > 1 else "192.168.33.1"
BASE = f"http://{HOST}"
TIMEOUT = 8
MAX_UI_FILES = 400

# Only methods whose verb starts with one of these words (GetStatus,
# ListMethods, ...). Anything else could change the device: never called.
READ_VERBS = ("Get", "List")

# The web UI crawl fetches only files with these extensions, so it can't
# reach a device endpoint that acts (/rpc/..., or a script's own endpoint
# under /script/, which can have any name, even data.json).
STATIC_EXT = (".js", ".mjs", ".css", ".json", ".svg", ".png", ".jpg", ".ico", ".woff", ".woff2", ".ttf",
              ".html", ".webmanifest", ".map")


def fetch(path):
    """GET path; returns (status, headers, body bytes) with gzip undone."""
    req = urllib.request.Request(BASE + path, headers={"Accept-Encoding": "gzip"})
    # headers stays an email.message.Message: its lookups ignore case.
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
            body = r.read()
            status, headers = r.status, r.headers
    except urllib.error.HTTPError as e:
        body, status, headers = e.read(), e.code, (e.headers if e.headers is not None else {})
    if headers.get("Content-Encoding", "").lower() == "gzip" or body[:2] == b"\x1f\x8b":
        try:
            body = gzip.decompress(body)
        except OSError:
            pass
    return status, headers, body


def rpc(method, params=None):
    q = "?" + urllib.parse.urlencode(params) if params else ""
    try:
        status, _, body = fetch(f"/rpc/{method}{q}")
    except Exception as e:  # noqa: BLE001 - a timeout must not end the capture
        return 0, {"error": {"message": f"no reply: {e}"}}
    try:
        data = json.loads(body.decode("utf-8", "replace"))
    except ValueError:
        data = {"_raw": body.decode("utf-8", "replace")}
    return status, data


def is_error(data):
    # Over HTTP GET a Shelly returns errors as {"code": -103, "message": ...};
    # over JSON-RPC they are wrapped in {"error": {...}}.
    return isinstance(data, dict) and ("error" in data or (isinstance(data.get("code"), int) and data["code"] < 0 and "message" in data))


def is_read_only(method):
    if "." not in method:
        return False
    verb = method.rsplit(".", 1)[1]  # BLE.CloudRelay.ListInfos -> ListInfos
    # "Get" or "List" followed by a new word: GetConfig, not "Getaway".
    return any(re.match(v + r"[A-Z]|" + v + r"$", verb) for v in READ_VERBS)


def crawlable(ref):
    """A static UI file on the device itself: no scheme or host, no template
    placeholder from the JS (`${x}`), not under /rpc or /script, a STATIC_EXT name."""
    parts = urllib.parse.urlsplit(ref)
    if parts.scheme or parts.netloc or "$" in ref or "{" in ref:
        return False
    p = parts.path
    return (p.startswith("/") and not p.lower().startswith(("/rpc", "/script"))
            and p.lower().endswith(STATIC_EXT))


def safe_name(s):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("_") or "index"


def main():
    t0 = time.time()
    summary = {"host": HOST, "started": time.strftime("%Y-%m-%d %H:%M:%S"), "rpc": {}, "ui": {}, "errors": []}

    status, info = rpc("Shelly.GetDeviceInfo")
    if status != 200:
        sys.exit(f"No Shelly answering at {BASE} (HTTP {status}). Are you on its Wi-Fi?")
    dev_id = info.get("id", "shelly")
    out = f"shelly-capture-{safe_name(dev_id)}-{time.strftime('%Y%m%d-%H%M%S')}"
    os.makedirs(f"{out}/rpc", exist_ok=True)
    os.makedirs(f"{out}/ui", exist_ok=True)
    print(f"Capturing {dev_id} ({info.get('model')}, stock {info.get('ver')}) into {out}/")

    def save_rpc(name, data):
        with open(f"{out}/rpc/{safe_name(name)}.json", "w") as f:
            json.dump(data, f, indent=2, sort_keys=True)

    # 1. Every read-only RPC method, with each component id where it needs one.
    status, methods = rpc("Shelly.ListMethods")
    if status == 401:
        sys.exit(f"{dev_id} has a password set (HTTP 401), which this script doesn't support. "
                 "Turn authentication off in the stock web UI, then run it again.")
    save_rpc("Shelly.ListMethods", methods)
    all_methods = sorted(methods.get("methods", [])) if isinstance(methods, dict) else []
    read_methods = [m for m in all_methods if is_read_only(m)]
    summary["methods_total"] = len(all_methods)
    summary["methods_skipped_not_read_only"] = [m for m in all_methods if m not in read_methods]

    # Component ids from the status keys ("switch:0", "input:0", ...).
    _, full_status = rpc("Shelly.GetStatus")
    ids = {}
    if isinstance(full_status, dict):
        for key in full_status:
            if ":" in key:
                kind, _, cid = key.partition(":")
                if cid.isdigit():
                    ids.setdefault(kind.lower(), []).append(int(cid))

    for m in read_methods:
        status, data = rpc(m)
        # No reply (0) or refused for auth (401): an id won't help.
        needs_id = status not in (0, 401) and is_error(data)
        kind = m.split(".", 1)[0].lower()
        if needs_id and ids.get(kind):
            for cid in ids[kind]:
                s2, d2 = rpc(m, {"id": cid})
                save_rpc(f"{m}.id{cid}", d2)
                summary["rpc"][f"{m}?id={cid}"] = s2
        elif needs_id and not ids.get(kind):
            s2, d2 = rpc(m, {"id": 0})
            if not is_error(d2):
                save_rpc(f"{m}.id0", d2)
                summary["rpc"][f"{m}?id=0"] = s2
            else:
                save_rpc(m, data)
                summary["rpc"][m] = status
        else:
            save_rpc(m, data)
            summary["rpc"][m] = status
        print(f"  rpc {m}")

    # Paged component list, in full.
    comps, offset = [], 0
    while True:
        _, page = rpc("Shelly.GetComponents", {"offset": offset, "dynamic_only": "false"})
        if not isinstance(page, dict) or not page.get("components"):
            break
        comps += page["components"]
        offset += len(page["components"])
        if offset >= page.get("total", 0):
            break
    save_rpc("Shelly.GetComponents.all", comps)

    # Legacy info endpoint.
    try:
        s, _, body = fetch("/shelly")
    except Exception as e:  # noqa: BLE001 - a timeout must not end the capture
        summary["errors"].append(f"/shelly: {e}")
        s = 0
    if s == 200:
        with open(f"{out}/rpc/_shelly.json", "wb") as f:
            f.write(body)

    # 2. The web UI: the page, then every file it references, recursively.
    ext_alt = "|".join(sorted((e[1:] for e in STATIC_EXT), key=len, reverse=True)).encode()
    ref_re = re.compile(rb"""(?:src|href)\s*=\s*["']([^"'#?]+)|["'`](\.?/?[A-Za-z0-9_./-]+\.(?:""" + ext_alt + rb"""))["'`]""")
    queue, seen = deque(["/", "/index.html"]), set()
    queued = set(queue)
    while queue and len(seen) < MAX_UI_FILES:
        path = queue.popleft()
        seen.add(path)
        try:
            s, headers, body = fetch(path)
        except Exception as e:  # noqa: BLE001 - keep going, record it
            summary["errors"].append(f"{path}: {e}")
            continue
        summary["ui"][path] = s
        if s != 200:
            continue
        # Keep the device's folders, so /a/b.js and /a_b.js stay two files.
        parts = [safe_name(seg).lstrip(".") or "_" for seg in path.split("/") if seg] or ["index.html"]
        local = os.path.join(out, "ui", *parts)
        os.makedirs(os.path.dirname(local), exist_ok=True)
        with open(local, "wb") as f:
            f.write(body)
        print(f"  ui  {path} ({len(body)} bytes)")
        ctype = headers.get("Content-Type", "")
        if any(t in ctype for t in ("html", "javascript", "css", "json")) or path.endswith((".js", ".css", ".html", ".json", ".mjs")):
            for m in ref_re.finditer(body):
                try:
                    ref = urllib.parse.urljoin(path, (m.group(1) or m.group(2)).decode("utf-8", "replace"))
                    ok = crawlable(ref)
                except ValueError:  # a malformed ref in minified JS ("//[x")
                    continue
                if ok and ref not in queued:
                    queued.add(ref)
                    queue.append(ref)

    summary["seconds"] = round(time.time() - t0, 1)
    with open(f"{out}/summary.json", "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)

    with zipfile.ZipFile(f"{out}.zip", "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(out):
            for name in files:
                z.write(os.path.join(root, name))
    print(f"\nDone in {summary['seconds']} s: {len(summary['rpc'])} RPC calls, {len(summary['ui'])} UI files.")
    print(f"Saved {out}/ and {out}.zip")


if __name__ == "__main__":
    main()
