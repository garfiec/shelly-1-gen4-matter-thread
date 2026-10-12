#!/bin/bash
# Packages one product line built by scripts/ci-build.sh, inside the builder
# container, and checks the packages before a release publishes them:
#
#   scripts/ci-package.sh source/shelly-1-gen4/light
#
# Writes to dist/:
#   <hardware>-<line>-v<version>.ota       Matter OTA, and the Wi-Fi mode page
#   <hardware>-<line>-v<version>-ota.zip   Shelly web UI install
#   <hardware>-<line>-v<version>-uart.bin  UART install at 0x0
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
project="${1:?usage: scripts/ci-package.sh source/<device>/<line>}"
dir="$repo/$project"
hardware="$(basename "$(dirname "$dir")")"
line="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project_name"])' "$dir/build/project_description.json")"
version="$(cat "$repo/VERSION")"
number="$(python3 "$repo/scripts/bump-version.py" encode "$version")"
stem="$hardware-$line-v$version"

if [ -z "${IDF_EXPORT_DONE:-}" ]; then
    export IDF_PATH_FORCE=1
    set +u
    . "$IDF_PATH/export.sh" >/dev/null
    . "$ESP_MATTER_PATH/export.sh" >/dev/null
    set -u
fi

built="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["project_version"])' "$dir/build/project_description.json")"
if [ "$built" != "$version" ]; then
    echo "::error::$project was built as $built, but VERSION is $version"
    exit 1
fi

python3 "$repo/scripts/make-matter-ota.py" "$dir"
python3 "$repo/scripts/make-webui-ota-zip.py" "$dir"
(cd "$dir" && idf.py merge-bin >/dev/null)

mkdir -p "$repo/dist"
mv "$dir/$stem.ota" "$dir/$stem-ota.zip" "$repo/dist/"
cp "$dir/build/merged-binary.bin" "$repo/dist/$stem-uart.bin"

# The .ota header decides which devices take the update: its product ID must
# be this line's and its version this release's.
pid="$(sed -n 's/^CONFIG_DEVICE_PRODUCT_ID=//p' "$dir/sdkconfig")"
show="$(python3 "$ESP_MATTER_PATH/connectedhomeip/connectedhomeip/src/app/ota_image_tool.py" show "$repo/dist/$stem.ota")"
echo "$show"
python3 - "$show" "$pid" "$number" "$version" <<'PY'
import re, sys
show, pid, number, version = sys.argv[1:]
def field(name):
    m = re.search(rf"\] {name}: (\S+)", show)
    if not m:
        sys.exit(f"ota_image_tool.py show printed no {name}")
    return m.group(1)
checks = {
    "Product Id": int(pid, 0),
    "Version": int(number),
}
for name, want in checks.items():
    got = int(field(name), 0)
    if got != want:
        sys.exit(f"{name} in the .ota header is {got}, expected {want}")
if field("Version String") != version:
    sys.exit(f"Version String in the .ota header is not {version}")
print(f"header OK: product ID 0x{int(pid, 0):04X}, version {number} ({version})")
PY
ls -l "$repo/dist/$stem"*
