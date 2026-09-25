#!/usr/bin/env bash
# Builds the prebuilt Linux archive: bdex-framegen-linux-x86_64[-VERSION].tar.gz
# (64-bit layer, 32-bit layer when the multilib toolchain is there, the CLI,
# the control panel, profiles, install.sh / uninstall.sh). The layer links
# its C++ runtime statically (BDEX_STATIC_RUNTIME); build on the oldest glibc
# you want to support (the CI uses Ubuntu 22.04: glibc 2.35).
#   tools/package-linux.sh [outdir] [version]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)"
VERSION="${2:-}"
NAME="bdex-framegen-linux-x86_64${VERSION:+-$VERSION}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$NAME"
common=(-DCMAKE_BUILD_TYPE=Release -DBDEX_STATIC_RUNTIME=ON -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_DEMO=OFF
        -DBDEX_BUILD_OFFLINE=OFF -DCMAKE_INSTALL_PREFIX=/prefix)

cmake -S "$ROOT" -B "$WORK/b64" "${common[@]}" >/dev/null
cmake --build "$WORK/b64" -j"$(nproc)" >/dev/null
DESTDIR="$WORK/root" cmake --install "$WORK/b64" >/dev/null
if echo 'int main(){return 0;}' | g++ -m32 -x c++ - -o /dev/null 2>/dev/null; then
  cmake -S "$ROOT" -B "$WORK/b32" "${common[@]}" -DBDEX_32BIT=ON >/dev/null
  cmake --build "$WORK/b32" -j"$(nproc)" >/dev/null
  DESTDIR="$WORK/root" cmake --install "$WORK/b32" >/dev/null
else
  echo "package-linux: no 32-bit toolchain (g++ -m32): the archive has no 32-bit layer" >&2
fi

mkdir -p "$STAGE"
mv "$WORK/root/prefix" "$STAGE/files"
cp "$ROOT/packaging/linux/install.sh" "$ROOT/tools/uninstall.sh" "$ROOT/packaging/linux/README.txt" \
   "$ROOT/LICENSE" "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE/uninstall.sh"
# The runtime must not leak: only glibc, libm, libdl, libpthread.
for so in "$STAGE"/files/lib*/libVkLayer_bdex_framegen.so; do
  if readelf -d "$so" | grep -E 'NEEDED.*(libstdc\+\+|libgcc_s)'; then
    echo "package-linux: $so still needs the shared C++ runtime" >&2
    exit 1
  fi
done
tar -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "$OUT/$NAME.tar.gz"
tar -tzf "$OUT/$NAME.tar.gz" | sed 's/^/  /'
