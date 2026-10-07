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
# Over the statistics lines, the first (start-up) left out. A present shows
# the real frame and a generated one — or the real frame alone when the
# layer catches up ("N frame(s) without generation to catch up": the game
# waited for the previous synthesis; a CPU-starved runner, where lavapipe's
# synthesis competes with the game for the CPU). The output is then
# 2 x game - catch-ups: that is what is checked, catch-ups kept under half
# the frames. A line covers 2 s, which on a slow runner (a sanitizer build on
# software Vulkan: ~3 game fps) is ~6 frames, so one frame more or less in a
# window reads x1.86 or x2.17 while generation is steady: means over the
# lines.
read -r ratio expected behind < <(grep -E 'game [0-9.]+ fps -> output' "$LOG" | awk -v T=2 '
  { g = o = c = 0
    for (i = 1; i <= NF; ++i) {
      if ($i == "game") g = $(i + 1)
      if ($i == "output") o = $(i + 1)
      if ($(i + 1) == "frame(s)" && $(i + 2) == "without") c = $i
    }
    if (g <= 0) next
    ++n; r[n] = o / g; f[n] = c / (g * T) }
  END { if (n == 0) exit; s = e = b = 0; k = 0
        for (i = (n > 1 ? 2 : 1); i <= n; ++i) { s += r[i]; b += f[i]; ++k }
        s /= k; b /= k; e = 2 - (b > 1 ? 1 : b)
        printf "%.2f %.2f %.2f\n", s, e, b }')
if [ -z "${ratio:-}" ]; then
  echo "no statistics line found"
  cat "$LOG"
  exit 1
fi
if awk -v r="$ratio" -v e="$expected" -v b="$behind" 'BEGIN { exit !(r > e - 0.1 && r < e + 0.1 && b <= 0.5) }'; then
  if [ "$expected" = 2.00 ]; then
    echo "output/input frame ratio: $ratio"
  else
    echo "output/input frame ratio: $ratio (expected $expected: $(awk -v b="$behind" 'BEGIN { printf "%.0f", b * 100 }')% of the presents without generation to catch up)"
  fi
  exit 0
fi
echo "unexpected frame ratio: $ratio (expected $expected, catch-up on $behind of the presents)"
cat "$LOG"
exit 1
