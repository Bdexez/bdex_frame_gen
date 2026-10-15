#!/usr/bin/env bash
# Builds a prebuilt Linux archive: bdex-framegen-linux-<arch>[-VERSION].tar.gz
# (the layer — x86_64: also the 32-bit one when the multilib toolchain is
# there —, the CLI, the control panel, profiles, install.sh / uninstall.sh).
# The layer links its C++ runtime statically (BDEX_STATIC_RUNTIME), so it only
# needs glibc: the archive runs on distributions whose glibc is at least the
# build machine's. The release build (tools/ci/package-release.sh, the CI) runs
# this on Ubuntu 20.04 — glibc 2.31 — and fails if a newer symbol slipped in.
#   tools/package-linux.sh [outdir] [version]
# Environment:
#   BDEX_ARCH=x86_64|aarch64   target (default: this machine's)
#   BDEX_TOOLCHAIN=<file>      CMake toolchain for a cross build (aarch64 on x86_64)
#   BDEX_SPIRV_DIR=<dir>       precompiled SPIR-V (no GLSL compiler on the build machine)
#   BDEX_VULKAN_INCLUDE_DIR=<dir>  Vulkan headers newer than the system's
#   BDEX_MAX_GLIBC=2.31        fail if the layer needs a newer glibc (empty: no check)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)"
VERSION="${2:-}"
ARCH="${BDEX_ARCH:-$(uname -m)}"
NAME="bdex-framegen-linux-$ARCH${VERSION:+-$VERSION}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$NAME"
common=(-DCMAKE_BUILD_TYPE=Release -DBDEX_STATIC_RUNTIME=ON -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_DEMO=OFF
        -DBDEX_BUILD_OFFLINE=OFF -DCMAKE_INSTALL_PREFIX=/prefix)
[ -n "${BDEX_SPIRV_DIR:-}" ] && common+=(-DBDEX_SPIRV_DIR="$BDEX_SPIRV_DIR")
[ -n "${BDEX_VULKAN_INCLUDE_DIR:-}" ] && common+=(-DBDEX_VULKAN_INCLUDE_DIR="$BDEX_VULKAN_INCLUDE_DIR")

target=()
if [ -n "${BDEX_TOOLCHAIN:-}" ]; then
  # A cross build cannot run the generator of the panel's options_ui.json:
  # a native build makes it (the file does not depend on the architecture).
  cmake -S "$ROOT" -B "$WORK/native" "${common[@]}" >/dev/null
  cmake --build "$WORK/native" --target bdex_options_ui -j"$(nproc)" >/dev/null
  target=(-DCMAKE_TOOLCHAIN_FILE="$BDEX_TOOLCHAIN" -DBDEX_OPTIONS_JSON="$WORK/native/options_ui.json")
fi
cmake -S "$ROOT" -B "$WORK/b64" "${common[@]}" "${target[@]}" >/dev/null
cmake --build "$WORK/b64" -j"$(nproc)" >/dev/null
DESTDIR="$WORK/root" cmake --install "$WORK/b64" >/dev/null
if [ "$ARCH" = x86_64 ] && echo 'int main(){return 0;}' | "${CXX:-g++}" -m32 -x c++ - -o /dev/null 2>/dev/null; then
  cmake -S "$ROOT" -B "$WORK/b32" "${common[@]}" -DBDEX_32BIT=ON >/dev/null
  cmake --build "$WORK/b32" -j"$(nproc)" >/dev/null
  DESTDIR="$WORK/root" cmake --install "$WORK/b32" >/dev/null
elif [ "$ARCH" = x86_64 ]; then
  echo "package-linux: no 32-bit toolchain (g++ -m32): the archive has no 32-bit layer" >&2
fi

mkdir -p "$STAGE"
mv "$WORK/root/prefix" "$STAGE/files"
cp "$ROOT/packaging/linux/install.sh" "$ROOT/packaging/linux/flatpak.sh" "$ROOT/tools/uninstall.sh" \
   "$ROOT/packaging/linux/README.txt" "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/"
chmod +x "$STAGE/install.sh" "$STAGE/flatpak.sh" "$STAGE/uninstall.sh"
echo "$ARCH" > "$STAGE/files/share/bdex-framegen/ARCH"
# The runtime must not leak: only glibc (libc, libm, libdl, libpthread), and
# nothing newer than the oldest glibc the archive is meant for. (readelf reads
# any architecture.)
max="${BDEX_MAX_GLIBC-}"
for so in "$STAGE"/files/lib*/libVkLayer_bdex_framegen.so; do
  if readelf -d "$so" | grep -E 'NEEDED.*(libstdc\+\+|libgcc_s)'; then
    echo "package-linux: $so still needs the shared C++ runtime" >&2
    exit 1
  fi
  need="$(readelf -V "$so" | grep -oE 'GLIBC_[0-9]+\.[0-9]+(\.[0-9]+)?' | sed 's/GLIBC_//' | sort -Vu | tail -1)"
  echo "package-linux: $(basename "$(dirname "$so")")/$(basename "$so") needs glibc $need"
  if [ -n "$max" ] && [ "$(printf '%s\n%s\n' "$need" "$max" | sort -V | tail -1)" != "$max" ]; then
    echo "package-linux: $so needs glibc $need, newer than $max: build on an older distribution" >&2
    exit 1
  fi
done
tar -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "$OUT/$NAME.tar.gz"
tar -tzf "$OUT/$NAME.tar.gz" | sed 's/^/  /'
