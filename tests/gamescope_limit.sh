#!/usr/bin/env bash
# Steam's framerate limit (the Steam Deck's Quick Access Menu) in Game Mode,
# reproduced in a headless gamescope — nothing on screen: the demo at 30 fps,
# x2, through the build's layer above gamescope's WSI layer; the limit set to
# 30 fps at 7 s (gamescopectl), lifted at 14 s. Under the limit the game must
# keep its 30 fps (at x2 it fell to 15, 233 ms from present to screen); after
# it, x2 again without a queue left behind. As a native Vulkan game and as a
# "frame limiter aware" one (DXVK, VKD3D-Proton: FIFO only, swapchain
# re-created at each switch). Skipped (77) without gamescope 3.14+ and a GPU.
#   tests/gamescope_limit.sh <bdex_demo> <build>/layer
set -uo pipefail
DEMO="$1" LAYER_DIR="$2"
command -v gamescope >/dev/null && command -v gamescopectl >/dev/null || { echo "no gamescope: skipped"; exit 77; }
gamescope --help 2>&1 | grep -q headless || { echo "gamescope without a headless backend: skipped"; exit 77; }
ls /usr/share/vulkan/implicit_layer.d/*gamescope_wsi* >/dev/null 2>&1 || { echo "no gamescope WSI layer: skipped"; exit 77; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/xdg/vulkan/implicit_layer.d"
cp "$LAYER_DIR"/*.json "$work/xdg/vulkan/implicit_layer.d/"  # (XDG_DATA_HOME: gamescope's layer stays found)
cat > "$work/run.sh" <<RUN
#!/bin/sh
XDG_DATA_HOME="$work/xdg" BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=2 \\
  GAMESCOPE_WSI_FRAME_LIMITER_AWARE=\$AWARE "$DEMO" --fps 30 --frames 100000 --duration 21 >"\$LOG" 2>&1 &
pid=\$!
sleep 7; gamescopectl debug_set_fps_limit 30 >/dev/null 2>&1
sleep 7; gamescopectl debug_set_fps_limit 0 >/dev/null 2>&1
wait \$pid
RUN
chmod +x "$work/run.sh"
status=0
for aware in 0 1; do
  log="$work/demo-$aware.log"
  timeout 90 gamescope --backend headless -W 1280 -H 800 -r 60 -- env AWARE=$aware LOG="$log" "$work/run.sh" \
    >"$work/gamescope-$aware.log" 2>&1
  if ! grep -q '^\[Gamescope WSI\]' "$log"; then
    echo "gamescope's WSI layer not loaded here: skipped"; exit 77
  fi
  if ! grep -q "presenting through gamescope's WSI layer: refresh rate" "$log"; then
    echo "FAIL aware=$aware: the layer did not take gamescope's refresh cycle"; status=1; continue
  fi
  # Statistics lines: time, game fps, output fps, present-to-screen.
  stats="$(sed -nE 's/^\[bdex-fg +([0-9.]+) INFO\] game ([0-9.]+) fps -> output ([0-9.]+) fps.*present-to-screen ([0-9.]+) ms.*/\1 \2 \3 \4/p' "$log")"
  verdict="$(awk -v aware=$aware '
    $1 >= 3 && $1 < 6.9  { a++; ar += $3 / $2 }
    $1 >= 10 && $1 < 13.9 { b++; bg += $2; bo += $3; bl += $4 }
    $1 >= 17.5           { c++; cr += $3 / $2; cl += $4 }
    END {
      if (!a || !b || !c) { print "FAIL: statistics missing"; exit }
      ar /= a; bg /= b; bo /= b; bl /= b; cr /= c; cl /= c
      ok = ar >= 1.9 && bg >= 27 && bo <= 32 && bl < 110 && cr >= 1.9 && cl < 70
      printf "%s aware=%d: before x%.2f; limit 30: game %.1f fps, output %.1f, present-to-screen %.0f ms; after x%.2f, %.0f ms\n",
             ok ? "ok" : "FAIL", aware, ar, bg, bo, bl, cr, cl
    }' <<<"$stats")"
  echo "$verdict"
  case "$verdict" in ok*) ;; *) status=1; grep -E 'fps ->|frame limit' "$log" | cut -c1-200 ;; esac
done
exit $status
