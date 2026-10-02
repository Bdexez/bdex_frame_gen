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
#   flatpak.sh --check <prefix>
#                           list the sandboxes whose copy of the layer differs
#                           from the prefix's (left behind by an update of the
#                           layer): "<app> <copy>" per line, exit 1 if any
set -euo pipefail
APPS=(com.valvesoftware.Steam com.heroicgameslauncher.hgl net.lutris.Lutris com.usebottles.bottles)
arg="${1:?usage: flatpak.sh <prefix> | --remove | --check <prefix>}"
check=0
if [ "$arg" = --check ]; then check=1; arg="${2:?usage: flatpak.sh --check <prefix>}"; fi
stale=0
# Replaces a file by a new inode (copy then rename): a running game keeps the
# library it mapped — cp over it would truncate that mapping (SIGBUS).
put() { cp "$1" "$2.tmp.$$" && mv -f "$2.tmp.$$" "$2"; }
# The library a manifest of the prefix names.
host_so() { sed -n 's/.*"library_path": *"\([^"]*\)".*/\1/p' "$arg/share/vulkan/implicit_layer.d/$1"; }
# Same contents? (cmp is in diffutils, missing from minimal systems; cksum is
# in coreutils and busybox.)
same() {
  if command -v cmp >/dev/null; then cmp -s "$1" "$2"; else [ "$(cksum < "$1")" = "$(cksum < "$2")" ]; fi
}
# The layer's Flatpak extension (packaging/flatpak, Flathub), for the
# Freedesktop runtime branch an app runs on: then the app needs no copy, the
# extension loads (and `flatpak update` updates) the layer. Prints the branch.
EXT=org.freedesktop.Platform.VulkanLayer.BdexFramegen
covered_by_extension() {
  local app="$1" meta rt branch root
  for root in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
    meta="$root/app/$app/current/active/metadata"
    [ -f "$meta" ] || continue
    rt="$(sed -n 's/^runtime=//p' "$meta" | head -1)"
    case "$rt" in org.freedesktop.Platform/*) ;; *) return 1 ;; esac
    branch="${rt##*/}"
    for root in "$HOME/.local/share/flatpak" /var/lib/flatpak; do
      if [ -d "$root/runtime/$EXT/$(uname -m)/$branch" ]; then echo "$branch"; return 0; fi
    done
    return 1
  done
  return 1
}
# --check: one sandbox (name, its lib dir root), compared with the prefix.
check_one() {
  local name="$1" dest="$2" suffix json so lib
  for suffix in "" _32; do
    json="VK_LAYER_BDEX_framegen$suffix.json"
    [ -f "$arg/share/vulkan/implicit_layer.d/$json" ] || continue
    so="$(host_so "$json")"
    [ -f "$so" ] || continue
    lib=lib; [ -n "$suffix" ] && lib=lib32
    [ -f "$dest/$lib/libVkLayer_bdex_framegen.so" ] || continue
    if ! same "$so" "$dest/$lib/libVkLayer_bdex_framegen.so"; then
      echo "$name $dest/$lib/libVkLayer_bdex_framegen.so"
      stale=1
    fi
  done
}
cfg="${XDG_CONFIG_HOME:-$HOME/.config}/bdex-framegen.conf"
for app in "${APPS[@]}"; do
  d="$HOME/.var/app/$app"
  [ -d "$d" ] || continue
  dest="$d/data/bdex-framegen"
  manifests="$d/data/vulkan/implicit_layer.d"
  if [ "$check" = 1 ]; then
    [ -f "$manifests/VK_LAYER_BDEX_framegen.json" ] && check_one "$app" "$dest"
    continue
  fi
  if [ "$arg" = --remove ]; then
    rm -rf "$dest"
    rm -f "$manifests"/VK_LAYER_BDEX_framegen.json "$manifests"/VK_LAYER_BDEX_framegen_32.json
    echo "  removed from $app (Flatpak)"
    continue
  fi
  if branch="$(covered_by_extension "$app")"; then
    # No copy beside the extension (a stale one would load first): only the
    # settings, and a mark for the panel (sessions, settings mirror).
    rm -rf "$dest/lib" "$dest/lib32"
    rm -f "$manifests"/VK_LAYER_BDEX_framegen.json "$manifests"/VK_LAYER_BDEX_framegen_32.json
    mkdir -p "$dest"; echo "$branch" > "$dest/extension"
    if [ -f "$cfg" ]; then mkdir -p "$d/config"; put "$cfg" "$d/config/bdex-framegen.conf"; fi
    echo "  $app (Flatpak): layer provided by the $EXT extension ($branch)"
    continue
  fi
  rm -f "$dest/extension"
  mkdir -p "$dest/share/bdex-framegen" "$manifests"
  for suffix in "" _32; do
    json="VK_LAYER_BDEX_framegen$suffix.json"
    [ -f "$arg/share/vulkan/implicit_layer.d/$json" ] || continue
    # The library the manifest names: <prefix>/lib[32] for install.sh, the
    # distribution's library dir for a package (lib/x86_64-linux-gnu, lib64).
    so="$(host_so "$json")"
    [ -f "$so" ] || continue
    lib=lib
    if [ -n "$suffix" ]; then lib=lib32; fi
    mkdir -p "$dest/$lib"
    put "$so" "$dest/$lib/libVkLayer_bdex_framegen.so"
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
  if [ "$check" = 1 ]; then
    [ -f "$manifests/VK_LAYER_BDEX_framegen.json" ] && check_one "snap:$app" "$dest"
    continue
  fi
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
    so="$(host_so "$json")"
    [ -f "$so" ] || continue
    lib=lib
    if [ -n "$suffix" ]; then lib=lib32; fi
    mkdir -p "$dest/$lib"
    put "$so" "$dest/$lib/libVkLayer_bdex_framegen.so"
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
[ "$check" = 0 ] || exit "$stale"
