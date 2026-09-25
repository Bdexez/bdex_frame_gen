#!/usr/bin/env bash
# Installs the prebuilt bdex-framegen of this archive for the current user
# (~/.local by default) — nothing to compile.
#   ./install.sh [prefix]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-$HOME/.local}"
mkdir -p "$PREFIX"
cp -rv "$HERE/files/." "$PREFIX/" | sed 's/^/  /'
# The manifests point the Vulkan loader at the library: absolute paths into
# this prefix.
for m in "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json:lib" \
         "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json:lib32"; do
  f="${m%%:*}" dir="${m##*:}"
  [ -f "$f" ] || continue
  sed -i "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$PREFIX/$dir/libVkLayer_bdex_framegen.so\"#" "$f"
done
echo
echo "Installed into $PREFIX. The layer now loads into every Vulkan game"
echo "(native and Proton/DXVK/VKD3D), inert until you turn it on:"
echo "    bdex-framegen-gui                  control panel (needs python3-gobject, GTK 4, libadwaita)"
echo "    bdex-framegen -- <game>            one run with frame generation"
echo "    Steam launch options: BDEX_FG=1 %command%"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "(add $PREFIX/bin to your PATH)";; esac
