#!/usr/bin/env bash
# The Proton path end to end, without Steam: a Direct3D 11 program
# (tests/proton/d3d11_probe.cpp, cross-compiled with MinGW) runs under Wine
# with DXVK, whose Vulkan swapchain reaches the layer through winevulkan as a
# Proton game's does; lavapipe in an Ubuntu container. The layer must load
# into the Wine process, generate, and double the frame rate.
# Display: Xorg's dummy driver with a 60 Hz modeline, not Xvfb — Xvfb's mode
# has no refresh rate, Wine reports 0 Hz and DXVK (2.5 to 2.7 at least)
# divides by it in its first Present.
#   tools/ci/proton-check.sh [image]   (default: ubuntu:24.04)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
image="${1:-ubuntu:24.04}"
dxvk_ver=2.5.3
dxvk_sha=d8e6ef7d1168095165e1f8a98c7d5a4485b080467bb573d2a9ef3e3d79ea1eb8
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build\|^dist' | tar -cf - -T - | tar -xf - -C "$work/src")
docker run --rm -e dxvk_ver=$dxvk_ver -e dxvk_sha=$dxvk_sha -v "$work:/work" "$image" bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends cmake make g++ glslc libvulkan-dev python3 curl ca-certificates \
  xserver-xorg-core xserver-xorg-video-dummy mesa-vulkan-drivers libvulkan1 wine64 g++-mingw-w64-x86-64 >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
cmake -S /work/src -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_OFFLINE=OFF >/tmp/cfg.log 2>&1 ||
  { tail -20 /tmp/cfg.log; exit 1; }
cmake --build /tmp/b -j"$(nproc)" --target bdex_framegen >/tmp/build.log 2>&1 ||
  { grep -E -A5 error /tmp/build.log | head -40; exit 1; }
mkdir -p /tmp/xdg/vulkan/implicit_layer.d
cp /tmp/b/layer/*.json /tmp/xdg/vulkan/implicit_layer.d/
# The "game".
x86_64-w64-mingw32-g++ -O2 -static /work/src/tests/proton/d3d11_probe.cpp -ld3d11 -ldxgi -o /tmp/probe.exe
# DXVK, checked against its hash, in a fresh prefix.
curl -sSL -o /tmp/dxvk.tar.gz "https://github.com/doitsujin/dxvk/releases/download/v$dxvk_ver/dxvk-$dxvk_ver.tar.gz"
echo "$dxvk_sha  /tmp/dxvk.tar.gz" | sha256sum -c --quiet
tar xzf /tmp/dxvk.tar.gz -C /tmp
export WINEPREFIX=/tmp/prefix WINEDEBUG=-all WINEDLLOVERRIDES="d3d11,dxgi=n;mscoree,mshtml=" DISPLAY=:99
Xorg :99 -config /work/src/tests/proton/xorg-dummy.conf -noreset -logfile /tmp/xorg.log >/dev/null 2>&1 &
sleep 3
wine=$(command -v wine64 || command -v wine || ls /usr/lib/wine/wine64)
timeout 120 "$wine" wineboot -i >/dev/null 2>&1 || true
cp /tmp/dxvk-$dxvk_ver/x64/d3d11.dll /tmp/dxvk-$dxvk_ver/x64/dxgi.dll "$WINEPREFIX/drive_c/windows/system32/"
icd=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)
out="$(XDG_DATA_HOME=/tmp/xdg VK_ICD_FILENAMES=$icd BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
  BDEX_FG_STATS_INTERVAL=3 DXVK_LOG_LEVEL=warn timeout 90 "$wine" /tmp/probe.exe 12 2>&1 || true)"
grep -q "frames presented" <<<"$out" || { echo "the D3D11 program did not finish:"; tail -30 <<<"$out"; exit 1; }
grep -m1 "VK_LAYER_BDEX_framegen" <<<"$out" || { echo "the layer did not load into Wine"; tail -20 <<<"$out"; exit 1; }
grep -q "generation on" <<<"$out" || { echo "no generation:"; tail -20 <<<"$out"; exit 1; }
ratio=$(grep -E "game [0-9.]+ fps -> output" <<<"$out" | tail -2 | head -1 | sed -n "s/.*(x\([0-9.]*\)).*/\1/p")
grep -E "fps -> output" <<<"$out" | tail -2
echo "Wine + DXVK $dxvk_ver: generation on, output/game x$ratio"
awk -v r="${ratio:-0}" "BEGIN { exit !(r >= 1.7) }" || exit 1
echo "proton OK"
'
