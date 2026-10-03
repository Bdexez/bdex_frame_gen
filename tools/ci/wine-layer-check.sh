#!/usr/bin/env bash
# The Windows layer (mode 1, VkLayer_bdex_framegen.dll) at run time, without
# Windows: cross-built with MinGW-w64, registered as an implicit layer the
# way register-layer.ps1 does (HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers,
# a manifest beside the DLL), loaded by Khronos' Windows loader (vulkan-1.dll
# of the Vulkan runtime, LunarG) into a Win32 Vulkan program
# (tests/proton/vulkan_probe.cpp), under Wine with winevulkan as the
# loader's driver, on lavapipe. The layer must load, generate and double the
# frame rate: its Windows code paths (platform shims, %APPDATA% config,
# Win32 surfaces, timers, threads) run, not only compile.
#   tools/ci/wine-layer-check.sh [image]   (default: ubuntu:24.04)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
image="${1:-ubuntu:24.04}"
vkrt_ver=1.4.357.0
vkrt_sha=a14672efed15aafc7f5a16572d35cd3a3416eadf670aeee3cdf50ee32d5fbf83
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build\|^dist' | tar -cf - -T - | tar -xf - -C "$work/src")
docker run --rm -e vkrt_ver=$vkrt_ver -e vkrt_sha=$vkrt_sha -v "$work:/work" "$image" bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends cmake make g++ glslc libvulkan-dev g++-mingw-w64-x86-64-posix wine64 \
  xserver-xorg-core xserver-xorg-video-dummy mesa-vulkan-drivers libvulkan1 unzip curl ca-certificates \
  >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
mkdir -p /tmp/vkinc && ln -sf /usr/include/vulkan /usr/include/vk_video /tmp/vkinc/
cmake -S /work/src -B /tmp/bw -DCMAKE_TOOLCHAIN_FILE=/work/src/tools/ci/mingw/x86_64-w64-mingw32.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_OFFLINE=OFF \
  -DBDEX_VULKAN_INCLUDE_DIR=/tmp/vkinc >/tmp/cfg.log 2>&1 || { tail -20 /tmp/cfg.log; exit 1; }
cmake --build /tmp/bw -j"$(nproc)" --target bdex_framegen >/tmp/build.log 2>&1 ||
  { grep -E -A5 " error|undefined reference" /tmp/build.log | head -40; exit 1; }
x86_64-w64-mingw32-g++-posix -O2 -static -I/tmp/vkinc /work/src/tests/proton/vulkan_probe.cpp -o /tmp/vkprobe.exe
curl -sSL -o /tmp/vkrt.zip "https://sdk.lunarg.com/sdk/download/$vkrt_ver/windows/VulkanRT-X64-$vkrt_ver-Components.zip"
echo "$vkrt_sha  /tmp/vkrt.zip" | sha256sum -c --quiet
(cd /tmp && unzip -q vkrt.zip)
export WINEPREFIX=/tmp/prefix WINEDEBUG=-all DISPLAY=:99
W=/usr/lib/wine/wine64
# (Xorg dummy at 60 Hz rather than Xvfb: see proton-check.sh.)
Xorg :99 -config /work/src/tests/proton/xorg-dummy.conf -noreset -logfile /tmp/xorg.log >/dev/null 2>&1 &
sleep 3
WINEDLLOVERRIDES="mscoree,mshtml=" timeout 120 "$W" wineboot -i >/dev/null 2>&1 || true
dir="$WINEPREFIX/drive_c/bdex"
mkdir -p "$dir/layer"
cp /tmp/VulkanRT-X64-$vkrt_ver-Components/x64/vulkan-1.dll /tmp/VulkanRT-X64-$vkrt_ver-Components/x64/vulkaninfo.exe \
  /tmp/vkprobe.exe "$dir/"
cp /tmp/bw/layer/VkLayer_bdex_framegen.dll "$dir/layer/"
# The manifest as the Windows package ships it: the DLL beside it.
sed "s#\"library_path\": \".*\"#\"library_path\": \".\\\\\\\\VkLayer_bdex_framegen.dll\"#" \
  /tmp/bw/layer/VK_LAYER_BDEX_framegen.json > "$dir/layer/VK_LAYER_BDEX_framegen.json"
"$W" reg add "HKLM\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers" /v "C:\\bdex\\layer\\VK_LAYER_BDEX_framegen.json" \
  /t REG_DWORD /d 0 /f >/dev/null 2>&1
export VK_ICD_FILENAMES=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1) WINEDLLOVERRIDES="vulkan-1=n;mscoree,mshtml="
cd "$dir"
layers="$(timeout 60 "$W" vulkaninfo.exe --summary 2>&1 | tr -d "\r" || true)"
grep -q "VK_LAYER_BDEX_framegen" <<<"$layers" || { echo "the Windows loader does not list the layer:"; head -30 <<<"$layers"; exit 1; }
echo "$(grep -m1 "Vulkan Instance Version" <<<"$layers"): VK_LAYER_BDEX_framegen listed"
out="$(BDEX_FG=1 BDEX_FG_CONFIG=NUL BDEX_FG_LOG=2 BDEX_FG_STATS_INTERVAL=3 BDEX_FG_LOG_FILE="C:\\bdex\\fg.log" \
  timeout 90 "$W" vkprobe.exe 12 2>&1 | tr -d "\r" || true)"
log="$(tr -d "\r" < fg.log 2>/dev/null || true)"
grep -q "frames presented" <<<"$out" || { echo "the probe did not finish:"; tail -20 <<<"$out"; tail -20 <<<"$log"; exit 1; }
grep -q "generation on" <<<"$log" || { echo "no generation:"; tail -20 <<<"$log"; exit 1; }
ratio=$(grep -E "game [0-9.]+ fps -> output" <<<"$log" | tail -2 | head -1 | sed -n "s/.*(x\([0-9.]*\)).*/\1/p")
grep -E "fps -> output|thread priority" <<<"$log" | tail -3
echo "Windows layer under Wine ($("$W" --version)): generation on, output/game x$ratio"
awk -v r="${ratio:-0}" "BEGIN { exit !(r >= 1.7) }" || exit 1
echo "windows layer OK"
'
