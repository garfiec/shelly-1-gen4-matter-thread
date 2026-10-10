#!/bin/sh
# Checks that the committed Wi-Fi mode page (page.html.gz) is a build of its
# Svelte source, and type-checks the source. Needs Node 20.19+ and npm; the
# firmware build itself needs neither, since it only embeds page.html.gz.
#
#   scripts/check-web.sh
#
# After changing anything under shelly_maintenance/web, run it and commit
# the rebuilt page.html.gz with the source.
set -eu

dir="$(cd "$(dirname "$0")/.." && pwd)/source/components/shelly_maintenance"
web="$dir/web"
before="$(mktemp)"
trap 'rm -f "$before"' EXIT
cp "$dir/page.html.gz" "$before"

cd "$web"
npm ci --no-fund --no-audit --loglevel=error
npm run --silent check
npm run --silent build

# Compare what the browser gets, not the gzip bytes: a different zlib may
# compress the same page differently.
python3 - "$before" "$dir/page.html.gz" <<'EOF'
import gzip, sys
old, new = (gzip.open(p).read() for p in sys.argv[1:3])
if old != new:
    sys.exit("page.html.gz was not built from the current source: commit the rebuilt file")
print(f"page.html.gz matches its source ({len(new)} bytes, served gzipped)")
EOF
