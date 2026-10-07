#!/usr/bin/env bash
# The Linux install scripts, in a scratch home (no GPU, no display):
# - packaging/linux/flatpak.sh copies the layer into Flatpak / Snap launchers,
#   reports the copies an update left behind (--check), re-copies them, and
#   removes them;
# - every copy over an installed library goes through a new inode (a running
#   game keeps the library it mapped: an in-place copy truncates it, SIGBUS),
#   in flatpak.sh and in the archive's install.sh.
#   tests/linux_scripts.sh <source dir>
set -euo pipefail
SRC="$(cd "${1:?usage: linux_scripts.sh <source dir>}" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config"
mkdir -p "$HOME/.var/app/com.valvesoftware.Steam" "$HOME/.var/app/net.lutris.Lutris" "$HOME/snap/steam/current"
pre="$T/prefix"
mkdir -p "$pre/lib" "$pre/lib32" "$pre/share/vulkan/implicit_layer.d" "$pre/share/bdex-framegen"
echo v1 > "$pre/lib/libVkLayer_bdex_framegen.so"
echo v1-32 > "$pre/lib32/libVkLayer_bdex_framegen.so"
echo "[*]" > "$pre/share/bdex-framegen/profiles.conf"
for s in "" _32; do
  lib=lib; [ -n "$s" ] && lib=lib32
  printf '{"file_format_version": "1.1.2", "layer": {"library_path": "%s"}}\n' \
    "$pre/$lib/libVkLayer_bdex_framegen.so" > "$pre/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen$s.json"
done
mkdir -p "$XDG_CONFIG_HOME"; echo "enabled = 1" > "$XDG_CONFIG_HOME/bdex-framegen.conf"

FP="$SRC/packaging/linux/flatpak.sh"
bash "$FP" "$pre" > /dev/null
steam="$HOME/.var/app/com.valvesoftware.Steam"
copy="$steam/data/bdex-framegen/lib/libVkLayer_bdex_framegen.so"
snapcopy="$HOME/snap/steam/common/bdex-framegen/lib32/libVkLayer_bdex_framegen.so"
[ "$(cat "$copy")" = v1 ] || fail "Flatpak copy"
[ "$(cat "$snapcopy")" = v1-32 ] || fail "Snap 32-bit copy"
grep -q "\"library_path\": \"$copy\"" "$steam/data/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json" ||
  fail "Flatpak manifest does not name the copy"
[ -f "$steam/config/bdex-framegen.conf" ] || fail "settings not copied"
out="$(bash "$FP" --check "$pre")" || fail "fresh copies reported stale: $out"
[ -z "$out" ] || fail "fresh copies listed: $out"

# An update of the layer: --check lists every copy left behind, exit 1.
echo v2 > "$pre/lib/libVkLayer_bdex_framegen.so"
rc=0; out="$(bash "$FP" --check "$pre")" || rc=$?
[ "$rc" = 1 ] || fail "--check exit $rc after an update"
grep -q "^com.valvesoftware.Steam $copy\$" <<<"$out" || fail "Steam copy not listed: $out"
grep -q "^net.lutris.Lutris " <<<"$out" || fail "Lutris copy not listed: $out"
grep -q "^snap:steam .*/lib/libVkLayer" <<<"$out" || fail "Snap copy not listed: $out"
if grep -q "lib32" <<<"$out"; then fail "unchanged 32-bit copy listed: $out"; fi

# Re-copy while a "game" holds the old library open: it keeps its inode.
exec 9< "$copy"
ino="$(stat -c %i "$copy")"
bash "$FP" "$pre" > /dev/null
[ "$(stat -c %i "$copy")" != "$ino" ] || fail "flatpak.sh copied over the installed library in place"
[ "$(cat <&9)" = v1 ] || fail "the open library changed under the game"
exec 9<&-
[ "$(cat "$copy")" = v2 ] || fail "copy not refreshed"
bash "$FP" --check "$pre" > /dev/null || fail "still stale after the re-copy"
ls "$steam/data/bdex-framegen/lib/" | grep -q tmp && fail "temporary file left behind"

bash "$FP" --remove > /dev/null
[ ! -e "$copy" ] && [ ! -e "$steam/data/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json" ] ||
  fail "--remove left the Flatpak copy"
[ ! -e "$snapcopy" ] || fail "--remove left the Snap copy"

# The archive's install.sh: new inodes for the libraries it replaces.
arc="$T/archive"
mkdir -p "$arc/files/lib" "$arc/files/share/bdex-framegen" "$arc/files/share/vulkan/implicit_layer.d"
cp "$SRC/packaging/linux/install.sh" "$SRC/packaging/linux/flatpak.sh" "$arc/"
uname -m > "$arc/files/share/bdex-framegen/ARCH"
echo new > "$arc/files/lib/libVkLayer_bdex_framegen.so"
printf '{"layer": {"library_path": "/x/lib/libVkLayer_bdex_framegen.so"}}\n' \
  > "$arc/files/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json"
inst="$T/installed"
mkdir -p "$inst/lib"; echo old > "$inst/lib/libVkLayer_bdex_framegen.so"
exec 9< "$inst/lib/libVkLayer_bdex_framegen.so"
bash "$arc/install.sh" "$inst" > /dev/null
[ "$(cat <&9)" = old ] || fail "install.sh copied over the installed library in place"
exec 9<&-
[ "$(cat "$inst/lib/libVkLayer_bdex_framegen.so")" = new ] || fail "install.sh did not install"
grep -q "\"library_path\": \"$inst/lib/libVkLayer_bdex_framegen.so\"" \
  "$inst/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json" || fail "install.sh manifest path"
echo "linux scripts OK"
