#!/usr/bin/env bash
# Renders ground truth (30*multiplier fps) and a 30 fps test run through the layer with
# frame dumping, then prints PSNR statistics. Extra environment variables
# (BDEX_FG_*) are passed through to the test run; BDEX_EVAL_ARGS adds demo
# arguments to both runs (e.g. "--speed 2").
#
#   tools/run_eval.sh [build_dir] [frames]
set -euo pipefail
BUILD="${1:-$(dirname "$0")/../build}"
FRAMES="${2:-40}"
MULT="${BDEX_FG_MULTIPLIER:-2}"
case "$MULT" in 2|3|4) ;; *) echo "BDEX_FG_MULTIPLIER must be 2, 3 or 4" >&2; exit 2 ;; esac
case "$FRAMES" in ''|*[!0-9]*) echo "frames must be an integer >= 2" >&2; exit 2 ;; esac
if [ "$FRAMES" -lt 2 ]; then echo "frames must be an integer >= 2" >&2; exit 2; fi
BUILD="$(cd "$BUILD" && pwd)"
# On disk, not in /tmp: /tmp is often a tmpfs, and hundreds of MB of dumps in
# RAM can get a game killed by the OOM killer on a small machine.
OUT="${BDEX_EVAL_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/bdex-eval}"
# Test exactly this build even if an older implicit layer is installed.
# shellcheck source=layer_env.sh
. "$(dirname "$0")/layer_env.sh"
bdex_use_layer "$BUILD/layer"
trap 'rm -rf "$BDEX_LAYER_XDG"' EXIT
rm -rf "$OUT/gt" "$OUT/test"
mkdir -p "$OUT/gt" "$OUT/test"
export BDEX_FG=1 BDEX_FG_LOG=0 BDEX_FG_HUD=0 BDEX_FG_RENDER_SCALE=1
# Do not inherit a game profile or global experimental knobs from the user's
# config file. Explicit BDEX_FG_* environment overrides remain available for
# A/B experiments (including a caller-set preset).
export BDEX_FG_CONFIG=/dev/null
export BDEX_FG_PRESET="${BDEX_FG_PRESET:-balanced}"
BDEX_FG_MULTIPLIER=1 BDEX_FG_DUMP="$OUT/gt" BDEX_FG_DUMP_FRAMES=$((FRAMES * MULT)) \
  "$BUILD/demo/bdex_demo" --fps $((30 * MULT)) --frames $((FRAMES * MULT)) --deterministic ${BDEX_EVAL_ARGS:-} >/dev/null
BDEX_FG_MULTIPLIER="$MULT" BDEX_FG_DUMP="$OUT/test" BDEX_FG_DUMP_FRAMES=$((FRAMES * MULT)) \
  "$BUILD/demo/bdex_demo" --fps 30 --frames "$FRAMES" --deterministic ${BDEX_EVAL_ARGS:-} >/dev/null
# The first rendered frame has no predecessor. Require 75% (rounded down) of
# the remaining generated frames to align with ground truth; a one-frame sample
# must never pass the CI quality threshold as if it represented the run.
EXPECTED_GENERATED=$(( (FRAMES - 1) * (MULT - 1) ))
MIN_COMPARABLE=$(( EXPECTED_GENERATED * 3 / 4 ))
if [ "$MIN_COMPARABLE" -lt 1 ]; then MIN_COMPARABLE=1; fi
python3 "$(dirname "$0")/eval_quality.py" "$OUT/gt" "$OUT/test" "$MULT" \
  --expected-generated "$EXPECTED_GENERATED" --min-comparable "$MIN_COMPARABLE"
