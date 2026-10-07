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
Check that docs/BUTTON-AND-LED.md matches the code and tests, row for row.

The page is the specification of the device UI. Every row has an ID:

  G-xx, H-xx, B-xx, SW-xx, SM-xx
                           a host test in source/components/test_host/test_ui.c
                           with the same ID and the same words
  LED-xx                   a pattern in shelly_status_led/include/led_patterns.h
  T-xx                     a constant in shelly_button/include/button_logic.h
  P-xx                     documented only (needs hardware)

Fails if a row is missing, extra, duplicated or worded differently on
either side. Run by source/components/test_host/run.sh.
"""
import os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DOC = os.path.join(ROOT, "docs", "BUTTON-AND-LED.md")
COMP = os.path.join(ROOT, "source", "components")
TESTS = os.path.join(COMP, "test_host", "test_ui.c")
PATTERNS = os.path.join(COMP, "shelly_status_led", "include", "led_patterns.h")
TIMINGS = os.path.join(COMP, "shelly_button", "include", "button_logic.h")

ID_RE = re.compile(r"^(G|H|B|SW|SM|LED|T|P)-\d+$")
TESTED = ("G", "H", "B", "SW", "SM")


def rel(path):
    return os.path.relpath(path, ROOT)


def doc_rows(errors):
    rows = {}
    with open(DOC, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if not line.startswith("|"):
                continue
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if not cells or not ID_RE.match(cells[0]):
                continue
            if cells[0] in rows:
                errors.append(f"{rel(DOC)}:{n}: duplicate row {cells[0]}")
            rows[cells[0]] = (n, cells[1:])
    return rows


def test_rows(errors):
    rows = {}
    text = open(TESTS, encoding="utf-8").read()
    for m in re.finditer(r'\{"((?:G|H|B|SW|SM)-\d+)",((?:\s*"[^"]*",?)+)', text):
        rid = m.group(1)
        words = re.findall(r'"([^"]*)"', m.group(2))
        if rid in rows:
            errors.append(f"{rel(TESTS)}: duplicate test row {rid}")
        rows[rid] = words
    return rows


def render_pattern(first_on, repeat, steps, ms):
    if steps == 0:
        return "on" if first_on else "off"
    parts, level = [], first_on
    for v in ms[:steps]:
        parts.append(f"{'on' if level else 'off'} {v}")
        level = not level
    return ", ".join(parts) + (", repeating" if repeat else ", once")


def code_patterns(errors):
    rows = {}
    pat = re.compile(r"=\s*\{\s*(true|false)\s*,\s*(true|false)\s*,\s*(\d+)\s*,\s*\{([^}]*)\}\s*\}\s*,\s*//\s*(LED-\d+)")
    for line in open(PATTERNS, encoding="utf-8"):
        m = pat.search(line)
        if m:
            ms = [int(x) for x in m.group(4).split(",") if x.strip()]
            rows[m.group(5)] = render_pattern(m.group(1) == "true", m.group(2) == "true", int(m.group(3)), ms)
    return rows


def code_timings(errors):
    rows = {}
    pat = re.compile(r"#define\s+(\w+)\s+(\d+)\s*//\s*(T-\d+)")
    for line in open(TIMINGS, encoding="utf-8"):
        m = pat.search(line)
        if m:
            name, value, rid = m.groups()
            rows[rid] = f"{value} ms" if name.endswith("_MS") else value
    return rows


def main():
    errors = []
    doc = doc_rows(errors)
    tests = test_rows(errors)
    leds = code_patterns(errors)
    timings = code_timings(errors)

    def compare(prefixes, code, what, cells_for):
        doc_ids = {i for i in doc if i.split("-")[0] in prefixes}
        for rid in sorted(doc_ids - set(code)):
            errors.append(f"{rel(DOC)}:{doc[rid][0]}: row {rid} has no {what}")
        for rid in sorted(set(code) - doc_ids):
            errors.append(f"{what} {rid} has no row in {rel(DOC)}")
        for rid in sorted(doc_ids & set(code)):
            want = code[rid] if isinstance(code[rid], list) else [code[rid]]
            got = cells_for(rid, len(want))
            if got != want:
                errors.append(f"{rel(DOC)}:{doc[rid][0]}: row {rid} says {got}, the {what} says {want}")

    compare(TESTED, tests, "test in test_ui.c", lambda rid, n: doc[rid][1][:n])
    compare(("LED",), leds, "pattern in led_patterns.h", lambda rid, n: doc[rid][1][:1])
    compare(("T",), timings, "constant in button_logic.h", lambda rid, n: doc[rid][1][1:2])

    if errors:
        for e in errors:
            print(f"UI SPEC: {e}", file=sys.stderr)
        print(f"UI SPEC CHECK FAILED ({len(errors)} problem(s)). docs/BUTTON-AND-LED.md, the code and "
              f"test_ui.c must change together; see CLAUDE.md.", file=sys.stderr)
        return 1
    counts = {p: sum(1 for i in doc if i.split("-")[0] == p) for p in ("G", "H", "B", "SW", "SM", "LED", "T", "P")}
    print("UI spec check passed: " + ", ".join(f"{n} {p}" for p, n in counts.items()) + " rows match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
