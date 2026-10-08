#!/usr/bin/env bash
# Runs the demo through the layer under the Khronos validation layer, with
# synchronization validation, in interpolation and extrapolation with every
# optional pass enabled (upscaling + CAS, HUD, and the opt-in flow/synthesis
# settings), and fails on any VUID or SYNC message. The validation layer sits
# below this layer in the chain, so it checks the layer's own commands.
# Skips (77) without a display or without VK_LAYER_KHRONOS_validation.
set -u
DEMO="$1"
LAYER_DIR="$2"
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
  echo "no display available, skipping"; exit 77
fi
if ! vulkaninfo 2>/dev/null | grep -q VK_LAYER_KHRONOS_validation; then
  echo "VK_LAYER_KHRONOS_validation not installed, skipping"; exit 77
fi
LOG="$(mktemp)"
# Validation layers before 1.3.290 (Ubuntu 24.04 ships 1.3.275) report
# SYNC-HAZARD-WRITE-AFTER-PRESENT on the present of a real swapchain image the
# worker acquired while holding others acquired (a present's frames are
# acquired and copied ahead, then presented at their times). Traced in that
# container: every image goes acquire -> copy (waiting the acquire semaphore)
# -> present, and is acquired again only after its present; the image count
# allows the images held (imageCount - minImageCount + 1). 1.4.357 reports
# nothing on the same runs. Ignored with those old layers only.
IGNORE='^$'
vvl=$(vulkaninfo 2>/dev/null | grep -m1 "VK_LAYER_KHRONOS_validation" | grep -oE "Vulkan version [0-9.]+" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+")
if [ -n "$vvl" ] && [ "$(printf '%s\n1.3.290\n' "$vvl" | sort -V | head -1)" = "$vvl" ] && [ "$vvl" != "1.3.290" ]; then
  IGNORE='SYNC-HAZARD-WRITE-AFTER-PRESENT'
  echo "validation layer $vvl: ignoring $IGNORE (false positive of layers before 1.3.290, see the script)"
fi
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
# online_tune decisions for the flow options, so that the "probe" runs start
# with the synthesis options (the held-out synthesis probe) at once.
TUNE_CACHE="$(mktemp -d)"
printf 'zero_bias_contrast 0.1 dropped 0 0\ntemporal_prediction 1 dropped 0 0\nflow_temporal 1 dropped 0 0\n' \
  > "$TUNE_CACHE/online-tune-bdex_demo-interpolate.txt"
printf 'zero_bias_contrast 0.1 dropped 0 0\ntemporal_prediction 1 dropped 0 0\nsubpixel 1 dropped 0 0\nflow_temporal 1 dropped 0 0\nsubpixel_lk 2 dropped 0 0\n' \
  > "$TUNE_CACHE/online-tune-bdex_demo-extrapolate.txt"
trap 'rm -rf "$LOG" "$BDEX_LAYER_XDG" "$TUNE_CACHE"' EXIT
status=0
# Second pass per mode: the per-frame state options (content-time t, repeated
# frames, per-region scene cuts, acceleration, chroma, flow_temporal,
# online_tune, upscale_temporal, GPU budget), on a demo that presents every
# image twice so the repeat path (gated flow dispatches,
# extrapolated real frame) runs too. "direct": extrapolation with the real
# frame copied straight (no upscaling / HUD), the path where the worker
# submits the synthesis after the real frame.
# "async": the synthesis on a compute-only queue family (async_compute),
# where the device has one — its cross-queue semaphores under sync validation.
# "probe": online_tune's synthesis probe (flow copies, composed flows, a
# synthesis of its own, the comparison), from the primed cache above.
for run in interpolate extrapolate hybrid interpolate:state extrapolate:state hybrid:state extrapolate:direct \
           interpolate:async extrapolate:async interpolate:probe extrapolate:probe hybrid:adaptive interpolate:x8 interpolate:hold hybrid:hold; do
  mode=${run%%:*}
  mult=3
  output="BDEX_FG_RENDER_SCALE=0.75 BDEX_FG_SHARPNESS=0.5 BDEX_FG_HUD=2"
  if [ "$run" = extrapolate:direct ]; then
    state=""; demo_args="--frames 90 --duration 40"; output="BDEX_FG_HUD=0"
  elif [ "${run#*:}" = probe ]; then
    state="BDEX_FG_ONLINE_TUNE=1 BDEX_FG_CACHE_DIR=$TUNE_CACHE BDEX_FG_HUD_MASK=24"; demo_args="--frames 120 --duration 40"
    output="BDEX_FG_RENDER_SCALE=0.75 BDEX_FG_HUD=2"
  elif [ "${run#*:}" = x8 ]; then
    # Eight frames a present: more than most surfaces let the worker hold
    # acquired at once, so part of them go through the staging images.
    state=""; mult=8; demo_args="--frames 40 --duration 40"
  elif [ "${run#*:}" = adaptive ]; then
    # 30 fps into 75: 2, 3, 2, 3... frames a game frame, one output period apart.
    state="BDEX_FG_ADAPTIVE=1 BDEX_FG_TARGET_FPS=75"; demo_args="--frames 60 --duration 40"
  elif [ "${run#*:}" = hold ]; then
    # interp_hold: the frames before the real one closer together (their t
    # from the display times, frame_timing on).
    state="BDEX_FG_INTERP_HOLD=0.3 BDEX_FG_FRAME_TIMING=1"; mult=4; demo_args="--frames 60 --duration 40"
  elif [ "${run#*:}" = async ]; then
    state="BDEX_FG_ASYNC_COMPUTE=1 BDEX_FG_DEDUP=1 BDEX_FG_FLOW_TEMPORAL=1 BDEX_FG_ONLINE_TUNE=1 BDEX_FG_CACHE_DIR= BDEX_FG_HUD_MASK=24 BDEX_FG_SUBPIXEL_LK=2"; demo_args="--repeat 2 --frames 90 --duration 40"; output="BDEX_FG_HUD=0"
  elif [ "$run" != "$mode" ]; then
    # A tiny gpu_budget walks the whole quality ladder within the run,
    # output-stage rebuild included (level 3) unless the flow is at 1/4.
    state="BDEX_FG_FRAME_TIMING=1 BDEX_FG_DEDUP=1 BDEX_FG_SCENE_CUT_LOCAL=1 BDEX_FG_EXTRAPOLATE_ACCEL=1 BDEX_FG_CHROMA_WEIGHT=1 BDEX_FG_FLOW_TEMPORAL=1 BDEX_FG_ONLINE_TUNE=1 BDEX_FG_CACHE_DIR= BDEX_FG_UPSCALE_TEMPORAL=0.1 BDEX_FG_SUBPIXEL_LK=2 BDEX_FG_SYNTH_BICUBIC=1 BDEX_FG_HUD_MASK=24 BDEX_FG_REVEAL_FILL=1 BDEX_FG_GPU_BUDGET=0.001"
    demo_args="--repeat 2 --frames 300 --duration 40"
  else
    state=""; demo_args="--frames 45 --duration 40"
  fi
  env $state $output VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
  BDEX_FG_MODE=$mode BDEX_FG_MULTIPLIER=$mult \
  BDEX_FG_ZERO_BIAS_CONTRAST=0.1 BDEX_FG_SUBPIXEL=1 BDEX_FG_PIXEL_VECTORS=1 BDEX_FG_EXTRAPOLATE_SPLAT=1 \
  BDEX_FG_TEMPORAL_PREDICTION=1 BDEX_FG_STATIC_GUARD=1 BDEX_FG_OCCLUSION_STRENGTH=1 \
    timeout 120 "$DEMO" --fps 30 --deterministic $demo_args >"$LOG" 2>&1
  rc=$?
  n=$(grep -E "VUID-|SYNC-" "$LOG" | grep -cvE "$IGNORE")
  echo "$run: demo exit $rc, $n validation message(s)"
  if [ "$n" -gt 0 ]; then grep -E "VUID-|SYNC-" "$LOG" | grep -vE "$IGNORE" | head -20; status=1; fi
  if [ "$rc" -ne 0 ]; then tail -20 "$LOG"; status=1; fi
  # The rebuild levels exist only if the flow is not already at 1/4.
  if [ -n "$state" ] && [ "${run#*:}" = state ] && ! grep -qE "quality level 2 -> 3|level 1 -> 2 \(flow 1/4\)" "$LOG"; then
    echo "$run: the GPU budget did not walk the quality ladder"; grep "gpu budget" "$LOG"; status=1
  fi
done

# "panel": the control panel's session (require_app), like the Windows
# launcher — the game starts armed, « Démarrer » targets it, the options
# change while it runs (multiplier, mode, HUD: the output stage is rebuilt),
# « Arrêter » stops it. A sleeping process and a touch loop stand in for the panel.
PANEL_DIR="$(mktemp -d)"
printf '[*]\nenabled = 1\nrequire_app = 1\nmultiplier = 2\noverlay = 2\n' > "$PANEL_DIR/conf"
bash -c 'exec -a bdex-framegen-gui sleep 120' & panel=$!
echo "$panel 0" > "$PANEL_DIR/app.session"
( while kill -0 $panel 2>/dev/null; do touch "$PANEL_DIR/app.session"; sleep 1; done ) &  # the heartbeat
env VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true \
  BDEX_FG_CONFIG="$PANEL_DIR/conf" BDEX_FG_STATUS_DIR="$PANEL_DIR" BDEX_FG_LOG=2 \
  timeout 120 "$DEMO" --fps 30 --deterministic --frames 400 --duration 40 >"$LOG" 2>&1 &
demo=$!
sleep 3; echo "$panel $(pgrep -n -P $demo)" > "$PANEL_DIR/app.session"  # the demo, child of timeout
sleep 3; sed -i 's/multiplier = 2/multiplier = 4/; s/overlay = 2/overlay = 0/' "$PANEL_DIR/conf"
sleep 3; echo 'mode = extrapolate' >> "$PANEL_DIR/conf"
sleep 3; echo "$panel 0" > "$PANEL_DIR/app.session"
wait $demo; rc=$?
kill $panel 2>/dev/null
n=$(grep -E "VUID-|SYNC-" "$LOG" | grep -cvE "$IGNORE")
echo "panel: demo exit $rc, $n validation message(s)"
if [ "$n" -gt 0 ]; then grep -E "VUID-|SYNC-" "$LOG" | grep -vE "$IGNORE" | head -20; status=1; fi
if [ "$rc" -ne 0 ]; then tail -20 "$LOG"; status=1; fi
for want in "generation target changed (frame generation resumed)" "rebuilt (x4 interpolate, generation on)" \
            "rebuilt (x4 extrapolate, generation on)" "generation target changed (frame generation paused)"; do
  if ! grep -qF "$want" "$LOG"; then echo "panel: missing '$want'"; grep "control panel" "$LOG"; status=1; fi
done
rm -rf "$PANEL_DIR"
exit $status
