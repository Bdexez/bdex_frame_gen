#!/usr/bin/env bash
# The Proton path on Wine's Wayland driver (PROTON_ENABLE_WAYLAND / winewayland:
# no X server at all): the Direct3D 11 program of proton-check.sh under Wine
# from WineHQ (the distribution's Wine 9 has no Vulkan in its Wayland driver)
# with DXVK, in a headless weston, lavapipe; the layer's native Wayland path
# must load, generate and double the frame rate.
#   tools/ci/proton-wayland-check.sh [image]   (default: ubuntu:24.04)
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
dpkg --add-architecture i386
apt-get update -qq
apt-get install -y -qq --no-install-recommends cmake make g++ glslc libvulkan-dev python3 curl ca-certificates gnupg \
  mesa-vulkan-drivers libvulkan1 g++-mingw-w64-x86-64 weston >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
. /etc/os-release
mkdir -pm755 /etc/apt/keyrings
curl -sSL -o /etc/apt/keyrings/winehq-archive.key https://dl.winehq.org/wine-builds/winehq.key
curl -sSL -o /etc/apt/sources.list.d/winehq.sources \
  "https://dl.winehq.org/wine-builds/ubuntu/dists/$VERSION_CODENAME/winehq-$VERSION_CODENAME.sources"
apt-get update -qq >/dev/null
apt-get install -y -qq --install-recommends winehq-stable >/tmp/wine.log 2>&1 || { tail -20 /tmp/wine.log; exit 1; }
W=/opt/wine-stable/bin/wine
cmake -S /work/src -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_OFFLINE=OFF \
  -DBDEX_BUILD_CAPTURE=OFF >/tmp/cfg.log 2>&1 || { tail -20 /tmp/cfg.log; exit 1; }
cmake --build /tmp/b -j"$(nproc)" --target bdex_framegen >/tmp/build.log 2>&1 || { grep -E -A5 error /tmp/build.log | head -40; exit 1; }
mkdir -p /tmp/xdg/vulkan/implicit_layer.d
cp /tmp/b/layer/*.json /tmp/xdg/vulkan/implicit_layer.d/
x86_64-w64-mingw32-g++ -O2 -static /work/src/tests/proton/d3d11_probe.cpp -ld3d11 -ldxgi -o /tmp/d3d11.exe
curl -sSL -o /tmp/dxvk.tar.gz "https://github.com/doitsujin/dxvk/releases/download/v$dxvk_ver/dxvk-$dxvk_ver.tar.gz"
echo "$dxvk_sha  /tmp/dxvk.tar.gz" | sha256sum -c --quiet
tar xzf /tmp/dxvk.tar.gz -C /tmp
# A Wayland compositor and nothing else: no DISPLAY, Wine on its Wayland driver.
unset DISPLAY
export XDG_RUNTIME_DIR=/tmp/xdgrt WAYLAND_DISPLAY=wl-0 WINEPREFIX=/tmp/prefix WINEDEBUG=-all
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
weston --backend=headless --width=1280 --height=800 --renderer=pixman --socket=wl-0 >/tmp/weston.log 2>&1 &
sleep 3
WINEDLLOVERRIDES="mscoree,mshtml=" timeout 120 "$W" wineboot -i >/dev/null 2>&1 || true
"$W" reg add "HKCU\\Software\\Wine\\Drivers" /v Graphics /d wayland /f >/dev/null 2>&1
cp /tmp/dxvk-$dxvk_ver/x64/d3d11.dll /tmp/dxvk-$dxvk_ver/x64/dxgi.dll "$WINEPREFIX/drive_c/windows/system32/"
icd=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)
out="$(XDG_DATA_HOME=/tmp/xdg VK_ICD_FILENAMES=$icd BDEX_FG=1 BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=2 \
  BDEX_FG_STATS_INTERVAL=3 DXVK_LOG_LEVEL=warn WINEDLLOVERRIDES="d3d11,dxgi=n;mscoree,mshtml=" \
  timeout 90 "$W" /tmp/d3d11.exe 12 2>&1 || true)"
grep -q "frames presented" <<<"$out" || { echo "the program did not finish:"; tail -30 <<<"$out"; exit 1; }
grep -q "generation on" <<<"$out" || { echo "no generation:"; tail -20 <<<"$out"; exit 1; }
ratio=$(grep -E "game [0-9.]+ fps -> output" <<<"$out" | tail -2 | head -1 | sed -n "s/.*(x\([0-9.]*\)).*/\1/p")
grep -E "fps -> output" <<<"$out" | tail -2
echo "$("$W" --version), Wayland driver, DXVK $dxvk_ver: generation on, output/game x$ratio"
awk -v r="${ratio:-0}" "BEGIN { exit !(r >= 1.7) }" || exit 1
echo "proton wayland OK"
'
