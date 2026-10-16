#!/usr/bin/env bash
# The online benchmark (tools/replay_bench.py) on a synthetic sequence: the
# pan of tools/make_corpus.py replayed by bdex_demo --replay through the layer
# at x2; every generated frame scored, clearly better than repeating or
# cross-fading the real frames (the same frames bdex_offline makes).
#   replay_bench.sh <bdex_demo> <layer_dir>
# Skips (77) without a display or numpy.
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then echo "no display available, skipping"; exit 77; fi
python3 -c "import numpy, PIL" 2>/dev/null || { echo "numpy / Pillow missing, skipping"; exit 77; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
python3 "$ROOT/tools/make_corpus.py" "$dir/corpus" --frames 13 >/dev/null || exit 1
python3 "$ROOT/tools/replay_bench.py" "$dir/corpus/pan" --multiplier 2 --demo "$DEMO" --layer-dir "$LAYER_DIR" \
  --json "$dir/r.json" || exit 1
python3 - "$dir/r.json" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
ok = r["scored"] >= r["expected"] - 1 and r["psnr_mean"] > max(r["repeat_psnr"], r["fade_psnr"]) + 5.0
print("replay bench:", "ok" if ok else "FAILED", r)
sys.exit(0 if ok else 1)
PY
