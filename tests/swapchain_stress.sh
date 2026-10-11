#!/usr/bin/env bash
# What games do to their swapchain, through the layer: the demo's --stress
# modes (window resizes, swapchains recreated from the old one with other
# present modes and image counts, surfaces and windows destroyed and made
# again, fullscreen toggles, minimization) every few frames, then a steady
# stretch. Each run must exit cleanly, under the Khronos validation layer
# when it is installed (no VUID or SYNC message), and generate again at x2
# once the storm is over.
#   swapchain_stress.sh <bdex_demo> <layer_dir>
# Skips (77) without a display. Display tests run in headless gamescope
# (see AGENTS.md), never on the user's desktop.
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
  echo "no display available, skipping"; exit 77
fi
VALIDATION=""
if vulkaninfo 2>/dev/null | grep -q VK_LAYER_KHRONOS_validation; then
  VALIDATION="VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation"
else
  echo "VK_LAYER_KHRONOS_validation not installed: crashes and generation only"
fi
# The same false positive of old validation layers as validation.sh.
IGNORE='^$'
vvl=$(vulkaninfo 2>/dev/null | grep -m1 "VK_LAYER_KHRONOS_validation" | grep -oE "Vulkan version [0-9.]+" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+")
if [ -n "$vvl" ] && [ "$(printf '%s\n1.3.290\n' "$vvl" | sort -V | head -1)" = "$vvl" ] && [ "$vvl" != "1.3.290" ]; then
  IGNORE='SYNC-HAZARD-WRITE-AFTER-PRESENT'
fi
LOG="$(mktemp)"
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
trap 'rm -rf "$LOG" "$BDEX_LAYER_XDG"' EXIT
status=0
# kind:every  layer settings
runs=(
  "resize:7"
  "recreate:5"
  "surface:23"
  "window:41"
  "fullscreen:17"
  "minimize:29"
  "resize:11,recreate:13,surface:31,fullscreen:19|BDEX_FG_MODE=extrapolate"
  "resize:9,recreate:7|BDEX_FG_MODE=hybrid BDEX_FG_MULTIPLIER=3 BDEX_FG_RENDER_SCALE=0.75 BDEX_FG_HUD=2"
  "recreate:6,resize:13|BDEX_FG_MULTIPLIER=8"
  "resize:10,recreate:9|BDEX_FG_ADAPTIVE=1 BDEX_FG_MULTIPLIER=4 BDEX_FG_TARGET_FPS=75 BDEX_FG_PRESENT_QUEUE=0"
)
for run in "${runs[@]}"; do
  stress=${run%%|*}
  settings=""; [ "$stress" != "$run" ] && settings=${run#*|}
  # 30 fps: 150 frames of storm, then 90 steady ones (3 s, the last
  # statistics lines).
  env $VALIDATION VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
    BDEX_FG_STATS_INTERVAL=1 $settings \
    timeout 120 "$DEMO" --fps 30 --size 640x400 --frames 240 --duration 60 --stress "$stress" --stress-until 150 \
    >"$LOG" 2>&1
  rc=$?
  if [ $rc -ne 0 ] && grep -q 'demo: glfwInit failed' "$LOG"; then echo "no usable display, skipping"; exit 77; fi
  n=$(grep -E "VUID-|SYNC-" "$LOG" | grep -cvE "$IGNORE")
  events=$(grep -c "demo: stress" "$LOG")
  swaps=$(grep -c "demo: swapchain" "$LOG")
  # The last statistics line (the steady stretch).
  ratio=$(grep -E "game [0-9.]+ fps -> output" "$LOG" | tail -1 | sed -n 's/.*(x\([0-9.]*\)).*/\1/p')
  want=2; case "$settings" in *ADAPTIVE*) want=2.5 ;; *MULTIPLIER=3*) want=3 ;; *MULTIPLIER=8*) want=8 ;; esac
  echo "$stress ${settings:+[$settings] }: exit $rc, $events stress events, $swaps swapchains, $n validation message(s), last x${ratio:-?} (want ~x$want)"
  bad=0
  [ $rc -ne 0 ] && bad=1
  [ "$n" -gt 0 ] && { grep -E "VUID-|SYNC-" "$LOG" | grep -vE "$IGNORE" | sort | uniq -c | sort -rn | head -10; bad=1; }
  [ "$events" -lt 3 ] && { echo "  too few stress events"; bad=1; }
  grep -q "generation on" "$LOG" || { echo "  no generation"; bad=1; }
  # A lower bound only: lavapipe under sanitizers or a busy runner falls
  # behind (catch-ups), and so does synchronization validation at x8 (240
  # presents a second, each validated: x4.5 where the same run without it
  # gets x8.00) — what is checked is that generation came back.
  min=$(awk -v w="$want" -v v="$VALIDATION" 'BEGIN { m = 1 + (w - 1) * 0.6; if (v != "" && m > 1.5) m = 1.5; print m }')
  awk -v r="${ratio:-0}" -v m="$min" 'BEGIN { exit !(r >= m) }' ||
    { echo "  generation did not recover after the storm"; bad=1; }
  if [ $bad -ne 0 ]; then
    grep -E "bdex-fg.*(WARN|ERROR)|demo:" "$LOG" | tail -15
    status=1
  fi
done
exit $status
