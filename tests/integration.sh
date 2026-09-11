#!/usr/bin/env bash
# End-to-end check: the demo rendered at 30 fps through the layer must report
# ~60 presented frames per second. Usage: integration.sh <bdex_demo> <layer_dir>
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
  echo "no display available, skipping"
  exit 77
fi
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
VK_ADD_IMPLICIT_LAYER_PATH="$LAYER_DIR" BDEX_FG=1 BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=2 \
  timeout 60 "$DEMO" --fps 30 --frames 150 --deterministic >"$LOG" 2>&1
status=$?
if [ $status -ne 0 ]; then
  echo "demo exited with status $status"
  cat "$LOG"
  exit 1
fi
if ! grep -q "generation on" "$LOG"; then
  echo "layer did not activate frame generation"
  cat "$LOG"
  exit 1
fi
ratio="$(grep -o '(x[0-9.]*)' "$LOG" | tail -1 | tr -d '(x)')"
if [ -z "$ratio" ]; then
  echo "no statistics line found"
  cat "$LOG"
  exit 1
fi
if awk -v r="$ratio" 'BEGIN { exit !(r > 1.9 && r < 2.1) }'; then
  echo "output/input frame ratio: $ratio"
  exit 0
fi
echo "unexpected frame ratio: $ratio"
cat "$LOG"
exit 1
