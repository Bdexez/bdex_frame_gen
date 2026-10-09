#!/usr/bin/env bash
# Input-to-screen latency of the demo through this build's layer, per
# setting, in a headless gamescope (no window on the desktop; present
# feedback available): automatic inputs every half second answered with a
# white frame, joined with the layer's display log (tools/latency_report.py).
#   tools/latency_bench.sh [--fps N] [--refresh HZ] [--size WxH] [--seconds S] LABEL=ENV[,ENV...] ...
# e.g.  tools/latency_bench.sh x1=BDEX_FG_MULTIPLIER=1 interp= hold=BDEX_FG_INTERP_HOLD=auto extrap=BDEX_FG_MODE=extrapolate
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BDEX_BUILD:-$ROOT/build}"
fps=40 hz=144 size=1280x720 secs=15
while [ $# -gt 0 ]; do
  case "$1" in
    --fps) fps="$2"; shift 2 ;;
    --refresh) hz="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --seconds) secs="$2"; shift 2 ;;
    *) break ;;
  esac
done
[ $# -gt 0 ] || set -- x1=BDEX_FG_MULTIPLIER=1 interpolate= interp_hold_auto=BDEX_FG_INTERP_HOLD=auto extrapolate=BDEX_FG_MODE=extrapolate
command -v gamescope >/dev/null || { echo "gamescope not found" >&2; exit 77; }
W="${size%x*}" H="${size#*x}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
mkdir -p "$work/xdg/vulkan/implicit_layer.d"
cp "$BUILD"/layer/*.json "$work/xdg/vulkan/implicit_layer.d/"
jsons=()
for spec in "$@"; do
  label="${spec%%=*}" envs="${spec#*=}"
  [ "$spec" = "$label" ] && envs=""
  IFS=',' read -r -a extra <<<"$envs"
  rm -f "$work/demo.log" "$work/layer.log"
  # (in a subshell, quiet: headless gamescope often crashes on its way out)
  ( env -u WAYLAND_DISPLAY -u DISPLAY XDG_DATA_HOME="$work/xdg" BDEX_FG_CONFIG=/dev/null \
      timeout $((secs + 25)) gamescope --backend headless -W "$W" -H "$H" -r "$hz" -- \
      env BDEX_FG=1 BDEX_FG_LATENCY_LOG="$work/layer.log" ${extra[@]+"${extra[@]}"} "$BUILD/demo/bdex_demo" --size "$size" \
      --ignore-resize --fps "$fps" --duration "$secs" --latency-test auto --latency-log "$work/demo.log" >/dev/null 2>&1; true ) \
    2>/dev/null || true
  [ -s "$work/demo.log" ] || { echo "$label: the demo wrote no flash" >&2; exit 1; }
  python3 "$ROOT/tools/latency_report.py" "$work/demo.log" "$work/layer.log" --label "$label" --json "$work/$label.json" >/dev/null
  jsons+=("$work/$label.json")
done
echo "demo at $fps fps, screen $hz Hz, $size"
python3 "$ROOT/tools/latency_report.py" --compare "${jsons[@]}"
