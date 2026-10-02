#!/usr/bin/env bash
# Installs the prebuilt bdex-framegen of this archive for the current user
# (~/.local by default) — nothing to compile.
#   ./install.sh [prefix]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${1:-$HOME/.local}"
arch="$(cat "$HERE/files/share/bdex-framegen/ARCH" 2>/dev/null || echo x86_64)"
if [ "$arch" != "$(uname -m)" ]; then
  echo "This archive is for $arch, this machine is $(uname -m): download the $(uname -m) archive." >&2
  exit 1
fi
mkdir -p "$PREFIX"
# New inodes (remove, then copy): a running game keeps the library it mapped
# — copying over it in place would truncate that mapping (SIGBUS).
(cd "$HERE/files" && find . -type f) | while IFS= read -r f; do rm -f "$PREFIX/$f"; done
cp -rv "$HERE/files/." "$PREFIX/" | sed 's/^/  /'
# The manifests point the Vulkan loader at the library: absolute paths into
# this prefix.
for m in "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json:lib" \
         "$PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json:lib32"; do
  f="${m%%:*}" dir="${m##*:}"
  [ -f "$f" ] || continue
  sed -i "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$PREFIX/$dir/libVkLayer_bdex_framegen.so\"#" "$f"
done
# Steam, Heroic, Lutris or Bottles installed as a Flatpak: a copy in their
# sandbox (flatpak.sh).
bash "$HERE/flatpak.sh" "$PREFIX"
echo
echo "Installed into $PREFIX. The layer now loads into every Vulkan game"
echo "(native and Proton/DXVK/VKD3D), inert until you turn it on:"
echo "    bdex-framegen-gui                  control panel (needs Python 3 with PyGObject and GTK 4)"
echo "    bdex-framegen -- <game>            one run with frame generation"
echo "    Steam launch options: BDEX_FG=1 %command%"
case ":$PATH:" in *":$PREFIX/bin:"*) ;; *) echo "(add $PREFIX/bin to your PATH)";; esac
