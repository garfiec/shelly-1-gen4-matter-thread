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
Build a Matter OTA image (.ota) from a Gen4 variant's build output.

Point it at a variant directory (defaults to the current one):

    python3 scripts/make-matter-ota.py source/shelly-1-gen4/opener
    python3 scripts/make-matter-ota.py source/shelly-1pm-gen4/outlet
    python3 scripts/make-matter-ota.py source/shelly-1-mini-gen4/outlet

Every field that the device matches against is read from the build:

  - vendor / product ID from the variant's sdkconfig (CONFIG_DEVICE_VENDOR_ID /
    CONFIG_DEVICE_PRODUCT_ID); an opener build can never get an outlet's PID.
    Matter OTA only ever offers it to the variant it was built for.
  - software version (number + string) from main/CHIPProjectConfig.h
    (CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION[_STRING]). The script refuses
    to package a build that predates the header (version string not compiled
    into the .bin). The .ota header cannot claim a version its payload
    does not carry.

It wraps build/<variant>.bin with the upstream Matter ota_image_tool.py and writes
<hardware>-<variant>-v<version>.ota next to the build, where <hardware>
is the source/<hardware> directory the variant lives in.

Run it in the same esp-matter environment you build in. ota_image_tool.py is
located in the connectedhomeip checkout via $ESP_MATTER_PATH (or set $OTA_IMAGE_TOOL
to its path). A copy under scripts/ will NOT run on its own: ota_image_tool.py
imports matter.tlv from a sibling directory that only exists inside the esp-matter
tree, so it has to run from there.
"""
import json, os, re, subprocess, sys


def sdkconfig_int(sdkconfig, key):
    m = re.search(rf"^{re.escape(key)}=(.+)$", open(sdkconfig).read(), re.M)
    if not m:
        sys.exit(f"{key} not set in {sdkconfig} — run `idf.py build` for this variant first")
    return int(m.group(1).strip(), 0)


def define(header, key):
    m = re.search(rf"^#define\s+{re.escape(key)}\s+(.+)$", open(header).read(), re.M)
    if not m:
        sys.exit(f"{key} not found in {header}")
    return m.group(1).strip()


def find_ota_image_tool():
    override = os.environ.get("OTA_IMAGE_TOOL")
    if override and os.path.isfile(override):
        return override
    esp_matter = os.environ.get("ESP_MATTER_PATH")
    if esp_matter:
        cand = os.path.join(esp_matter, "connectedhomeip", "connectedhomeip",
                            "src", "app", "ota_image_tool.py")
        if os.path.isfile(cand):
            return cand
    sys.exit("could not find ota_image_tool.py — set ESP_MATTER_PATH (or OTA_IMAGE_TOOL) "
             "to the esp-matter checkout you build with")


def main():
    variant_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    hardware = os.path.basename(os.path.dirname(variant_dir))
    build = os.path.join(variant_dir, "build")
    desc = os.path.join(build, "project_description.json")
    if not os.path.isfile(desc):
        sys.exit(f"no build found at {build} — run `idf.py build` in {variant_dir} first")
    variant = json.load(open(desc))["project_name"]

    sdkconfig = os.path.join(variant_dir, "sdkconfig")
    header = os.path.join(variant_dir, "main", "CHIPProjectConfig.h")
    app_bin = os.path.join(build, f"{variant}.bin")
    for p in (sdkconfig, header, app_bin):
        if not os.path.isfile(p):
            sys.exit(f"missing build output: {p}")

    # Package only a build that passes the reversibility check, whichever
    # target built it (`idf.py app` skips the check).
    check = os.path.join(os.path.dirname(os.path.abspath(__file__)), "check-reversibility.py")
    if subprocess.run([sys.executable, check, variant_dir]).returncode != 0:
        sys.exit("the build fails the reversibility check; not packaging it")

    vid = sdkconfig_int(sdkconfig, "CONFIG_DEVICE_VENDOR_ID")
    pid = sdkconfig_int(sdkconfig, "CONFIG_DEVICE_PRODUCT_ID")
    version = int(define(header, "CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION").split()[0], 0)
    version_str = define(header, "CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING").split('"')[1]

    # The version above is read from source, but the payload comes from
    # build/. A build that predates a version bump would produce an .ota
    # whose header claims a version its payload does not carry. The device
    # flashes it, reboots, reports the old version, and the controller
    # marks the update failed.
    if version_str.encode() not in open(app_bin, "rb").read():
        sys.exit(f"version {version_str} not found in {app_bin} — rebuild before packaging")

    # Wi-Fi maintenance mode installs an image only if its hardware code
    # (shelly_hw_desc.h, at byte 288) matches the device. Refuse to package
    # one that would be refused there, or would reach the wrong hardware.
    m = re.search(r'^CONFIG_SHELLY_HARDWARE_CODE="([^"]*)"$', open(sdkconfig).read(), re.M)
    if not m:
        sys.exit(f"CONFIG_SHELLY_HARDWARE_CODE not set in {sdkconfig}")
    image = open(app_bin, "rb").read(288 + 20)
    magic = int.from_bytes(image[288:292], "little")
    hw_code = image[292:308].split(b"\0")[0].decode(errors="replace")
    if magic != 0x44574853 or hw_code != m.group(1):
        sys.exit(f"{app_bin} carries hardware code {hw_code!r} (magic 0x{magic:08X}), expected "
                 f"{m.group(1)!r} — rebuild before packaging")

    out = os.path.join(variant_dir, f"{hardware}-{variant}-v{version_str}.ota")

    cmd = [sys.executable, find_ota_image_tool(), "create",
           "-v", f"0x{vid:04X}", "-p", f"0x{pid:04X}",
           "-vn", str(version), "-vs", version_str,
           "-da", "sha256", app_bin, out]
    subprocess.run(cmd, check=True)

    print(f"Created {os.path.basename(out)} ({os.path.getsize(out) / 1024 / 1024:.1f} MB)")
    print(f"  {variant}  VID 0x{vid:04X}  PID 0x{pid:04X}  version {version} ({version_str})")


if __name__ == "__main__":
    main()
