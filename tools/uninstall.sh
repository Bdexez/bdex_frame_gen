#!/usr/bin/env bash
# Removes the files installed by install.sh.
#   tools/uninstall.sh [--purge] [prefix]
# --purge also removes the settings (~/.config/bdex-framegen.conf, the
# copies in Flatpak / Snap launchers) and the cache (~/.cache/bdex-framegen:
# session, game status, online_tune results); without it they stay, for a
# later install.
set -euo pipefail
purge=0
if [ "${1:-}" = --purge ]; then purge=1; shift; fi
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
if [ "$purge" = 1 ]; then
  rm -fv "${XDG_CONFIG_HOME:-$HOME/.config}/bdex-framegen.conf"
  rm -rfv "${XDG_CACHE_HOME:-$HOME/.cache}/bdex-framegen"
  for d in "$HOME"/.var/app/*/config "$HOME"/snap/*/current/.config; do
    [ -f "$d/bdex-framegen.conf" ] && rm -fv "$d/bdex-framegen.conf"
  done
  for d in "$HOME"/.var/app/*/.cache/bdex-framegen "$HOME"/snap/*/current/.cache/bdex-framegen; do
    [ -d "$d" ] && rm -rfv "$d"
  done
fi
true
