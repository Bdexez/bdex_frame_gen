#!/usr/bin/env bash
# The Windows build of the layer on this machine's GPU, without Windows:
# DLLs cross-built with MinGW-w64 (x64 and Win32), Khronos' Windows loader
# (vulkan-1.dll of the Vulkan runtime), Wine's winevulkan driving the host's
# Vulkan driver, in headless gamescope (nothing on screen). What a Windows
# player runs, on real hardware:
#   - a Vulkan game, 64 and 32 bits (tests/proton/vulkan_probe.cpp), in
#     interpolation, extrapolation and hybrid x3;
#   - Direct3D 11 and 12 games with DXVK / VKD3D-Proton copied beside them,
#     the injection's default (capture/src/injection.cpp), which then load
#     vulkan-1.dll and through it the layer — 32-bit D3D11 too;
#   - the depth option in DXVK's and VKD3D-Proton's commands, and a frame
#     limit held at DXVK's present wait (fps_limit_wait).
# wine-layer-check.sh covers the same DLL on lavapipe in CI; this one needs a
# GPU, so it is run by hand (after a change to the worker, the pacing, the
# platform shims or the Windows paths).
#   tools/ci/wine-gpu-check.sh [WORKDIR]   (default: $TMPDIR/bdex-wine-gpu-check, downloads kept there)
# Needs: wine (Wine >= 9 in its WoW64 build for the 32-bit runs), gamescope,
# docker, curl, python3.
#
# Under Wine, two things differ from Windows and are set up here:
#   - DXVK and VKD3D-Proton load winevulkan.dll when it exists, which skips
#     Windows' loader (and so the layer): their copies get that name changed,
#     and they load vulkan-1.dll as under Windows;
#   - the loader takes CreateDXGIFactory1 from the system's dxgi.dll: with
#     WINEDLLOVERRIDES dxgi=n (native only) Wine's own is refused and the
#     loader calls a null pointer (loader_windows.c:389, measured 10/11):
#     dxgi=n,b — DXVK's beside the game for it, Wine's for the loader.
# The host's own (Linux) build of the layer, installed in ~/.local, would load
# under winevulkan too (the same name, BDEX_FG=1 in the environment: with a
# frame limit it offered the Windows layer FIFO alone, 10/11): an empty
# XDG_DATA_HOME and XDG_CONFIG_HOME, and XDG_DATA_DIRS without the home.
set -euo pipefail
isolate() {
  export XDG_DATA_HOME="$1/emptyxdg" XDG_CONFIG_HOME="$1/emptyxdg"
  XDG_DATA_DIRS="$(printf '%s' "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" | tr ':' '\n' | grep -v "^${HOME:-/nonexistent}" | paste -sd: -)"
  export XDG_DATA_DIRS="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
self="$ROOT/tools/ci/$(basename "$0")"  # (run again inside gamescope, from another directory)
vkrt_ver=1.4.357.0
vkrt_sha=a14672efed15aafc7f5a16572d35cd3a3416eadf670aeee3cdf50ee32d5fbf83
dxvk_ver=2.5.3
dxvk_sha=d8e6ef7d1168095165e1f8a98c7d5a4485b080467bb573d2a9ef3e3d79ea1eb8
vkd3d_ver=3.0.1
vkd3d_sha=3cf2315522af5e43605ef6d3c41dad91387040bf97199934f3f7ab76caaa2f0c

if [ "${1:-}" = --inside ]; then
  # In headless gamescope: one probe a run, the layer's log in C:\bdex\logs.
  work="$2"
  export WINEPREFIX="$work/prefix" WINEDEBUG=-all
  isolate "$work"
  unset WAYLAND_DISPLAY
  C="$WINEPREFIX/drive_c/bdex"
  mkdir -p "$C/logs"
  run() {  # run NAME DIR OVERRIDES EXE ARGS... (environment: the layer's settings)
    local name="$1" dir="$2" ov="$3"; shift 3
    rm -f "$C/logs/$name.log"
    (cd "$C/$dir" && WINEDLLOVERRIDES="$ov;mscoree,mshtml=" BDEX_FG=1 BDEX_FG_CONFIG=NUL BDEX_FG_LOG=2 \
      BDEX_FG_STATS_INTERVAL=3 BDEX_FG_LOG_FILE="C:\\bdex\\logs\\$name.log" DXVK_LOG_LEVEL=none VKD3D_DEBUG=none \
      timeout 90 wine "$@" > "$C/logs/$name.out" 2>&1) || true
    wineserver -k 2>/dev/null || true  # (else gamescope waits for Wine's services)
  }
  vk="vulkan-1=n"
  (cd "$C/x64" && VK_LOADER_DEBUG=layer WINEDLLOVERRIDES="$vk;mscoree,mshtml=" BDEX_FG_DISABLE=1 timeout 60 wine vkprobe.exe 1 2>&1 |
    tr -d '\r' | grep -E 'Insert instance layer' | sort -u > "$C/logs/host-layers.txt") || true
  wineserver -k 2>/dev/null || true
  BDEX_FG_MODE=interpolate run vk64 x64 "$vk" vkprobe.exe 12
  BDEX_FG_MODE=extrapolate run vk64-extrapolate x64 "$vk" vkprobe.exe 12
  BDEX_FG_MODE=hybrid BDEX_FG_MULTIPLIER=3 run vk64-hybrid x64 "$vk" vkprobe.exe 12
  BDEX_FG_MODE=interpolate run vk32 x86 "$vk" vkprobe.exe 12
  run d3d11 d3d11 "d3d11,dxgi=n,b;$vk" d3d11.exe 12
  run d3d11-32 d3d11-32 "d3d11,dxgi=n,b;$vk" d3d11.exe 12
  run d3d12 d3d12 "d3d12,d3d12core=n;dxgi=n,b;$vk" d3d12.exe 12
  BDEX_FG_DEPTH=1 run d3d11-depth d3d11 "d3d11,dxgi=n,b;$vk" d3d11.exe 8 depth
  BDEX_FG_DEPTH=1 run d3d12-depth d3d12 "d3d12,d3d12core=n;dxgi=n,b;$vk" d3d12.exe 8 depth
  BDEX_FG_FPS_LIMIT=30 run d3d11-limit d3d11 "d3d11,dxgi=n,b;$vk" d3d11.exe 10 uncapped
  : > "$work/inside.done"
  exit 0
fi

work="${1:-${TMPDIR:-/tmp}/bdex-wine-gpu-check}"
mkdir -p "$work/dl" "$work/emptyxdg"
for t in wine gamescope docker curl python3; do command -v "$t" >/dev/null || { echo "missing: $t"; exit 1; }; done
echo "wine $(wine --version), gamescope $(gamescope --version 2>&1 | grep -o '[0-9][0-9.]*' | head -1), work: $work"

# 1. The DLLs and the probes, cross-built (the MinGW image of tools/ci/run.sh mingw).
docker build -q -t bdex-ci-mingw:24.04 "$ROOT/tools/ci/mingw" >/dev/null
rm -rf "$work/src" && mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build\|^dist' | tar -cf - -T - | tar -xf - -C "$work/src")
sed 's/x86_64/i686/g' "$ROOT/tools/ci/mingw/x86_64-w64-mingw32.cmake" > "$work/i686-w64-mingw32.cmake"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$work:/work" bdex-ci-mingw:24.04 bash -euo pipefail -c '
mkdir -p /tmp/vkinc && ln -sf /usr/include/vulkan /usr/include/vk_video /tmp/vkinc/
for a in x86_64 i686; do
  tc=/work/src/tools/ci/mingw/x86_64-w64-mingw32.cmake; [ $a = i686 ] && tc=/work/i686-w64-mingw32.cmake
  cmake -S /work/src -B /work/b-$a -DCMAKE_TOOLCHAIN_FILE=$tc -DCMAKE_BUILD_TYPE=Release -DBDEX_BUILD_TESTS=OFF \
    -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_OFFLINE=OFF -DBDEX_VULKAN_INCLUDE_DIR=/tmp/vkinc >/work/cfg-$a.log 2>&1 ||
    { tail -20 /work/cfg-$a.log; exit 1; }
  cmake --build /work/b-$a -j"$(nproc)" --target bdex_framegen >/work/build-$a.log 2>&1 ||
    { grep -E -A5 " error|undefined reference" /work/build-$a.log | head -40; exit 1; }
  $a-w64-mingw32-g++-posix -O2 -static -I/tmp/vkinc /work/src/tests/proton/vulkan_probe.cpp -o /work/vkprobe-$a.exe
  $a-w64-mingw32-g++-posix -O2 -static /work/src/tests/proton/d3d11_probe.cpp -ld3d11 -ldxgi -o /work/d3d11-$a.exe
done
x86_64-w64-mingw32-g++-posix -O2 -static /work/src/tests/proton/d3d12_probe.cpp -ld3d12 -ldxgi -o /work/d3d12-x86_64.exe
'

# 2. Windows' loader, DXVK, VKD3D-Proton, checked against their hashes.
cd "$work/dl"
fetch() {  # fetch FILE URL SHA256
  [ -f "$1" ] && echo "$3  $1" | sha256sum -c --quiet 2>/dev/null && return 0
  curl -sSfL -o "$1" "$2"
  echo "$3  $1" | sha256sum -c --quiet
}
fetch vkrt.zip "https://sdk.lunarg.com/sdk/download/$vkrt_ver/windows/VulkanRT-X64-$vkrt_ver-Components.zip" $vkrt_sha
fetch dxvk.tar.gz "https://github.com/doitsujin/dxvk/releases/download/v$dxvk_ver/dxvk-$dxvk_ver.tar.gz" $dxvk_sha
fetch vkd3d.tar.zst "https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v$vkd3d_ver/vkd3d-proton-$vkd3d_ver.tar.zst" $vkd3d_sha
rm -rf VulkanRT-X64-$vkrt_ver-Components dxvk-$dxvk_ver vkd3d-proton-$vkd3d_ver
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' vkrt.zip .
tar xzf dxvk.tar.gz
tar --zstd -xf vkd3d.tar.zst
rt="$work/dl/VulkanRT-X64-$vkrt_ver-Components"

# 3. A fresh prefix, the layer registered as register-layer.ps1 does (the
# 32-bit one under WOW6432Node), each game in a folder of its own.
export WINEPREFIX="$work/prefix" WINEDEBUG=-all
isolate "$work"
rm -rf "$WINEPREFIX"
(unset DISPLAY WAYLAND_DISPLAY; WINEDLLOVERRIDES="mscoree,mshtml=" timeout 180 wineboot -i >/dev/null 2>&1) || true
C="$WINEPREFIX/drive_c/bdex"
mkdir -p "$C"/{layer64,layer32,x64,x86,d3d11,d3d11-32,d3d12}
cp "$work/b-x86_64/layer/VkLayer_bdex_framegen.dll" "$C/layer64/"
cp "$work/b-i686/layer/VkLayer_bdex_framegen.dll" "$C/layer32/"
manifest() { sed 's#"library_path": ".*"#"library_path": ".\\\\VkLayer_bdex_framegen.dll"#' "$1" > "$2"; }
manifest "$work/b-x86_64/layer/VK_LAYER_BDEX_framegen.json" "$C/layer64/VK_LAYER_BDEX_framegen.json"
manifest "$work/b-i686/layer/VK_LAYER_BDEX_framegen_32.json" "$C/layer32/VK_LAYER_BDEX_framegen.json"
(unset DISPLAY WAYLAND_DISPLAY
 wine reg add 'HKLM\SOFTWARE\Khronos\Vulkan\ImplicitLayers' /v 'C:\bdex\layer64\VK_LAYER_BDEX_framegen.json' /t REG_DWORD /d 0 /f
 wine reg add 'HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\ImplicitLayers' /v 'C:\bdex\layer32\VK_LAYER_BDEX_framegen.json' /t REG_DWORD /d 0 /f
 wineserver -k) >/dev/null 2>&1 || true
cp "$rt/x64/vulkan-1.dll" "$C/x64/" && cp "$work/vkprobe-x86_64.exe" "$C/x64/vkprobe.exe"
cp "$rt/x86/vulkan-1.dll" "$C/x86/" && cp "$work/vkprobe-i686.exe" "$C/x86/vkprobe.exe"
cp "$rt/x64/vulkan-1.dll" "$C/d3d11/" && cp "$work/d3d11-x86_64.exe" "$C/d3d11/d3d11.exe"
cp "$rt/x86/vulkan-1.dll" "$C/d3d11-32/" && cp "$work/d3d11-i686.exe" "$C/d3d11-32/d3d11.exe"
cp "$rt/x64/vulkan-1.dll" "$C/d3d12/" && cp "$work/d3d12-x86_64.exe" "$C/d3d12/d3d12.exe"
# (the copies the injection makes, winevulkan.dll's name changed: see the top)
python3 -I - "$work/dl" "$C" <<'EOF'
import sys, pathlib
dl, c = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
dxvk = next(dl.glob("dxvk-*")); vkd3d = next(dl.glob("vkd3d-proton-*"))
copies = [(dxvk / "x64", "d3d11"), (dxvk / "x32", "d3d11-32")]
for src, dst in copies:
    for name in ("d3d11.dll", "dxgi.dll"):
        (c / dst / name).write_bytes((src / name).read_bytes().replace(b"winevulkan.dll\0", b"winevulkXX.dll\0"))
(c / "d3d12" / "dxgi.dll").write_bytes((c / "d3d11" / "dxgi.dll").read_bytes())
for name in ("d3d12.dll", "d3d12core.dll"):
    (c / "d3d12" / name).write_bytes((vkd3d / "x64" / name).read_bytes().replace(b"winevulkan.dll\0", b"winevulkXX.dll\0"))
EOF

# 4. The runs, in headless gamescope (its Xwayland; nothing on screen).
rm -f "$work/inside.done"
timeout -k 10 600 gamescope --backend headless -W 1280 -H 720 -r 144 -- "$self" --inside "$work" >"$work/gamescope.log" 2>&1 || true
[ -f "$work/inside.done" ] || { echo "the runs did not finish (gamescope: $work/gamescope.log)"; tail -20 "$work/gamescope.log"; exit 1; }

# 5. Verdicts.
status=0
if grep -q BDEX "$C/logs/host-layers.txt" 2>/dev/null; then
  echo "FAIL the host's own layer loads under winevulkan (the runs measure both):"; cat "$C/logs/host-layers.txt"; status=1
fi
log() { tr -d '\r' < "$C/logs/$1.log" 2>/dev/null || true; }
out() { tr -d '\r' < "$C/logs/$1.out" 2>/dev/null || true; }
statline() {  # statline NAME FIELD: from the last statistics line but one (settled)
  log "$1" | grep -E "game [0-9.]+ fps -> output" | tail -2 | head -1 | sed -n "$2"
}
gen() {  # gen NAME MIN_RATIO
  local name="$1" min="$2" ratio fps late
  if ! log "$name" | grep -q "generation on"; then echo "FAIL $name: no generation"; out "$name" | tail -5; log "$name" | tail -5; status=1; return; fi
  ratio=$(statline "$name" 's/.*(x\([0-9.]*\)).*/\1/p')
  fps=$(statline "$name" 's/.*game \([0-9.]*\) fps.*/\1/p')
  late=$(statline "$name" 's/.*wake-up late \([0-9]*\) us.*/\1/p')
  if awk -v r="${ratio:-0}" -v m="$min" -v f="${fps:-0}" -v l="${late:-0}" 'BEGIN { exit !(r >= m && f > 27 && f < 33 && l < 1000) }'; then
    echo "ok   $name: game ${fps} fps, x${ratio}, wake-up late ${late:-?} us"
  else
    echo "FAIL $name: game ${fps:-?} fps, x${ratio:-?} (at least x$min), wake-up late ${late:-?} us (under 1000)"; status=1
  fi
}
gen vk64 1.9
gen vk64-extrapolate 1.9
gen vk64-hybrid 2.8
gen vk32 1.9
gen d3d11 1.9
gen d3d11-32 1.9
gen d3d12 1.9
for d in d3d11-depth d3d12-depth; do
  if log "$d" | grep -q "reversed Z"; then echo "ok   $d: $(log "$d" | grep -m1 -oE 'depth: [0-9].*')"
  else echo "FAIL $d: the depth buffer was not found"; log "$d" | grep -i depth | head -5; status=1; fi
done
fps=$(out d3d11-limit | sed -n 's/.*probe: \([0-9.]*\) fps.*/\1/p' | tail -1)
if awk -v f="${fps:-0}" 'BEGIN { exit !(f > 28.5 && f < 31.5) }' && log d3d11-limit | grep -q "held where it waits"; then
  echo "ok   d3d11-limit: held at 30 fps (${fps}) at DXVK's present wait"
else
  echo "FAIL d3d11-limit: ${fps:-?} fps, $(log d3d11-limit | grep -m1 -oE 'fps_limit_wait[^,]*' || echo 'no fps_limit_wait line')"; status=1
fi
[ "$status" = 0 ] && echo "windows layer on the GPU OK (logs: $C/logs)" || echo "logs: $C/logs"
exit $status
