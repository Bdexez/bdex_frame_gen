#!/usr/bin/env bash
# A recording asked of a running game (the panel's / launcher's « Enregistrer
# pour le corpus »: <status dir>/<pid>.record = "<frames> <dir>"): the demo
# through the layer, generating x2; the next 40 real frames must land in
# <status dir>/<dir> as f0000.ppm.. f0039.ppm, then DONE — while generation
# goes on.
#   record_live.sh <bdex_demo> <layer_dir>
# Skips (77) without a display (Xvfb in CI, headless gamescope on a GPU).
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then echo "no display available, skipping"; exit 77; fi
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
dir="$(mktemp -d)"
trap 'kill $demo 2>/dev/null; rm -rf "$dir" "$BDEX_LAYER_XDG"' EXIT
BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=1 BDEX_FG_STATUS_DIR="$dir" \
  timeout 90 "$DEMO" --fps 30 --size 640x400 --duration 30 >"$dir/log" 2>&1 &
demo=$!
t=0
while ! grep -qF "generation on" "$dir/log" && [ $t -lt 300 ]; do sleep 0.2; t=$((t + 1)); done
pid=$(pgrep -n -P $demo || echo $demo)   # (the demo, child of timeout)
echo "40 rec-test" > "$dir/$pid.record"
t=0
while [ ! -f "$dir/rec-test/DONE" ] && [ $t -lt 300 ]; do sleep 0.2; t=$((t + 1)); done
status=0
n=$(ls "$dir/rec-test" 2>/dev/null | grep -c '^f[0-9]\{4\}\.ppm$')
[ -f "$dir/rec-test/DONE" ] || { echo "no DONE"; status=1; }
[ "$n" = 40 ] || { echo "recorded $n frames, want 40"; status=1; }
[ -f "$dir/$pid.record" ] && { echo "the request file was not taken"; status=1; }
head -c 20 "$dir/rec-test/f0000.ppm" 2>/dev/null | grep -q "^P6" || { echo "f0000.ppm is not a PPM"; status=1; }
# Times must describe the game presents, not the asynchronous file writes.
# One row per image, same order, with intervals matching the clock differences.
awk -F, '
  NR == 1 { if ($0 != "frame,present_ms,interval_ms") bad = 1; next }
  { if ($1 != sprintf("f%04d.ppm", NR - 2) || $2 <= 0) bad = 1
    if (NR == 2 && $3 != 0) bad = 1
    if (NR > 2) {
      d = $2 - prev - $3; if (d < 0) d = -d
      if ($2 <= prev || $3 <= 0 || d > 0.002) bad = 1
    }
    prev = $2 }
  END { exit (bad || NR != 41) }
' "$dir/rec-test/timestamps.csv" || { echo "recording timestamps missing or inconsistent"; status=1; }
# Generation went on (the last statistics line after the recording).
sleep 3
ratio=$(grep -E "game [0-9.]+ fps -> output" "$dir/log" | tail -1 | sed -n 's/.*(x\([0-9.]*\)).*/\1/p')
awk -v r="${ratio:-0}" 'BEGIN { exit !(r >= 1.6) }' || { echo "generation stopped (x${ratio:-?})"; status=1; }
echo "recorded $n frames ($(sed -n 's/^width=//p' "$dir/rec-test/DONE" 2>/dev/null)x$(sed -n 's/^height=//p' "$dir/rec-test/DONE" 2>/dev/null)), last x${ratio:-?}"
[ $status -eq 0 ] || grep -E "record|WARN|ERROR" "$dir/log" | tail -10
exit $status
