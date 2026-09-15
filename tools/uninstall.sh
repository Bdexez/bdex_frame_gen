#!/usr/bin/env bash
# Removes the files installed by install.sh.   tools/uninstall.sh [prefix]
set -euo pipefail
PREFIX="${1:-$HOME/.local}"
rm -fv "$PREFIX/lib/libVkLayer_bdex_framegen.so" \
       "$PREFIX/lib32/libVkLayer_bdex_framegen.so" \
       "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json" \
       "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json" \
       "$PREFIX/bin/bdex-framegen" "$PREFIX/bin/bdex-framegen-gui" "$PREFIX/bin/bdex_demo" \
       "$PREFIX/share/applications/bdex-framegen.desktop"
