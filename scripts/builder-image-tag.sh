#!/bin/sh
# Prints the builder image tag for the current .devcontainer/Dockerfile:
# df-<first 12 hex digits of its SHA-256>. A change to the Dockerfile gives a
# new tag, so CI never builds with an image made from an older Dockerfile.
#
#   scripts/builder-image-tag.sh        -> df-0123456789ab
set -eu
dockerfile="$(cd "$(dirname "$0")/.." && pwd)/.devcontainer/Dockerfile"
if command -v sha256sum >/dev/null 2>&1; then
    sum="$(sha256sum "$dockerfile")"
else
    sum="$(shasum -a 256 "$dockerfile")"
fi
echo "df-$(echo "$sum" | cut -c1-12)"
