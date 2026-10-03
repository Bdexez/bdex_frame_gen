#!/usr/bin/env bash
# End-to-end check: the demo rendered at 30 fps through the layer must report
# ~60 presented frames per second. Usage: integration.sh <bdex_demo> <layer_dir>
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
  if [ "${BDEX_REQUIRE_DISPLAY:-0}" = 1 ]; then
    echo "a graphical display is required for this test"
    exit 1
  fi
  echo "no display available, skipping"
  exit 77
fi
LOG="$(mktemp)"
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
trap 'rm -rf "$LOG" "$BDEX_LAYER_XDG"' EXIT
# BDEX_FG_CONFIG=/dev/null: the defaults, whatever the user's config says.
BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=2 \
  timeout 60 "$DEMO" --fps 30 --frames 150 --duration 40 --deterministic >"$LOG" 2>&1
status=$?
if [ $status -ne 0 ]; then
  if [ "${BDEX_REQUIRE_DISPLAY:-0}" != 1 ] && grep -q 'demo: glfwInit failed' "$LOG"; then
    echo "no usable graphical display, skipping"
    exit 77
  fi
  echo "demo exited with status $status"
  cat "$LOG"
  exit 1
fi
if ! grep -q "generation on" "$LOG"; then
  echo "layer did not activate frame generation"
  cat "$LOG"
  exit 1
fi
# Mean over the statistics lines, the first (start-up) left out: a line
# covers 2 s, which on a slow runner (a sanitizer build on software Vulkan:
# ~3 game fps) is ~6 frames, so one frame more or less in a window reads
# x1.86 or x2.17 while generation is steady at x2.
ratio="$(grep -o '(x[0-9.]*)' "$LOG" | tr -d '(x)' |
         awk '{ v[NR] = $1 } END { if (NR == 0) exit; s = 0; n = 0
               for (i = (NR > 1 ? 2 : 1); i <= NR; ++i) { s += v[i]; ++n }
               printf "%.2f", s / n }')"
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
