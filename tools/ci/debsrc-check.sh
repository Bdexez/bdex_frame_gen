#!/usr/bin/env bash
# Builds the Debian source package (packaging/debian, copied to debian/ as a
# PPA or OBS would) from this checkout in containers, the 32-bit layer
# included on amd64, installs the .deb and checks it: both libraries in the
# multiarch directories, manifests naming them, no dependency on libc6:i386.
#   tools/ci/debsrc-check.sh [image...]   (default: ubuntu:24.04 ubuntu:22.04 debian:12)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
images=("$@"); [ ${#images[@]} -gt 0 ] || images=(ubuntu:24.04 ubuntu:22.04 debian:12)
for img in "${images[@]}"; do
  echo "== $img"
  docker run --rm -v "$ROOT":/src:ro "$img" bash -euo pipefail -c '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq >/dev/null
    apt-get install -y -qq --no-install-recommends dpkg-dev devscripts equivs file >/dev/null
    mkdir -p /b && cd /b
    tar -C /src --exclude="./build*" --exclude=./dist --exclude=./.git -cf - . | (mkdir src && tar -C src -xf -)
    cd src && cp -r packaging/debian debian
    mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" debian/control >/dev/null 2>&1
    dpkg-buildpackage -us -uc -b > /tmp/build.log 2>&1 || { tail -50 /tmp/build.log; exit 1; }
    deb=$(ls /b/bdex-framegen_*.deb)
    deps="$(dpkg-deb -f "$deb" Depends)"
    echo "built $(basename "$deb"): Depends: $deps"
    if grep -q "i386" <<<"$deps"; then echo "FAIL: depends on an i386 package"; exit 1; fi
    apt-get install -y -qq "$deb" >/dev/null
    file -L /usr/lib/x86_64-linux-gnu/libVkLayer_bdex_framegen.so | grep -q "ELF 64-bit"
    file -L /usr/lib/i386-linux-gnu/libVkLayer_bdex_framegen.so | grep -q "ELF 32-bit"
    grep -h library_path /usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen*.json
    grep -q "\"/usr/lib/i386-linux-gnu/libVkLayer_bdex_framegen.so\"" /usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json
    [ -f /usr/share/bdex-framegen/profiles.conf ] && command -v bdex-framegen-gui >/dev/null
    echo OK
  '
done
