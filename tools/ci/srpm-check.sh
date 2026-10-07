#!/usr/bin/env bash
# Builds packaging/rpm/bdex-framegen.spec (the COPR / OBS source package) from
# this checkout in Fedora and openSUSE containers, the 32-bit layer included,
# installs the result and checks it: both libraries, manifests naming them, no
# dependency on a 32-bit glibc.
#   tools/ci/srpm-check.sh [image...]   (default: fedora:latest opensuse/tumbleweed)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="$(sed -n 's/^Version: *//p' "$ROOT/packaging/rpm/bdex-framegen.spec")"
images=("$@"); [ ${#images[@]} -gt 0 ] || images=(fedora:latest opensuse/tumbleweed)
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
# The tarball GitHub would serve for v$VERSION, from the working tree.
tar -C "$ROOT/.." --transform "s#^$(basename "$ROOT")#bdex_frame_gen-$VERSION#" \
  --exclude-vcs --exclude='build*' --exclude=dist --exclude=__pycache__ \
  -czf "$WORK/bdex_frame_gen-$VERSION.tar.gz" "$(basename "$ROOT")"
for img in "${images[@]}"; do
  echo "== $img"
  docker run --rm -v "$WORK":/w:ro -v "$ROOT/packaging/rpm":/spec:ro "$img" bash -euo pipefail -c '
    if command -v dnf >/dev/null; then
      dnf -y -q install rpm-build "dnf-command(builddep)" findutils file >/dev/null
      dnf -y -q builddep /spec/bdex-framegen.spec >/dev/null
      inst() { dnf -y -q install "$@" >/dev/null; }
    else
      zypper -n -q install rpm-build findutils file >/dev/null
      # (zypper has no builddep: the BuildRequires of the spec, as listed)
      zypper -n -q install $(rpmspec -q --buildrequires /spec/bdex-framegen.spec | sed "s/ >=.*//") >/dev/null
      inst() { zypper -n -q --no-gpg-checks install --allow-unsigned-rpm "$@" >/dev/null; }
    fi
    src="$(rpm --eval %_sourcedir)"; mkdir -p "$src" && cp /w/*.tar.gz "$src/"
    rpmbuild -ba /spec/bdex-framegen.spec > /tmp/rpmbuild.log 2>&1 || { tail -50 /tmp/rpmbuild.log; exit 1; }
    rpm=$(ls "$(rpm --eval %_rpmdir)"/x86_64/bdex-framegen-[0-9]*.rpm)
    echo "built $(basename "$rpm"), requires: $(rpm -qpR "$rpm" | grep -v "^rpmlib" | tr "\n" " ")"
    if rpm -qpR "$rpm" | grep -E "^libc\.so\.6(\(GLIBC[^)]*\))?$|^libstdc"; then
      echo "FAIL: requires a 32-bit glibc / libstdc++"; exit 1
    fi
    inst "$rpm"
    for f in /usr/lib64/libVkLayer_bdex_framegen.so /usr/lib/libVkLayer_bdex_framegen.so; do
      file -L "$f" | grep -q "ELF $( [ "$f" = /usr/lib/libVkLayer_bdex_framegen.so ] && echo 32 || echo 64)-bit" ||
        { echo "FAIL: $f"; exit 1; }
    done
    grep -h library_path /usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen*.json
    grep -q "\"/usr/lib/libVkLayer_bdex_framegen.so\"" /usr/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json
    [ -f /usr/share/bdex-framegen/profiles.conf ] && command -v bdex-framegen >/dev/null
    echo "OK"
  '
done
