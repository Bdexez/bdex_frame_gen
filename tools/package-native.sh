#!/usr/bin/env bash
# Distribution packages from a prebuilt archive (tools/package-linux.sh):
#   bdex-framegen_<version>_<amd64|arm64>.deb     Debian, Ubuntu, Mint, Pop!_OS...
#   bdex-framegen-<version>-1.<x86_64|aarch64>.rpm Fedora, Nobara, openSUSE, RHEL...
# The same files as the archive (the layer built against glibc 2.29/2.30 with
# its own C++ runtime, the 32-bit one for 32-bit games, the CLI, the panel),
# moved to the distribution's directories: the layer in its library dir
# (lib/x86_64-linux-gnu, lib64...; the 32-bit one in lib/i386-linux-gnu or
# lib), the manifests in /usr/share/vulkan/implicit_layer.d.
#   tools/package-native.sh ARCHIVE [outdir] [version]
# dpkg-deb and rpmbuild are used when installed, else Docker (debian:12,
# fedora:latest).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCHIVE="$(realpath "${1:?usage: package-native.sh ARCHIVE [outdir] [version]}")"
OUT="$(mkdir -p "${2:-$ROOT/dist}" && cd "${2:-$ROOT/dist}" && pwd)"
VERSION="${3:-}"
if [ -z "$VERSION" ]; then
  # bdex-framegen-linux-<arch>-<version>.tar.gz, else the sources' version.
  VERSION="$(basename "$ARCHIVE" .tar.gz | sed -n 's/^bdex-framegen-linux-[a-z0-9_]*-\([0-9].*\)$/\1/p')"
  [ -n "$VERSION" ] || VERSION="$(sed -n 's/^project(bdex_framegen VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
fi
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
tar -xzf "$ARCHIVE" -C "$WORK"
SRC="$(echo "$WORK"/bdex-framegen-linux-*)/files"
ARCH="$(cat "$SRC/share/bdex-framegen/ARCH" 2>/dev/null ||
        basename "$ARCHIVE" | sed -n 's/^bdex-framegen-linux-\(x86_64\|aarch64\).*/\1/p')"
case "$ARCH" in
  x86_64)  deb_arch=amd64 triplet=x86_64-linux-gnu triplet32=i386-linux-gnu glibc=2.29 ;;
  aarch64) deb_arch=arm64 triplet=aarch64-linux-gnu triplet32="" glibc=2.30 ;;
  *) echo "package-native: unknown architecture $ARCH" >&2; exit 1 ;;
esac
SUMMARY="Frame generation for Vulkan games (optical flow, x2 to x8), as a Vulkan layer"
DESCRIPTION="Generates frames between (or after) the game's own frames by GPU optical
flow, like Lossless Scaling / AFMF: native Vulkan games and Direct3D games
under Proton (DXVK, VKD3D-Proton). Loaded into every Vulkan program and inert
until turned on from the control panel (bdex-framegen-gui), per game with
BDEX_FG=1 %command%, or bdex-framegen -- <game>. Games protected by an
anti-cheat are left alone."

# stage ROOTDIR LIBDIR LIBDIR32: the archive's files under ROOTDIR/usr.
stage() {
  local root="$1" libdir="$2" libdir32="$3" suffix dir json
  mkdir -p "$root/usr"
  cp -r "$SRC/bin" "$SRC/share" "$root/usr/"
  rm -f "$root/usr/share/bdex-framegen/ARCH"
  cp "$ROOT/packaging/linux/flatpak.sh" "$root/usr/share/bdex-framegen/flatpak.sh"
  chmod 755 "$root/usr/share/bdex-framegen/flatpak.sh"
  for suffix in "" _32; do
    json="$root/usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen$suffix.json"
    [ -f "$json" ] || continue
    if [ -n "$suffix" ]; then
      [ -n "$libdir32" ] || { rm -f "$json"; continue; }
      dir="$libdir32"; from=lib32
    else
      dir="$libdir"; from=lib
    fi
    install -Dm755 "$SRC/$from/libVkLayer_bdex_framegen.so" "$root/usr/$dir/libVkLayer_bdex_framegen.so"
    sed -i "s#\"library_path\": \"[^\"]*\"#\"library_path\": \"/usr/$dir/libVkLayer_bdex_framegen.so\"#" "$json"
  done
  install -Dm644 "$ROOT/LICENSE" "$root/usr/share/licenses/bdex-framegen/LICENSE"
  find "$root" -type d -exec chmod 755 {} +
}

