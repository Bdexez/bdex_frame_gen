#!/usr/bin/env bash
# The 32-bit layer at run time — the path of 32-bit games (old titles, 32-bit
# DXVK under Proton), built by every CI run but never run before: in a Debian
# container with the i386 architecture, the layer is built with -m32 and a
# 32-bit vkcube runs through it on Xvfb with Mesa's 32-bit lavapipe; the
# layer must load, generate, and double the frame rate.
#   tools/ci/lib32-check.sh [image]   (default: debian:12)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
image="${1:-debian:12}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build\|^dist' | tar -cf - -T - | tar -xf - -C "$work/src")
docker run --rm -v "$work:/work" "$image" bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
dpkg --add-architecture i386
apt-get update -qq
apt-get install -y -qq --no-install-recommends cmake make g++ g++-multilib glslang-tools libvulkan-dev python3 file \
  xvfb mesa-vulkan-drivers:i386 libvulkan1:i386 vulkan-tools:i386 >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
file -L /usr/bin/vkcube | grep -q "32-bit" || { echo "vkcube is not the 32-bit one"; exit 1; }
cmake -S /work/src -B /tmp/b32 -DCMAKE_BUILD_TYPE=Release -DBDEX_32BIT=ON -DBDEX_STATIC_RUNTIME=ON >/tmp/cfg.log 2>&1 ||
  { tail -20 /tmp/cfg.log; exit 1; }
cmake --build /tmp/b32 -j"$(nproc)" >/tmp/build.log 2>&1 || { grep -E -A5 error /tmp/build.log | head -40; exit 1; }
so=$(ls /tmp/b32/layer/libVkLayer_bdex_framegen.so)
file -L "$so" | grep -q "32-bit" || { echo "the layer is not 32-bit"; exit 1; }
# This build only (its manifest, library_arch 32), in a private XDG_DATA_HOME.
mkdir -p /tmp/xdg/vulkan/implicit_layer.d
cp /tmp/b32/layer/*.json /tmp/xdg/vulkan/implicit_layer.d/
icd=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
out="$(DISPLAY=:99 XDG_DATA_HOME=/tmp/xdg VK_ICD_FILENAMES=$icd BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
  BDEX_FG_STATS_INTERVAL=3 timeout 20 vkcube 2>&1 || true)"
grep -m1 "VK_LAYER_BDEX_framegen_32 v" <<<"$out" || { echo "the 32-bit layer did not load"; tail -20 <<<"$out"; exit 1; }
grep -q "generation on" <<<"$out" || { echo "no generation:"; tail -10 <<<"$out"; exit 1; }
ratio=$(grep -E "game [0-9.]+ fps -> output" <<<"$out" | tail -2 | head -1 | sed -n "s/.*(x\([0-9.]*\)).*/\1/p")
echo "32-bit layer: generation on, output/game x$ratio"
awk -v r="${ratio:-0}" "BEGIN { exit !(r >= 1.7) }" || { grep -E "fps -> output" <<<"$out" | tail -3; exit 1; }
echo "lib32 OK"
'
