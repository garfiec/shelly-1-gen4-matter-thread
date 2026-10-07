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
Refuse a build that could make a device impossible to return to stock firmware.

Every variant runs this after building (see source/components/reversibility.cmake).
It can also be pointed at a built variant directory by hand:

    python3 scripts/check-reversibility.py source/shelly-1pm-gen4/outlet

It checks three things, and exits non-zero if any fails:

1. sdkconfig: secure boot, flash encryption, eFuse anti-rollback and a disabled
   or secure ROM download mode are all off. Each of them burns eFuses, which is
   permanent, and several of them would block a UART restore of the stock backup.
2. partitions.csv: the table matches the stock Shelly Gen4 layout row for row
   (name, type, subtype, offset, size), so the stock regions stay where the stock
   firmware expects them. Comments are ignored.
3. The linked app and bootloader: no eFuse-writing function from ESP-IDF's efuse
   component is linked in, so nothing in this firmware can burn an eFuse.

See docs/REVERSIBILITY.md.
"""
import argparse, csv, glob, os, re, shutil, subprocess, sys

# Kconfig options that must never be enabled.
FORBIDDEN_CONFIG = [
    "CONFIG_SECURE_BOOT",
    "CONFIG_SECURE_BOOT_V2_ENABLED",
    "CONFIG_SECURE_FLASH_ENC_ENABLED",
    "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK",
    "CONFIG_SECURE_DISABLE_ROM_DL_MODE",
    "CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE",
    "CONFIG_EFUSE_VIRTUAL",  # a virtual-eFuse test build must never ship
]

# The stock Shelly Gen4 partition layout (1 Gen4, 1PM Gen4, 1 Mini Gen4).
STOCK_PARTITIONS = [
    ("otadata",    "data", "ota",   0x11000,  0x2000),
    ("nvs",        "data", "nvs",   0x14000,  0xc000),
    ("app_0",      "app",  "ota_0", 0x20000,  0x300000),
    ("fs_0",       "data", "0x82",  0x320000, 0xe0000),
    ("app_1",      "app",  "ota_1", 0x400000, 0x300000),
    ("fs_1",       "data", "0x82",  0x700000, 0xe0000),
    ("zb_storage", "data", "0x81",  0x7e8000, 0x8000),
    ("shelly",     "data", "0x88",  0x7f0000, 0x10000),
]

# ESP-IDF functions that write eFuses. The ROM's own ets_efuse_* entry points
# are absolute symbols from the ROM linker script and always present; they
# are not checked.
FORBIDDEN_SYMBOLS = re.compile(
    r"^(esp_efuse_write\w*|esp_efuse_burn_new_values|esp_efuse_batch_write_commit|"
    r"esp_efuse_set_write_protect|esp_efuse_set_read_protect|esp_efuse_set_key_dis_\w+|"
    r"esp_efuse_set_digest_revoke|esp_efuse_set_rom_log_scheme|esp_efuse_disable_rom_download_mode|"
    r"esp_efuse_enable_rom_secure_download_mode|esp_efuse_update_secure_version|"
    r"esp_flash_encrypt_(init|enable|contents)|esp_flash_encryption_enable|"
    r"esp_flash_encryption_set_release_mode|"
    r"esp_secure_boot_(enable\w*|v2_permanently_enable|permanently_enable\w*|enable_secure_features))$")


def fail(errors, msg):
    errors.append(msg)
    print(f"REVERSIBILITY: {msg}", file=sys.stderr)


def check_sdkconfig(path, errors):
    if not os.path.isfile(path):
        fail(errors, f"{path} not found; build the variant first")
        return
    enabled = set()
    with open(path) as f:
        for line in f:
            m = re.match(r"^(CONFIG_\w+)=y\s*$", line)
            if m:
                enabled.add(m.group(1))
    for opt in FORBIDDEN_CONFIG:
        if opt in enabled:
            fail(errors, f"{opt} is enabled; it burns eFuses or blocks a UART restore")


def parse_int(s):
    return int(s, 0)


def check_partitions(project, errors):
    path = os.path.join(project, "partitions.csv")
    rows = []
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#"):
                continue
            cells = [c.strip() for c in row]
            rows.append((cells[0], cells[1], cells[2], parse_int(cells[3]), parse_int(cells[4])))
    if rows != STOCK_PARTITIONS:
        fail(errors, f"{path} does not match the stock Shelly Gen4 layout")
        for i in range(max(len(rows), len(STOCK_PARTITIONS))):
            got = rows[i] if i < len(rows) else None
            want = STOCK_PARTITIONS[i] if i < len(STOCK_PARTITIONS) else None
            if got != want:
                print(f"  row {i}: got {got}, stock {want}", file=sys.stderr)


def find_nm():
    for name in ("riscv32-esp-elf-nm",):
        found = shutil.which(name)
        if found:
            return found
    tools = os.environ.get("IDF_TOOLS_PATH", os.path.expanduser("~/.espressif"))
    hits = glob.glob(os.path.join(tools, "tools", "riscv32-esp-elf", "*", "riscv32-esp-elf", "bin",
                                  "riscv32-esp-elf-nm"))
    return hits[0] if hits else None


def check_elves(project, build, errors):
    elves = [p for p in glob.glob(os.path.join(build, "*.elf"))]
    bootloader = os.path.join(build, "bootloader", "bootloader.elf")
    if os.path.isfile(bootloader):
        elves.append(bootloader)
    if not elves:
        fail(errors, f"no ELF found in {build}; build the variant first")
        return
    nm = find_nm()
    if nm is None:
        fail(errors, "riscv32-esp-elf-nm not found; run inside the ESP-IDF environment")
        return
    for elf in elves:
        out = subprocess.run([nm, "--defined-only", elf], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) != 3 or parts[1] in ("A", "a"):
                continue
            if FORBIDDEN_SYMBOLS.match(parts[2]):
                fail(errors, f"{os.path.relpath(elf, project)} links {parts[2]}, which can burn eFuses")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", nargs="?", default=".", help="variant directory (default: current)")
    ap.add_argument("--sdkconfig", help="sdkconfig to check (default: <project>/sdkconfig)")
    ap.add_argument("--build-dir", help="build directory holding the ELFs (default: <project>/build)")
    ap.add_argument("--skip-elf", action="store_true", help="check sdkconfig and partitions only")
    args = ap.parse_args()

    project = os.path.abspath(args.project)
    sdkconfig = os.path.abspath(args.sdkconfig) if args.sdkconfig else os.path.join(project, "sdkconfig")
    build = os.path.abspath(args.build_dir) if args.build_dir else os.path.join(project, "build")
    errors = []
    check_sdkconfig(sdkconfig, errors)
    check_partitions(project, errors)
    if not args.skip_elf:
        check_elves(project, build, errors)

    if errors:
        print(f"REVERSIBILITY CHECK FAILED ({len(errors)} problem(s)); see docs/REVERSIBILITY.md",
              file=sys.stderr)
        return 1
    print("Reversibility check passed: no eFuse writes, no secure boot or flash encryption, "
          "stock partition layout")
    return 0


if __name__ == "__main__":
    sys.exit(main())
