#!/usr/bin/env python3
"""Writes a GitHub release's text: the CHANGELOG section, which file to
download for each device, and GitHub's list of merged changes.

    scripts/release-notes.py <version> <changelog notes> <dist dir> <generated notes> > body.md

Used by .github/workflows/release.yml.
"""
import glob
import os
import re
import subprocess
import sys

DEVICES = {
    "shelly-1-gen4": "Shelly 1 Gen4",
    "shelly-1-mini-gen4": "Shelly 1 Mini Gen4",
    "shelly-1pm-gen4": "Shelly 1PM Gen4",
    "shelly-2pm-gen4": "Shelly 2PM Gen4",
}
LINES = {"light": "Light", "outlet": "Outlet", "opener": "Opener", "light-switch": "Light Switch"}


def main(version, changelog, dist, generated):
    number = int(subprocess.check_output(
        [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "bump-version.py"),
         "encode", version], text=True))
    repo = (os.environ.get("GITHUB_SERVER_URL", "https://github.com") + "/"
            + os.environ.get("GITHUB_REPOSITORY", "garfiec/shelly-1-gen4-matter-thread"))
    rows = []
    for ota in sorted(glob.glob(os.path.join(dist, f"*-v{version}.ota"))):
        stem = os.path.basename(ota)[:-len(".ota")]
        m = re.match(rf"^(shelly-.+-gen4)-(.+)-v{re.escape(version)}$", stem)
        if not m:
            sys.exit(f"unexpected package name {os.path.basename(ota)}")
        hardware, line = m.groups()
        rows.append(((hardware, line), f"| {DEVICES.get(hardware, hardware)} | {LINES.get(line, line)} "
                     f"| `{stem}.ota` | `{stem}-ota.zip` | `{stem}-uart.bin` |"))
    if not rows:
        sys.exit(f"no .ota files for {version} in {dist}")

    print(open(changelog).read().strip())
    print()
    print("## Downloads")
    print()
    print("Pick your device and product line. A device installed from an earlier build updates with its "
          "own line's file, whatever device type you've chosen since.")
    print()
    print("| Device | Product line | Update (Matter or the Wi-Fi mode page) | First install from the Shelly web UI | UART install |")
    print("|---|---|---|---|---|")
    print("\n".join(row for _, row in sorted(rows)))
    print()
    print(f"Matter software version: {number} (0x{number:08X}). Check the files against `SHA256SUMS.txt`. "
          f"How to install: [Flashing]({repo}/blob/main/docs/FLASHING.md), "
          f"[Updating]({repo}/blob/main/docs/UPDATING.md).")
    text = open(generated).read().strip()
    if text:
        print()
        print(text)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
