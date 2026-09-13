#!/usr/bin/env bash
# Builds and installs the layer for the current user (~/.local by default).
#   tools/install.sh [prefix]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$HOME/.local}"
BUILD="$ROOT/build-install"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBDEX_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(nproc)"
cmake --install "$BUILD"
echo
echo "Installed:"
echo "  $PREFIX/lib/libVkLayer_bdex_framegen.so"
echo "  $PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json"
echo "  $PREFIX/bin/bdex-framegen"
echo "  $PREFIX/bin/bdex-framegen-gui  (graphical launcher)"

# 32-bit layer for 32-bit games, when the toolchain allows it.
if echo 'int main(){return 0;}' | g++ -m32 -x c++ - -o /dev/null 2>/dev/null && [ -e /usr/lib32/libvulkan.so.1 ]; then
  BUILD32="$ROOT/build-install32"
  cmake -S "$ROOT" -B "$BUILD32" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBDEX_32BIT=ON >/dev/null
  cmake --build "$BUILD32" -j"$(nproc)"
  cmake --install "$BUILD32"
  echo "  $PREFIX/lib32/libVkLayer_bdex_framegen.so (32-bit)"
  echo "  $PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json"
else
  echo "  (no 32-bit toolchain: 32-bit games will run without the layer)"
fi
echo
echo "Enable it per game with BDEX_FG=1 (Steam launch options: BDEX_FG=1 %command%)"
echo "or run:  bdex-framegen -- <game>"
