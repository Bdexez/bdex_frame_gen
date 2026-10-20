#!/usr/bin/env bash
# Builds the Decky Loader plugin (packaging/decky: the Steam Deck quick
# access menu) in a Node container — TypeScript checked, bundled by
# @decky/rollup — tests its backend, and packs the zip Decky installs
# ("Install from ZIP" in Decky's developer settings):
#   bdex-framegen/{dist/index.js, main.py, plugin.json, package.json, LICENSE, README.md}
#   tools/ci/decky-check.sh [outdir]   (default: dist/)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)"
python3 "$ROOT/tests/decky_backend.py"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp -r "$ROOT/packaging/decky" "$work/src"
cp "$ROOT/LICENSE" "$work/src/"
docker run --rm -v "$work:/work" -w /work/src node:20-bookworm-slim bash -euo pipefail -c '
trap "chown -R '"$(id -u):$(id -g)"' /work" EXIT   # (node_modules: removable by the caller)
corepack enable >/dev/null 2>&1
pnpm install --frozen-lockfile --silent >/tmp/install.log 2>&1 || { tail -30 /tmp/install.log; exit 1; }
# Known vulnerabilities in what the zip ships (the build tools are not in it:
# braces under @decky/rollup, 2026-10-13, has no fixed version).
pnpm audit --prod --audit-level high
pnpm exec tsc --noEmit
pnpm build >/tmp/build.log 2>&1 || { tail -30 /tmp/build.log; exit 1; }
test -s dist/index.js
mkdir -p /work/pkg/bdex-framegen
cp -r dist main.py plugin.json package.json LICENSE README.md /work/pkg/bdex-framegen/
'
(cd "$work/pkg" && python3 -c "
import shutil; shutil.make_archive('bdex-framegen-decky', 'zip', '.', 'bdex-framegen')")
cp "$work/pkg/bdex-framegen-decky.zip" "$out/"
echo "decky plugin: $out/bdex-framegen-decky.zip ($(du -h "$out/bdex-framegen-decky.zip" | cut -f1))"
