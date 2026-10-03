#!/usr/bin/env bash
# Settings changed in the configuration file while a game runs without the
# control panel (require_app = 0: Steam's Game Mode, where the Steam Deck
# menu edits the file): the demo through the layer, then x3, extrapolation,
# require_app = 1 (the game pauses) and back to 0 (it resumes) — each must
# reach the running game: output stage rebuilt, the frame rate following.
#   live_config.sh <bdex_demo> <layer_dir>
# Skips (77) without a display (Xvfb in CI, headless gamescope on a GPU).
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then echo "no display available, skipping"; exit 77; fi
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
dir="$(mktemp -d)"
trap 'rm -rf "$dir" "$BDEX_LAYER_XDG"' EXIT
conf="$dir/conf"
printf '[*]\nenabled = 1\nrequire_app = 0\nmultiplier = 2\n' > "$conf"
BDEX_FG_CONFIG="$conf" BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=1 BDEX_FG_STATUS_DIR="$dir" \
  timeout 90 "$DEMO" --fps 30 --size 640x400 --duration 26 >"$dir/log" 2>&1 &
demo=$!
# (each step waits for the previous one's rebuild: lavapipe compiles slowly)
step() {
  local want="$1" t=0
  while ! grep -qF "$want" "$dir/log" && [ $t -lt 100 ]; do sleep 0.2; t=$((t + 1)); done
  sleep 2
}
step "generation on"
sed -i 's/multiplier = 2/multiplier = 3/' "$conf"
step "rebuilt (x3 interpolate, generation on)"
echo 'mode = extrapolate' >> "$conf"
step "rebuilt (x3 extrapolate, generation on)"
sed -i 's/require_app = 0/require_app = 1/' "$conf"
step "(x3 extrapolate, generation paused)"
sed -i 's/require_app = 1/require_app = 0/' "$conf"
wait $demo
status=0
for want in "rebuilt (x3 interpolate, generation on)" "rebuilt (x3 extrapolate, generation on)" \
            "(frame generation paused)" "(frame generation resumed)"; do
  grep -qF "$want" "$dir/log" || { echo "missing: $want"; status=1; }
done
# The last statistics line: generating again at x3 (lavapipe may fall short).
ratio=$(grep -E "game [0-9.]+ fps -> output" "$dir/log" | tail -1 | sed -n 's/.*(x\([0-9.]*\)).*/\1/p')
echo "settings applied live without the panel; last x${ratio:-?}"
awk -v r="${ratio:-0}" 'BEGIN { exit !(r >= 1.8) }' || { echo "generation did not resume"; status=1; }
[ $status -eq 0 ] || grep -E "rebuilt|fps -> output|changed|paused|resumed" "$dir/log" | tail -20
exit $status
