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
echo
echo "Enable it per game with BDEX_FG=1 (Steam launch options: BDEX_FG=1 %command%)"
echo "or run:  bdex-framegen -- <game>"
