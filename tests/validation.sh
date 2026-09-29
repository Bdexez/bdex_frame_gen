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
. "$(dirname "$0")/../tools/layer_env.sh"
bdex_use_layer "$LAYER_DIR"
trap 'rm -rf "$LOG" "$BDEX_LAYER_XDG"' EXIT
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
for run in interpolate extrapolate interpolate:state extrapolate:state extrapolate:direct interpolate:async extrapolate:async; do
  mode=${run%%:*}
  output="BDEX_FG_RENDER_SCALE=0.75 BDEX_FG_SHARPNESS=0.5 BDEX_FG_HUD=2"
  if [ "$run" = extrapolate:direct ]; then
    state=""; demo_args="--frames 90 --duration 40"; output="BDEX_FG_HUD=0"
  elif [ "${run#*:}" = async ]; then
    state="BDEX_FG_ASYNC_COMPUTE=1 BDEX_FG_DEDUP=1 BDEX_FG_FLOW_TEMPORAL=1 BDEX_FG_ONLINE_TUNE=1 BDEX_FG_CACHE_DIR="; demo_args="--repeat 2 --frames 90 --duration 40"; output="BDEX_FG_HUD=0"
  elif [ "$run" != "$mode" ]; then
    # A tiny gpu_budget walks the whole quality ladder within the run,
    # output-stage rebuild included (level 3) unless the flow is at 1/4.
    state="BDEX_FG_FRAME_TIMING=1 BDEX_FG_DEDUP=1 BDEX_FG_SCENE_CUT_LOCAL=1 BDEX_FG_EXTRAPOLATE_ACCEL=1 BDEX_FG_CHROMA_WEIGHT=1 BDEX_FG_FLOW_TEMPORAL=1 BDEX_FG_ONLINE_TUNE=1 BDEX_FG_CACHE_DIR= BDEX_FG_UPSCALE_TEMPORAL=0.1 BDEX_FG_SUBPIXEL_LK=2 BDEX_FG_SYNTH_BICUBIC=1 BDEX_FG_GPU_BUDGET=0.001"
    demo_args="--repeat 2 --frames 300 --duration 40"
  else
    state=""; demo_args="--frames 45 --duration 40"
  fi
  env $state $output VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
  BDEX_FG_MODE=$mode BDEX_FG_MULTIPLIER=3 \
  BDEX_FG_ZERO_BIAS_CONTRAST=0.1 BDEX_FG_SUBPIXEL=1 BDEX_FG_PIXEL_VECTORS=1 BDEX_FG_EXTRAPOLATE_SPLAT=1 \
  BDEX_FG_TEMPORAL_PREDICTION=1 BDEX_FG_STATIC_GUARD=1 BDEX_FG_OCCLUSION_STRENGTH=1 \
    timeout 120 "$DEMO" --fps 30 --deterministic $demo_args >"$LOG" 2>&1
  rc=$?
  n=$(grep -cE "VUID-|SYNC-" "$LOG")
  echo "$run: demo exit $rc, $n validation message(s)"
  if [ "$n" -gt 0 ]; then grep -E "VUID-|SYNC-" "$LOG" | head -20; status=1; fi
  if [ "$rc" -ne 0 ]; then tail -20 "$LOG"; status=1; fi
  # The rebuild levels exist only if the flow is not already at 1/4.
  if [ -n "$state" ] && [ "${run#*:}" = state ] && ! grep -qE "quality level 2 -> 3|level 1 -> 2 \(flow 1/4\)" "$LOG"; then
    echo "$run: the GPU budget did not walk the quality ladder"; grep "gpu budget" "$LOG"; status=1
  fi
done
exit $status
