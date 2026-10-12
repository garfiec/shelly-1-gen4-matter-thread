#!/usr/bin/env python3
"""The firmware version: one number for every device and product line.

Versions are calendar-based, YYYY.MM.PATCH (month zero-padded, PATCH 0-99),
tagged vYYYY.MM.PATCH. VERSION at the repository root holds the current one;
`set` writes it into every product line's sources. See docs/RELEASING.md.

    scripts/bump-version.py next            # the next release's version
    scripts/bump-version.py set 2026.10.0   # write it into VERSION and the sources
    scripts/bump-version.py check           # VERSION and every source agree
    scripts/bump-version.py encode 2026.10.0

Matter compares the software version as a 32-bit number, and a device only
updates to a higher one. YYYY.MM.PATCH is YYYY*10000 + MM*100 + PATCH
(2026.10.0 is 20261000), above every SemVer release before it (3.0.0 was
0x00030000, 196608) and rising with every release.

In GitHub Actions, `next` and `set` also write version, tag and
version_number to $GITHUB_OUTPUT. BUMP_DATE=YYYY-MM overrides today's UTC
month, for testing.
"""
import datetime
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION_FILE = os.path.join(ROOT, "VERSION")
HEADER_GLOB = "source/*/*/main/CHIPProjectConfig.h"

CALVER = re.compile(r"^(\d{4})\.(\d{2})\.(\d{1,2})$")
# Releases before CalVer (3.0.0 and older); VERSION may still hold one.
SEMVER = re.compile(r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})$")

PATTERNS = {
    "cmake_ver": re.compile(r'^set\(PROJECT_VER "([^"]*)"\)$', re.M),
    "cmake_num": re.compile(r"^set\(PROJECT_VER_NUMBER (\d+)\)$", re.M),
    "header_str": re.compile(r'^(#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING +)"([^"]*)"$', re.M),
    "header_num": re.compile(r"^(#define CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION +)(0x[0-9A-Fa-f]+|\d+)$", re.M),
}


def fail(msg):
    sys.exit(f"bump-version: {msg}")


def parse(version):
    """(year, month, patch) for a CalVer version, or None for SemVer."""
    m = CALVER.match(version)
    if m:
        year, month, patch = (int(g) for g in m.groups())
        if not 1 <= month <= 12 or year < 2000:
            fail(f"{version} is not YYYY.MM.PATCH")
        return year, month, patch
    if SEMVER.match(version):
        return None
    fail(f"{version} is not YYYY.MM.PATCH")


def encode(version):
    cal = parse(version)
    if cal:
        year, month, patch = cal
        return year * 10000 + month * 100 + patch
    major, minor, patch = (int(g) for g in SEMVER.match(version).groups())
    return (major << 16) | (minor << 8) | patch


def read_version():
    try:
        version = open(VERSION_FILE).read().strip()
    except FileNotFoundError:
        fail("VERSION not found at the repository root")
    parse(version)
    return version


def today():
    override = os.environ.get("BUMP_DATE")
    if override:
        m = re.match(r"^(\d{4})-(\d{2})$", override)
        if not m:
            fail(f"BUMP_DATE must be YYYY-MM, not {override!r}")
        return int(m.group(1)), int(m.group(2))
    now = datetime.datetime.now(datetime.timezone.utc)
    return now.year, now.month


def next_version(current):
    year, month = today()
    cal = parse(current)
    if cal and (cal[0], cal[1]) > (year, month):
        fail(f"VERSION {current} is ahead of today ({year}.{month:02d})")
    if cal and (cal[0], cal[1]) == (year, month):
        if cal[2] >= 99:
            fail(f"{current} is the last release this month (PATCH stops at 99)")
        return f"{year}.{month:02d}.{cal[2] + 1}"
    return f"{year}.{month:02d}.0"


def sources():
    """Each product line's CMakeLists.txt and CHIPProjectConfig.h."""
    headers = sorted(glob.glob(os.path.join(ROOT, HEADER_GLOB)))
    if not headers:
        fail(f"no {HEADER_GLOB} found")
    cmake = [os.path.join(os.path.dirname(os.path.dirname(h)), "CMakeLists.txt") for h in headers]
    return cmake, headers


def rel(path):
    return os.path.relpath(path, ROOT)


def write_outputs(version):
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a") as f:
            f.write(f"version={version}\ntag=v{version}\nversion_number={encode(version)}\n")


def set_version(version):
    if parse(version) is None:
        fail(f"{version} is not YYYY.MM.PATCH; new versions are calendar-based")
    number = encode(version)
    cmake, headers = sources()
    for path in cmake:
        s = open(path).read()
        s, n1 = PATTERNS["cmake_ver"].subn(f'set(PROJECT_VER "{version}")', s)
        s, n2 = PATTERNS["cmake_num"].subn(f"set(PROJECT_VER_NUMBER {number})", s)
        if (n1, n2) != (1, 1):
            fail(f"{rel(path)}: expected one PROJECT_VER and one PROJECT_VER_NUMBER")
        open(path, "w").write(s)
    for path in headers:
        s = open(path).read()
        s, n1 = PATTERNS["header_str"].subn(lambda m: f'{m.group(1)}"{version}"', s)
        s, n2 = PATTERNS["header_num"].subn(lambda m: f"{m.group(1)}0x{number:08X}", s)
        if (n1, n2) != (1, 1):
            fail(f"{rel(path)}: expected one software version string and number")
        open(path, "w").write(s)
    open(VERSION_FILE, "w").write(version + "\n")
    write_outputs(version)
    print(f"{version} ({number}, 0x{number:08X}) in VERSION, {len(cmake)} CMakeLists.txt "
          f"and {len(headers)} CHIPProjectConfig.h")


def check():
    version = read_version()
    number = encode(version)
    cmake, headers = sources()
    bad = []
    for path in cmake:
        s = open(path).read()
        ver = PATTERNS["cmake_ver"].findall(s)
        num = PATTERNS["cmake_num"].findall(s)
        if ver != [version] or [int(n) for n in num] != [number]:
            bad.append(f"{rel(path)}: PROJECT_VER {ver}, PROJECT_VER_NUMBER {num}")
    for path in headers:
        s = open(path).read()
        ver = [m[1] for m in PATTERNS["header_str"].findall(s)]
        num = [int(m[1], 0) for m in PATTERNS["header_num"].findall(s)]
        if ver != [version] or num != [number]:
            bad.append(f"{rel(path)}: software version {ver}, number {num}")
    if bad:
        fail(f"VERSION is {version} ({number}), but:\n  " + "\n  ".join(bad)
             + "\nRun scripts/bump-version.py set <version>.")
    print(f"{version} ({number}) everywhere: VERSION, {len(cmake)} product lines")


def main(argv):
    if len(argv) == 1 and argv[0] == "next":
        version = next_version(read_version())
        write_outputs(version)
        print(version)
    elif len(argv) == 2 and argv[0] == "set":
        set_version(argv[1])
    elif len(argv) == 1 and argv[0] == "check":
        check()
    elif len(argv) == 2 and argv[0] == "encode":
        print(encode(argv[1]))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
