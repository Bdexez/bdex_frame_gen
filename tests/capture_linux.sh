#!/usr/bin/env bash
# bdex-capture (Linux) on the demo, the layer left out: the demo's window is
# captured (XComposite + XDamage + MIT-SHM), frames are generated and shown in
# the overlay above it. Checks the output/capture ratio (x2) and that the
# overlay shows the game: its pixels against the game window's own content
# (tests/x11_compare.py, through Xlib) — the same scene, not black.
#   capture_linux.sh <bdex-capture> <bdex_demo>
# Skips (77) without an X display (Xvfb in CI; never the user's desktop:
# run it under xvfb-run or in Docker).
set -u
CAP="$1"
DEMO="$2"
if [ -z "${DISPLAY:-}" ]; then echo "no X display, skipping"; exit 77; fi
# Inside gamescope (headless, the GPU bench) its WSI layer presents the
# programs' frames to gamescope directly, past the X window the capture reads:
# off for the demo and the capture app, as on an X11 / Xwayland desktop.
unset ENABLE_GAMESCOPE_WSI
LOG="$(mktemp)" DLOG="$(mktemp)"
trap 'kill $demo 2>/dev/null; rm -f "$LOG" "$DLOG"' EXIT
BDEX_FG=0 BDEX_FG_CONFIG=/dev/null timeout 120 "$DEMO" --fps 30 --size 640x400 --duration 90 >"$DLOG" 2>&1 &
demo=$!
sleep 2
dpid=$(pgrep -n -P $demo || echo $demo)   # (the demo, child of timeout; its title changes every 2 s)
status=0
# BDEX_TEST_COMPOSITOR=1 (CI, on its own virtual display — it replaces the
# window manager): a second pass under openbox and picom, the situation of
# an X11 desktop, whose compositor already redirects every window.
passes="plain"
if [ "${BDEX_TEST_COMPOSITOR:-0}" = 1 ] && command -v openbox >/dev/null && command -v picom >/dev/null; then
  passes="plain compositor"
fi
for pass in $passes; do
if [ "$pass" = compositor ]; then
  openbox >/dev/null 2>&1 & wm=$!
  sleep 1
  picom --backend xrender >/dev/null 2>&1 & comp=$!
  sleep 2
  echo "-- under openbox + picom"
fi
for args in "-m 2" "-m 3 --mode hybrid" "-m 2 --mode extrapolate"; do
  # (The capture's own config only: the user's file and environment stay out.)
  BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 timeout 40 "$CAP" --pid "$dpid" --duration 7 $args \
    >"$LOG" 2>&1 &
  cap=$!
  sleep 5
  cmp="$(python3 "$(dirname "$0")/x11_compare.py" "demo" "bdex-capture overlay" 2>&1)"
  wait $cap
  rc=$?
  want=2; case "$args" in *"-m 3"*) want=3 ;; esac
  # A software renderer (lavapipe, CI) shows the real frame ~25 ms after the
  # game's: the predicted frames (hybrid, extrapolation) are then later than
  # the next game frame and rightly dropped. Measured on a GPU (headless
  # gamescope without its WSI layer): x2.00, x2.97, x2.00.
  if grep -q "(software" "$LOG"; then case "$args" in *hybrid*) want=2 ;; *extrapolate*) want=1 ;; esac; fi
  ratio=$(grep -E "game [0-9.]+ fps -> output" "$LOG" | tail -1 | sed -n 's/.*(x\([0-9.]*\)).*/\1/p')
  game=$(grep -E "game [0-9.]+ fps -> output" "$LOG" | tail -1 | sed -n 's/.*game \([0-9.]*\) fps.*/\1/p')
  echo "bdex-capture $args: exit $rc, game ${game:-?} fps, x${ratio:-?} (want x$want); $cmp"
  if [ $rc -ne 0 ] || ! awk -v r="${ratio:-0}" -v w="$want" -v g="${game:-0}" \
       'BEGIN { exit !(r >= 1 + (w - 1) * 0.7 && g >= 10) }'; then
    grep -E "ERROR|WARN|fps -> output|capturing|gpu|engine" "$LOG" | tail -12; status=1
  fi
  case "$cmp" in "same scene"*) ;; *) echo "  the overlay does not show the game"; status=1 ;; esac
done
[ "$pass" = compositor ] && kill $comp $wm 2>/dev/null
done
kill $demo 2>/dev/null   # (it would run for the rest of its 90 s)
grep -q "demo:" "$DLOG" || { echo "demo did not run:"; cat "$DLOG"; status=1; }
exit $status
