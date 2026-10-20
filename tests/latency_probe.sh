#!/usr/bin/env bash
# The latency measurement end to end (tools/latency_bench.sh): the demo's
# flashes joined with the layer's display log in a headless gamescope —
# every flash found on screen, the real frame no earlier than the first
# photon, and interpolation x2 holding the real frame longer than x1.
# Skipped (77) without gamescope.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BDEX_BUILD="${1:?usage: latency_probe.sh <build dir>}"
command -v gamescope >/dev/null || { echo "gamescope not found: skipped"; exit 77; }
out="$("$ROOT/tools/latency_bench.sh" --seconds 6 x1=BDEX_FG_MULTIPLIER=1 interpolate=)" || exit $?
echo "$out"
row() { grep "^$1 " <<<"$out" | awk '{ print $6, $10 }'; }  # first photon, real frame medians
read -r x1first x1real <<<"$(row x1)"
read -r ifirst ireal <<<"$(row interpolate)"
for v in "$x1first" "$x1real" "$ifirst" "$ireal"; do
  [[ "$v" =~ ^[0-9.]+$ ]] || { echo "FAIL: no displayed flash (present feedback?)"; exit 1; }
done
awk -v f="$ifirst" -v r="$ireal" -v x="$x1real" 'BEGIN { exit !(r >= f && r > x) }' ||
  { echo "FAIL: interpolation's real frame ($ireal ms) should follow its first photon ($ifirst) and x1 ($x1real)"; exit 1; }
# fps_limit holds the game before its next frame, not the finished one: an
# uncapped demo capped at 40 fps by the layer must reach the screen as fast
# as the demo capping itself (until 13/10: +23 ms; the layer then stamped
# the present after its wait, and the join found no flash at all).
cap="$("$ROOT/tools/latency_bench.sh" --fps 1000 --seconds 6 x1cap=BDEX_FG_MULTIPLIER=1,BDEX_FG_FPS_LIMIT=40)" || exit $?
echo "$cap"
capreal="$(grep "^x1cap " <<<"$cap" | awk '{ print $10 }')"
[[ "$capreal" =~ ^[0-9.]+$ ]] || { echo "FAIL: fps_limit: no displayed flash (the limiter holds the present?)"; exit 1; }
awk -v c="$capreal" -v x="$x1real" 'BEGIN { exit !(c <= x + 4.0) }' ||
  { echo "FAIL: fps_limit adds latency: $capreal ms against $x1real ms with the game's own cap"; exit 1; }
echo "latency probe OK"
