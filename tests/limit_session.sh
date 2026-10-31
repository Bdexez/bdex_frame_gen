#!/usr/bin/env bash
# fps_limit under the control panel's model (require_app = 1) holds only the
# game the panel started: the demo (60 fps of its own) armed with a 20 fps
# limit in the file runs free while the panel has not started it, is held at
# 20 once « Démarrer » targets it, runs free again after « Arrêter », and
# after the panel closes (its session no longer refreshed). Until 31/10 the
# limit held every armed game, started or not, and stayed after the panel
# closed. A sleeping process and a touch loop stand in for the panel.
#   limit_session.sh <bdex_demo> <layer_dir> [demo args...]
# (with --present-thread 3: presented as DXVK / VKD3D-Proton do — where the
# driver has present wait, « Démarrer » makes it take FIFO and the limit
# holds it at its present wait)
# Skips (77) without a display (Xvfb in CI, headless gamescope on a GPU).
set -u
DEMO="$1"
LAYER_DIR="$2"
shift 2
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then echo "no display available, skipping"; exit 77; fi
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
dir="$(mktemp -d)"
panel=""; beat=""
trap 'kill $panel $beat 2>/dev/null; rm -rf "$dir" "$BDEX_LAYER_XDG"' EXIT
printf '[*]\nenabled = 1\nrequire_app = 1\nmultiplier = 2\nfps_limit = 20\n' > "$dir/conf"
bash -c 'exec -a bdex-framegen-gui sleep 120' & panel=$!
echo "$panel 0" > "$dir/app.session"
( while kill -0 $panel 2>/dev/null; do touch "$dir/app.session"; sleep 1; done ) & beat=$!  # the heartbeat
BDEX_FG_CONFIG="$dir/conf" BDEX_FG_STATUS_DIR="$dir" BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=1 \
  timeout 90 "$DEMO" --fps 60 --size 640x400 --duration 40 "$@" >"$dir/log" 2>&1 &
demo=$!
# The game's frame rate at the end of a phase: the layer's last statistics line.
rate() { grep -E "game [0-9.]+ fps -> output" "$dir/log" | tail -1 | sed -n 's/.*game \([0-9.]*\) fps.*/\1/p'; }
t=0
while ! grep -qE "game [0-9.]+ fps -> output" "$dir/log" && [ $t -lt 150 ]; do sleep 0.2; t=$((t + 1)); done
sleep 4
armed=$(rate)
game=$(pgrep -n -P $demo)
echo "$panel $game" > "$dir/app.session"   # « Démarrer »
sleep 6
started=$(rate)
echo "$panel 0" > "$dir/app.session"       # « Arrêter »
sleep 6
stopped=$(rate)
echo "$panel $game" > "$dir/app.session"   # « Démarrer » again, then the panel closes
sleep 6
kill $beat $panel 2>/dev/null
sleep 9                                    # (a session older than 5 s: the panel is gone)
closed=$(rate)
kill $demo 2>/dev/null
wait $demo 2>/dev/null
echo "game fps: armed ${armed:-?}, started ${started:-?}, stopped ${stopped:-?}, panel closed ${closed:-?} (limit 20, the demo's own 60)"
status=0
free() { awk -v r="${1:-0}" 'BEGIN { exit !(r > 35) }'; }
held() { awk -v r="${1:-0}" 'BEGIN { exit !(r > 15 && r < 25) }'; }
free "$armed" || { echo "FAIL: the limit held a game the panel had not started"; status=1; }
held "$started" || { echo "FAIL: the limit did not hold the started game"; status=1; }
free "$stopped" || { echo "FAIL: the limit stayed after « Arrêter »"; status=1; }
free "$closed" || { echo "FAIL: the limit stayed after the panel closed"; status=1; }
[ $status -eq 0 ] || grep -E "fps -> output|control panel|paused|resumed" "$dir/log" | tail -20
exit $status
