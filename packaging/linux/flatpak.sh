#!/usr/bin/env bash
# The layer for game launchers installed as Flatpaks — Steam, Heroic, Lutris,
# Bottles — or as a Snap (Steam on Ubuntu). Their sandbox sees neither
# ~/.local nor ~/.config: the layer is copied into each such app's own
# directory, where its Vulkan loader looks ($XDG_DATA_HOME =
# ~/.var/app/<app>/data; a snap's HOME is ~/snap/<name>/current, the library
# goes in ~/snap/<name>/common, kept across its revisions), with a copy of the
# settings (the control panel keeps it up to date).
#   flatpak.sh <prefix>     install from an installed prefix (its manifests
#                           <prefix>/share/vulkan/implicit_layer.d/*.json name the libraries)
#   flatpak.sh --remove     remove it again
set -euo pipefail
APPS=(com.valvesoftware.Steam com.heroicgameslauncher.hgl net.lutris.Lutris com.usebottles.bottles)
arg="${1:?usage: flatpak.sh <prefix> | --remove}"
cfg="${XDG_CONFIG_HOME:-$HOME/.config}/bdex-framegen.conf"
for app in "${APPS[@]}"; do
  d="$HOME/.var/app/$app"
  [ -d "$d" ] || continue
  dest="$d/data/bdex-framegen"
  manifests="$d/data/vulkan/implicit_layer.d"
  if [ "$arg" = --remove ]; then
    rm -rf "$dest"
    rm -f "$manifests"/VK_LAYER_BDEX_framegen.json "$manifests"/VK_LAYER_BDEX_framegen_32.json
    echo "  removed from $app (Flatpak)"
    continue
  fi
  mkdir -p "$dest/share/bdex-framegen" "$manifests"
  for suffix in "" _32; do
    json="VK_LAYER_BDEX_framegen$suffix.json"
    [ -f "$arg/share/vulkan/implicit_layer.d/$json" ] || continue
    # The library the manifest names: <prefix>/lib[32] for install.sh, the
    # distribution's library dir for a package (lib/x86_64-linux-gnu, lib64).
    so="$(sed -n 's/.*"library_path": *"\([^"]*\)".*/\1/p' "$arg/share/vulkan/implicit_layer.d/$json")"
    [ -f "$so" ] || continue
    lib=lib
    if [ -n "$suffix" ]; then lib=lib32; fi
    mkdir -p "$dest/$lib"
    cp "$so" "$dest/$lib/libVkLayer_bdex_framegen.so"
    # The same absolute path inside the sandbox: Flatpak mounts the app's
    # directory where it is.
    sed "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$dest/$lib/libVkLayer_bdex_framegen.so\"#" \
      "$arg/share/vulkan/implicit_layer.d/$json" > "$manifests/$json"
  done
  if [ -f "$arg/share/bdex-framegen/profiles.conf" ]; then
    cp "$arg/share/bdex-framegen/profiles.conf" "$dest/share/bdex-framegen/"
  fi
  if [ -f "$cfg" ]; then
    mkdir -p "$d/config"
    cp "$cfg" "$d/config/bdex-framegen.conf"
  fi
  echo "  also installed for $app (Flatpak): $dest"
done

# Snaps: HOME=~/snap/<name>/<revision> ("current" links the one in use; snapd
# copies it to the next revision on refresh).
for app in steam; do
  base="$HOME/snap/$app"
  [ -d "$base/current" ] || continue
  dest="$base/common/bdex-framegen"
  manifests="$base/current/.local/share/vulkan/implicit_layer.d"
  if [ "$arg" = --remove ]; then
    rm -rf "$dest"
    rm -f "$manifests"/VK_LAYER_BDEX_framegen.json "$manifests"/VK_LAYER_BDEX_framegen_32.json
    echo "  removed from $app (Snap)"
    continue
  fi
  mkdir -p "$dest/share/bdex-framegen" "$manifests"
  for suffix in "" _32; do
    json="VK_LAYER_BDEX_framegen$suffix.json"
    [ -f "$arg/share/vulkan/implicit_layer.d/$json" ] || continue
    so="$(sed -n 's/.*"library_path": *"\([^"]*\)".*/\1/p' "$arg/share/vulkan/implicit_layer.d/$json")"
    [ -f "$so" ] || continue
    lib=lib
    if [ -n "$suffix" ]; then lib=lib32; fi
    mkdir -p "$dest/$lib"
    cp "$so" "$dest/$lib/libVkLayer_bdex_framegen.so"
    sed "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"$dest/$lib/libVkLayer_bdex_framegen.so\"#" \
      "$arg/share/vulkan/implicit_layer.d/$json" > "$manifests/$json"
  done
  if [ -f "$arg/share/bdex-framegen/profiles.conf" ]; then
    cp "$arg/share/bdex-framegen/profiles.conf" "$dest/share/bdex-framegen/"
  fi
  if [ -f "$cfg" ]; then
    mkdir -p "$base/current/.config"
    cp "$cfg" "$base/current/.config/bdex-framegen.conf"
  fi
  echo "  also installed for $app (Snap): $dest"
done
