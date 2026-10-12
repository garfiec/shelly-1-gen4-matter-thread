#!/bin/sh
# Frees about 30 GB on a GitHub-hosted Ubuntu runner by removing toolchains
# this repository never uses. The builder image is about 12 GB unpacked and a
# firmware build adds a few more. CI only; never run it on your own machine.
set -eu
if [ "${GITHUB_ACTIONS:-}" != true ]; then
    echo "scripts/ci-free-disk.sh only runs on GitHub Actions runners" >&2
    exit 1
fi
df -h /
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup \
    /opt/hostedtoolcache/CodeQL /usr/local/share/boost /usr/share/swift \
    /usr/local/share/powershell /usr/local/lib/node_modules /opt/microsoft || true
sudo docker image prune -af >/dev/null 2>&1 || true
df -h /
