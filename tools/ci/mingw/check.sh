#!/usr/bin/env bash
# Compiles the Windows-built C++ (the layer, the unit tests, and on x64 the
# capture app except its C++/WinRT file) with MinGW for x64 and for 32-bit
# x86, syntax only. The 32-bit pass catches what only MSVC Win32
# used to report after a push: non-dispatchable Vulkan handles are uint64_t
# there, not pointers (no reinterpret_cast to or from them), and pointer-
# sized integers are 32 bits. Run through tools/ci/run.sh mingw (Docker), or
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
  [[ $cxx == x86_64* ]] && files="$files capture/src/vkctx.cpp capture/src/launcher.cpp capture/src/main.cpp capture/src/tray.cpp capture/src/injection.cpp capture/src/report.cpp"
  n=0
  for f in $files; do
    out=$($cxx -std=c++20 -fsyntax-only -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
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
exit $status
