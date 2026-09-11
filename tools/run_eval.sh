#!/usr/bin/env bash
# Renders ground truth (60 fps) and a 30 fps test run through the layer with
# frame dumping, then prints PSNR statistics. Extra environment variables
# (BDEX_FG_*) are passed through to the test run.
#
#   tools/run_eval.sh [build_dir] [frames]
set -euo pipefail
BUILD="${1:-$(dirname "$0")/../build}"
FRAMES="${2:-40}"
BUILD="$(cd "$BUILD" && pwd)"
OUT="${BDEX_EVAL_DIR:-/tmp/bdex-eval}"
rm -rf "$OUT/gt" "$OUT/test"
mkdir -p "$OUT/gt" "$OUT/test"
export VK_ADD_IMPLICIT_LAYER_PATH="$BUILD/layer" BDEX_FG=1 BDEX_FG_LOG=0
BDEX_FG_DUMP="$OUT/gt" BDEX_FG_DUMP_FRAMES=$((FRAMES * 4)) "$BUILD/demo/bdex_demo" --fps 60 --frames $((FRAMES * 2)) --deterministic >/dev/null
BDEX_FG_DUMP="$OUT/test" BDEX_FG_DUMP_FRAMES=$((FRAMES * 2)) "$BUILD/demo/bdex_demo" --fps 30 --frames "$FRAMES" --deterministic >/dev/null
python3 "$(dirname "$0")/eval_quality.py" "$OUT/gt" "$OUT/test"
