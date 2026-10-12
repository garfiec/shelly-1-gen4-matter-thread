#!/bin/bash
# Builds one product line the way CI does, inside the builder container:
#
#   scripts/ci-build.sh source/shelly-1-gen4/light
#
# With SHELLY_WERROR=1 (set by CI) a warning in this repository's own code
# fails the build (source/components/warnings.cmake); third-party warnings do
# not. The log check below catches a warning the compiler flags miss.
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
project="${1:?usage: scripts/ci-build.sh source/<device>/<line>}"
cd "$repo/$project"

if [ -z "${IDF_EXPORT_DONE:-}" ]; then
    export IDF_PATH_FORCE=1
    set +u
    . "$IDF_PATH/export.sh" >/dev/null
    . "$ESP_MATTER_PATH/export.sh" >/dev/null
    set -u
fi

args=(-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.c6_thread_shelly)
if [ "${SHELLY_WERROR:-0}" = 1 ]; then
    args+=(-DSHELLY_WERROR=1)
fi
# A clean checkout has no sdkconfig yet and would otherwise build for the
# default target. set-target wipes an existing build, so only run it then.
if [ ! -f sdkconfig ]; then
    idf.py "${args[@]}" set-target esp32c6
fi

status=0
idf.py "${args[@]}" build 2>&1 | tee build.log || status=$?

if command -v ccache >/dev/null 2>&1; then
    ccache -s || true
fi
if [ "$status" -ne 0 ]; then
    exit "$status"
fi

# Warnings from our own code: shared components, each device's app and each
# product line's main. Managed components and the SDKs are not ours.
ours="$(sed 's/\x1b\[[0-9;]*m//g' build.log \
    | grep -E ': warning:' \
    | grep -E '(^|/)source/(components/[^/]+|[^/]+/app|[^/]+/[^/]+/main)/' \
    | grep -v '/managed_components/' | sort -u || true)"
if [ -n "$ours" ]; then
    echo "::error::warnings in this repository's code:"
    echo "$ours"
    exit 1
fi
echo "No warnings in this repository's code."