# ---- .deb
deb="$WORK/deb"
stage "$deb" "lib/$triplet" "${triplet32:+lib/$triplet32}"
mkdir -p "$deb/DEBIAN" "$deb/usr/share/doc/bdex-framegen"
mv "$deb/usr/share/licenses/bdex-framegen/LICENSE" "$deb/usr/share/doc/bdex-framegen/copyright"
rm -rf "$deb/usr/share/licenses"
cat > "$deb/DEBIAN/control" <<EOF
Package: bdex-framegen
Version: $VERSION
Architecture: $deb_arch
Maintainer: Bdexez <https://github.com/Bdexez/bdex_frame_gen>
Installed-Size: $(du -sk "$deb/usr" | cut -f1)
Depends: libc6 (>= $glibc), libvulkan1
Recommends: python3-gi, gir1.2-gtk-4.0, libx11-6, libxcomposite1, libxdamage1, libxext6, libxfixes3
Suggests: gir1.2-adw-1, vulkan-tools
Section: games
Priority: optional
Homepage: https://github.com/Bdexez/bdex_frame_gen
Description: $SUMMARY
$(echo "$DESCRIPTION" | sed 's/^/ /')
EOF
# Flatpak launchers do not see /usr: each user copies the layer into them.
cat > "$deb/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
  echo "bdex-framegen: Steam, Heroic, Lutris or Bottles installed as a Flatpak? Run once, as your user:"
  echo "    bdex-framegen --sandboxes"
fi
EOF
chmod 755 "$deb/DEBIAN/postinst"
debfile="bdex-framegen_${VERSION}_$deb_arch.deb"
if command -v dpkg-deb >/dev/null; then
  dpkg-deb --root-owner-group -Zxz --build "$deb" "$OUT/$debfile" >/dev/null
else
  docker run --rm -v "$WORK:/work" -v "$OUT:/out" debian:12 bash -euc \
    "dpkg-deb --root-owner-group -Zxz --build /work/deb /out/$debfile >/dev/null; chown $(id -u):$(id -g) /out/$debfile; chown -R $(id -u):$(id -g) /work"
fi
echo "$OUT/$debfile"

# ---- .rpm
rpm="$WORK/rpm"
if [ "$ARCH" = x86_64 ]; then stage "$rpm/root" lib64 lib; else stage "$rpm/root" lib64 ""; fi
(cd "$rpm/root" && find . -type f -o -type l | sed 's#^\.##' | sort) > "$rpm/files"
cat > "$rpm/bdex-framegen.spec" <<EOF
Name:           bdex-framegen
Version:        $(echo "$VERSION" | tr '-' '~')
Release:        1
Summary:        $SUMMARY
License:        LicenseRef-proprietary
URL:            https://github.com/Bdexez/bdex_frame_gen
# Prebuilt: the 32-bit layer must not pull a 32-bit glibc (32-bit games bring
# theirs), and the panel's Python dependencies are only recommended.
AutoReqProv:    no
Requires:       (vulkan-loader or libvulkan1)
Recommends:     python3-gobject
Recommends:     (gtk4 or typelib-1_0-Gtk-4_0)
Recommends:     (libXcomposite or libXcomposite1)
Recommends:     (libXdamage or libXdamage1)
Suggests:       vulkan-tools
%global debug_package %{nil}
%global __os_install_post %{nil}
%global _build_id_links none
%global source_date_epoch_from_changelog 0

%description
$DESCRIPTION

%install
cp -a %{_sourcedir}/root/. %{buildroot}/

%post
echo "bdex-framegen: Steam, Heroic, Lutris or Bottles installed as a Flatpak? Run once, as your user:"
echo "    bdex-framegen --sandboxes"

%files
%license /usr/share/licenses/bdex-framegen/LICENSE
$(grep -v '^/usr/share/licenses/' "$rpm/files")
EOF
rpmcmd="rpmbuild -bb --target $ARCH --define '_topdir /work/rpm/top' --define '_sourcedir /work/rpm' /work/rpm/bdex-framegen.spec >/work/rpm.log 2>&1 || { cat /work/rpm.log; exit 1; }"
if command -v rpmbuild >/dev/null; then
  (eval "${rpmcmd//\/work/$WORK}")
  cp "$WORK"/rpm/top/RPMS/*/*.rpm "$OUT/"
else
  docker run --rm -v "$WORK:/work" -v "$OUT:/out" fedora:latest bash -euc "
    dnf install -y -q rpm-build >/dev/null 2>&1
    $rpmcmd
    cp /work/rpm/top/RPMS/*/*.rpm /out/ && chown $(id -u):$(id -g) /out/bdex-framegen-*.rpm
    chown -R $(id -u):$(id -g) /work"
fi
ls "$OUT"/bdex-framegen-"$(echo "$VERSION" | tr '-' '~')"-1.*"$ARCH".rpm
