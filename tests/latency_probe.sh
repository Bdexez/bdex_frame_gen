#!/usr/bin/env bash
# The latency measurement end to end (tools/latency_bench.sh): the demo's
# flashes joined with the layer's display log in a headless gamescope —
# every flash found on screen, the real frame no earlier than the first
# photon, and interpolation x2 holding the real frame longer than x1; and
# fps_limit as fast as the game's own limiter, a present thread's included.
# Skipped (77) without gamescope.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BDEX_BUILD="${1:?usage: latency_probe.sh <build dir>}"
command -v gamescope >/dev/null || { echo "gamescope not found: skipped"; exit 77; }
out="$("$ROOT/tools/latency_bench.sh" --seconds 6 x1=BDEX_FG_MULTIPLIER=1 interpolate=)" || exit $?
echo "$out"
row() { grep "^$1 " <<<"$out" | awk '{ print $6, $10 }'; }  # first photon, real frame medians
read -r x1first x1real <<<"$(row x1)"
read -r ifirst ireal <<<"$(row interpolate)"
for v in "$x1first" "$x1real" "$ifirst" "$ireal"; do
  [[ "$v" =~ ^[0-9.]+$ ]] || { echo "FAIL: no displayed flash (present feedback?)"; exit 1; }
done
awk -v f="$ifirst" -v r="$ireal" -v x="$x1real" 'BEGIN { exit !(r >= f && r > x) }' ||
  { echo "FAIL: interpolation's real frame ($ireal ms) should follow its first photon ($ifirst) and x1 ($x1real)"; exit 1; }
# fps_limit holds the game before its next frame, not the finished one: an
# uncapped demo capped at 40 fps by the layer must reach the screen as fast
# as the demo capping itself (until 13/10: +23 ms; the layer then stamped
# the present after its wait, and the join found no flash at all).
cap="$("$ROOT/tools/latency_bench.sh" --fps 1000 --seconds 6 x1cap=BDEX_FG_MULTIPLIER=1,BDEX_FG_FPS_LIMIT=40)" || exit $?
echo "$cap"
capreal="$(grep "^x1cap " <<<"$cap" | awk '{ print $10 }')"
[[ "$capreal" =~ ^[0-9.]+$ ]] || { echo "FAIL: fps_limit: no displayed flash (the limiter holds the present?)"; exit 1; }
awk -v c="$capreal" -v x="$x1real" 'BEGIN { exit !(c <= x + 4.0) }' ||
  { echo "FAIL: fps_limit adds latency: $capreal ms against $x1real ms with the game's own cap"; exit 1; }
# The same through a present thread, as DXVK / VKD3D-Proton present (the
# demo's --present-thread 3: VKD3D-Proton's own bound of three frames): held
# at its present wait (fps_limit_wait), the game must reach the screen as
# fast as when it caps itself — held at its present thread, it read its
# input two frames early (+50 ms at 40 fps).
pt="--present-thread 3 --mode immediate"
own="$("$ROOT/tools/latency_bench.sh" --fps 40 --seconds 6 --demo-args "$pt" ptown=BDEX_FG_MULTIPLIER=1)" || exit $?
echo "$own"
wait="$("$ROOT/tools/latency_bench.sh" --fps 1000 --seconds 6 --demo-args "$pt" ptwait=BDEX_FG_MULTIPLIER=1,BDEX_FG_FPS_LIMIT=40)" || exit $?
echo "$wait"
ownreal="$(grep "^ptown " <<<"$own" | awk '{ print $10 }')"
waitreal="$(grep "^ptwait " <<<"$wait" | awk '{ print $10 }')"
for v in "$ownreal" "$waitreal"; do
  [[ "$v" =~ ^[0-9.]+$ ]] || { echo "FAIL: present thread: no displayed flash"; exit 1; }
done
awk -v c="$waitreal" -v x="$ownreal" 'BEGIN { exit !(c <= x + 4.0) }' ||
  { echo "FAIL: fps_limit on a present thread adds latency: $waitreal ms against $ownreal ms with the game's own cap"; exit 1; }
# A game thread one frame ahead of the one that presents (Unreal, RE Engine:
# --sim-thread): held at its present wait the game still reads its input a
# frame early; NVIDIA Reflex's sleep is on the game thread, where the limit
# then holds it as its own limiter does (--reflex). Where the driver has
# VK_NV_low_latency2.
# (the output captured first: under pipefail, `vulkaninfo | grep -q` fails
# when grep finds it — SIGPIPE)
devices="$(vulkaninfo 2>/dev/null || true)"
if grep -q VK_NV_low_latency2 <<<"$devices"; then
  sim="$pt --sim-thread"
  simown="$("$ROOT/tools/latency_bench.sh" --fps 40 --seconds 6 --demo-args "$sim" simown=BDEX_FG_MULTIPLIER=1)" || exit $?
  echo "$simown"
  reflex="$("$ROOT/tools/latency_bench.sh" --fps 1000 --seconds 6 --demo-args "$sim --reflex" \
    simreflex=BDEX_FG_MULTIPLIER=1,BDEX_FG_FPS_LIMIT=40)" || exit $?
  echo "$reflex"
  # With Reflex on, Unreal hands its own frame rate limit to Reflex (the
  # driver's limiter, minimumIntervalUs): the reference of its in-game limit.
  rlimit="$("$ROOT/tools/latency_bench.sh" --fps 40 --seconds 6 --demo-args "$sim --reflex --reflex-limit" \
    simrlimit=BDEX_FG_MULTIPLIER=1)" || exit $?
  echo "$rlimit"
  simownreal="$(grep "^simown " <<<"$simown" | awk '{ print $10 }')"
  reflexreal="$(grep "^simreflex " <<<"$reflex" | awk '{ print $10 }')"
  rlimitreal="$(grep "^simrlimit " <<<"$rlimit" | awk '{ print $10 }')"
  for v in "$simownreal" "$reflexreal" "$rlimitreal"; do
    [[ "$v" =~ ^[0-9.]+$ ]] || { echo "FAIL: game thread: no displayed flash"; exit 1; }
  done
  awk -v c="$reflexreal" -v x="$simownreal" 'BEGIN { exit !(c <= x + 4.0) }' ||
    { echo "FAIL: fps_limit at Reflex's sleep adds latency: $reflexreal ms against $simownreal ms with the game's own cap"; exit 1; }
  awk -v c="$reflexreal" -v x="$rlimitreal" 'BEGIN { exit !(c <= x + 4.0) }' ||
    { echo "FAIL: fps_limit at Reflex's sleep adds latency: $reflexreal ms against $rlimitreal ms with Reflex's own limit"; exit 1; }
else
  echo "no VK_NV_low_latency2: the Reflex case skipped"
fi
echo "latency probe OK"
