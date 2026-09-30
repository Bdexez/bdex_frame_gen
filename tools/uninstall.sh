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
rm -rfv "$PREFIX/share/bdex-framegen"
# The copies made for Flatpak launchers (packaging/linux/flatpak.sh).
here="$(cd "$(dirname "$0")" && pwd)"
for f in "$here/flatpak.sh" "$here/../packaging/linux/flatpak.sh"; do
  if [ -f "$f" ]; then bash "$f" --remove; break; fi
done
