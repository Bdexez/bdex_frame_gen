#!/usr/bin/env bash
# Compiles the Windows-built C++ (the layer, the unit tests, and on x64 the
# capture app except its C++/WinRT file) with MinGW for x64 and for 32-bit
# x86, syntax only. The 32-bit pass catches what only MSVC Win32
# used to report after a push: non-dispatchable Vulkan handles are uint64_t
# there, not pointers (no reinterpret_cast to or from them), and pointer-
# sized integers are 32 bits. -Werror=shadow=local stands for MSVC's /W4
# /WX C4456/C4457 (a local hiding another local or a parameter: a
# lock_guard named like the function's unique_lock broke both Windows builds
# on 2026-11-01, after a month without the Windows CI). Run through tools/ci/run.sh mingw (Docker), or
# directly where both MinGW toolchains are installed (the CI's windows-syntax job).
set -uo pipefail
cd "$(dirname "$0")/../../.."
# shaders.h (generated) is included by the engine files.
cmake -S . -B build-gen -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build-gen --target bdex_layer_shaders >/dev/null || exit 1
# Only the Vulkan headers of the system (the rest would be glibc's).
mkdir -p /tmp/vkinc && ln -sf /usr/include/vulkan /usr/include/vk_video /tmp/vkinc/
# What the Windows CI builds (CMakeLists.txt): the layer and the unit tests
# for both architectures (tests that open a GLFW window are Linux-only), the
# capture app for x64 only; bdex_offline is not built on Windows.
common=$(ls layer/src/*.cpp layer/src/passes/*.cpp tests/test_*.cpp | xargs grep -L "GLFW/glfw3.h")
status=0
for cxx in x86_64-w64-mingw32-g++-posix i686-w64-mingw32-g++-posix; do
  files=$common
  [[ $cxx == x86_64* ]] && files="$files capture/src/vkctx.cpp capture/src/launcher.cpp capture/src/main.cpp capture/src/tray.cpp capture/src/injection.cpp capture/src/report.cpp tools/calibrate/bdex_calibrate.cpp"
  n=0
  for f in $files; do
    out=$($cxx -std=c++20 -fsyntax-only -Werror=shadow=local -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
          -DVK_USE_PLATFORM_WIN32_KHR -DBDEX_LAYER_NAME='"VK_LAYER_BDEX_framegen"' -DBDEX_LAYER_VERSION_STR='"0"' -DBDEX_VERSION_STR='"0"' \
          -Ilayer/src -Icapture/src -Itests -Ibuild-gen/layer/generated -isystem /tmp/vkinc "$f" 2>&1)
    if [ $? -ne 0 ]; then
      echo "FAIL [$cxx] $f"; grep -E "error" <<<"$out" | head -5; status=1
    fi
    n=$((n + 1))
  done
  echo "$cxx: $n files checked"
done
# The capture target compiles the engine too, with capture/src among its
# include directories (capture/CMakeLists.txt order): a header name both
# define (log.h) must still resolve to the layer's in the engine's files.
n=0
for f in layer/src/*.cpp layer/src/passes/*.cpp; do
  out=$(x86_64-w64-mingw32-g++-posix -std=c++20 -fsyntax-only -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
        -DVK_USE_PLATFORM_WIN32_KHR -DBDEX_LAYER_NAME='"VK_LAYER_BDEX_framegen"' -DBDEX_LAYER_VERSION_STR='"0"' -DBDEX_VERSION_STR='"0"' \
        -Ilayer/src -Ibuild-gen/layer/generated -Icapture/src -isystem /tmp/vkinc "$f" 2>&1)
  if [ $? -ne 0 ]; then echo "FAIL [capture include order] $f"; grep -E "error" <<<"$out" | head -5; status=1; fi
  n=$((n + 1))
done
echo "capture include order: $n files checked"
# Linkage, which a syntax check cannot see (vécu: a function the launcher's
# header declares, defined inside an anonymous namespace — LNK2019 under
# MSVC only): every bdex:: function main.cpp calls that the capture app's
# own headers declare must be defined, with external linkage, in one of its
# objects. (capture.cpp — C++/WinRT — is not compiled here: its class's
# members are left out.)
obj=$(mktemp -d)
for f in capture/src/main.cpp capture/src/launcher.cpp capture/src/injection.cpp capture/src/report.cpp capture/src/tray.cpp capture/src/vkctx.cpp; do
  x86_64-w64-mingw32-g++-posix -std=c++20 -c -o "$obj/$(basename "$f" .cpp).o" -DWIN32_LEAN_AND_MEAN -DNOMINMAX     -DVK_USE_PLATFORM_WIN32_KHR -DBDEX_LAYER_NAME='"VK_LAYER_BDEX_framegen"' -DBDEX_LAYER_VERSION_STR='"0"' -DBDEX_VERSION_STR='"0"'     -Ilayer/src -Ibuild-gen/layer/generated -Icapture/src -isystem /tmp/vkinc "$f" 2>/dev/null || { echo "FAIL [objects] $f"; status=1; }
done
declared=$(grep -hoE "\b[a-zA-Z_][a-zA-Z0-9_]*\(" capture/src/launcher.h capture/src/injection.h capture/src/report.h capture/src/tray.h capture/src/vkctx.h | tr -d '(' | sort -u)
defined=$(for o in "$obj"/*.o; do x86_64-w64-mingw32-nm -C --defined-only "$o"; done | awk '$2 == "T" { $1 = ""; $2 = ""; print }')
missing=0
for sym in $(x86_64-w64-mingw32-nm -C --undefined-only "$obj/main.o" | grep -oE "bdex::[A-Za-z_][A-Za-z0-9_:]*" | sort -u); do
  name=${sym##*::}
  grep -qx "$name" <<<"$declared" || continue
  case $sym in bdex::Capture::*) continue ;; esac
  grep -qE "${sym}(\[abi:[a-z0-9]+\])?\(" <<<"$defined" || { echo "FAIL [link] $sym: called by main.cpp, defined nowhere with external linkage"; missing=1; }
done
rm -rf "$obj"
[ $missing -eq 0 ] && echo "capture linkage: main.cpp's calls into the capture app's headers resolved" || status=1
exit $status
