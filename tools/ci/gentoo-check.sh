#!/usr/bin/env bash
# Builds the Gentoo ebuild (packaging/gentoo, a local overlay) from this
# checkout in a stage3 container, both ABIs (abi_x86_32: the 32-bit layer),
# through the ebuild's phases up to the image, and checks it: both layers in
# lib64 / lib, manifests naming them. (Its run-time dependency,
# vulkan-loader[abi_x86_32], is not merged: a 32-bit X/Wayland stack to
# compile.)
#   tools/ci/gentoo-check.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V="$(sed -n 's/^project(bdex_framegen VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
tar -C "$ROOT/.." --transform "s#^$(basename "$ROOT")#bdex_frame_gen-$V#" \
  --exclude-vcs --exclude='build*' --exclude=dist --exclude=__pycache__ \
  -czf "$WORK/bdex-framegen-$V.tar.gz" "$(basename "$ROOT")"
docker run --rm -v "$WORK":/w:ro -v "$ROOT/packaging/gentoo":/overlay-src:ro -e V="$V" gentoo/stage3 bash -euo pipefail -c '
  emerge-webrsync -q >/dev/null 2>&1
  # Binary packages for the build tools (compiling cmake / shaderc takes long).
  emerge -q --getbinpkg --oneshot dev-build/cmake dev-util/vulkan-headers dev-util/glslang >/tmp/deps.log 2>&1 ||
    { tail -30 /tmp/deps.log; exit 1; }
  cp -r /overlay-src /var/db/repos/bdex
  mkdir -p /etc/portage/repos.conf
  printf "[bdex-framegen]\nlocation = /var/db/repos/bdex\n" > /etc/portage/repos.conf/bdex.conf
  cd /var/db/repos/bdex/games-util/bdex-framegen
  cp bdex-framegen-9999.ebuild bdex-framegen-$V.ebuild
  mkdir -p /var/cache/distfiles && cp /w/bdex-framegen-$V.tar.gz /var/cache/distfiles/
  ebuild bdex-framegen-$V.ebuild manifest >/dev/null
  export ABI_X86="64 32" USE="-gui"
  ebuild bdex-framegen-$V.ebuild clean install > /tmp/ebuild.log 2>&1 || { tail -50 /tmp/ebuild.log; exit 1; }
  img=/var/tmp/portage/games-util/bdex-framegen-$V/image
  (cd $img && find . -type f | sort)
  file -L $img/usr/lib64/libVkLayer_bdex_framegen.so | grep -q "ELF 64-bit"
  file -L $img/usr/lib/libVkLayer_bdex_framegen.so | grep -q "ELF 32-bit"
  grep -h library_path $img/usr/share/vulkan/implicit_layer.d/*.json
  grep -q "\"/usr/lib/libVkLayer_bdex_framegen.so\"" $img/usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json
  [ ! -e $img/usr/bin/bdex-framegen-gui ] && [ -e $img/usr/bin/bdex-framegen ]
  echo "gentoo ebuild OK"
'
