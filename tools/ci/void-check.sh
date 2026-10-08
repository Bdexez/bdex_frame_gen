#!/usr/bin/env bash
# Builds packaging/void/bdex-framegen/template with xbps-src (void-packages)
# from this checkout, glibc and musl, installs the package and checks it.
#   tools/ci/void-check.sh [glibc|musl ...]   (default: both)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
V="$(sed -n 's/^version=//p' "$ROOT/packaging/void/bdex-framegen/template")"
libcs=("$@"); [ ${#libcs[@]} -gt 0 ] || libcs=(glibc musl)
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
# The tarball GitHub would serve for v$V, from the working tree.
tar -C "$ROOT/.." --transform "s#^$(basename "$ROOT")#bdex_frame_gen-$V#" \
  --exclude-vcs --exclude='build*' --exclude=dist --exclude=__pycache__ \
  -czf "$WORK/v$V.tar.gz" "$(basename "$ROOT")"
sum="$(sha256sum "$WORK/v$V.tar.gz" | cut -d' ' -f1)"
sed "s/@CHECKSUM@/$sum/" "$ROOT/packaging/void/bdex-framegen/template" > "$WORK/template"
for libc in "${libcs[@]}"; do
  echo "== void ($libc)"
  # (as void-packages' own CI: a user, the uchroot chroot style, privileged)
  docker run --rm --privileged -v "$WORK":/w:ro -e V="$V" -e LIBC="$libc" "ghcr.io/void-linux/void-$libc-full:latest" sh -euc '
    xbps-install -Syu xbps >/dev/null 2>&1; xbps-install -Syu >/dev/null 2>&1
    xbps-install -Sy git bash file sudo >/dev/null
    useradd -m -G xbuilder builder
    git clone -q --depth 1 https://github.com/void-linux/void-packages /vp
    cd /vp
    mkdir -p srcpkgs/bdex-framegen hostdir/sources/bdex-framegen-$V
    cp /w/template srcpkgs/bdex-framegen/template
    cp /w/v$V.tar.gz hostdir/sources/bdex-framegen-$V/
    echo XBPS_CHROOT_CMD=uchroot >> etc/conf
    chown -R builder /vp
    sudo -u builder ./xbps-src binary-bootstrap >/tmp/bootstrap.log 2>&1 || { tail -20 /tmp/bootstrap.log; exit 1; }
    sudo -u builder ./xbps-src pkg bdex-framegen >/tmp/pkg.log 2>&1 || { tail -40 /tmp/pkg.log; exit 1; }
    xbps-install -y --repository=hostdir/binpkgs bdex-framegen >/dev/null
    file -L /usr/lib/libVkLayer_bdex_framegen.so | grep -q "ELF 64-bit"
    grep -h library_path /usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json
    [ -f /usr/share/bdex-framegen/profiles.conf ] && [ -f /usr/share/licenses/bdex-framegen/LICENSE ]
    echo "void $LIBC OK"
  '
done
