#!/usr/bin/env bash
# Builds the release archives the way the CI does, in Docker:
#   1. the SPIR-V with glslc on Ubuntu 24.04 — the compiler of the tested
#      build (the CI's "build" job);
#   2. the layer on Ubuntu 20.04 — glibc 2.31, GCC 10 — for x86_64 (64-bit
#      and 32-bit layers) and, cross-compiled, aarch64.
# Linked against glibc 2.31 with a static C++ runtime, the archives load on
# every distribution with glibc 2.31 or newer: Ubuntu 20.04, Debian 11,
# Fedora 32, openSUSE Leap 15, RHEL 9, SteamOS and everything since.
#   tools/ci/package-release.sh [outdir] [version]
# Needs Docker and network access (packages, Vulkan headers).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$(mkdir -p "${1:-$ROOT/dist}" && cd "${1:-$ROOT/dist}" && pwd)"
VERSION="${2:-}"
VULKAN_HEADERS=v1.4.309
CMAKE_URL=https://github.com/Kitware/CMake/releases/download/v3.30.5/cmake-3.30.5-linux-x86_64.tar.gz
work="$(mktemp -d)"
owner="$(id -u):$(id -g)"
trap 'docker run --rm -v "$work:/work" ubuntu:20.04 rm -rf /work/src /work/spv /work/spvbuild >/dev/null 2>&1; rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v -e '^build' -e '^dist/' | tar -cf - -T - | tar -xf - -C "$work/src")

echo "== SPIR-V (glslc, Ubuntu 24.04)"
docker run --rm -v "$ROOT/tools/ci/apt-get:/opt/bdex-ci/apt-get:ro" -v "$work:/work" ubuntu:24.04 bash -euc '
  export DEBIAN_FRONTEND=noninteractive
  [ ! -x /usr/bin/apt-get ] || export PATH="/opt/bdex-ci:$PATH"  # tools/ci/apt-get
  apt-get update -qq && apt-get install -y -qq --no-install-recommends cmake make g++ glslc libvulkan-dev \
    >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
  cmake -S /work/src -B /work/spvbuild -DBDEX_BUILD_TESTS=OFF -DBDEX_BUILD_DEMO=OFF -DBDEX_BUILD_OFFLINE=OFF >/dev/null
  cmake --build /work/spvbuild --target bdex_layer_shaders -j"$(nproc)" >/dev/null
  cp -r /work/spvbuild/layer/spv /work/spv
  echo "$(ls /work/spv | wc -l) SPIR-V modules, $(glslc --version | head -1)"'

# Ubuntu 20.04: glibc 2.31 like Debian 11, whose archives are gone, and
# maintained until 2030. GCC 10 (its default GCC 9 lacks some C++20), CMake
# from Kitware (static binary), newer Vulkan headers.
setup='
  export DEBIAN_FRONTEND=noninteractive
  [ ! -x /usr/bin/apt-get ] || export PATH="/opt/bdex-ci:$PATH"  # tools/ci/apt-get
  set -o pipefail
  retry() { for i in 1 2 3 4; do "$@" && return; echo "retry $i: $*" >&2; sleep $((i * 15)); done; return 1; }
  # apt-get retries by itself (tools/ci/apt-get, mounted).
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends make git ca-certificates curl python3 binutils $PACKAGES \
    >/tmp/apt.log 2>&1 || { tail -20 /tmp/apt.log; exit 1; }
  for t in gcc g++ $CROSS; do ln -sf "/usr/bin/$t-10" "/usr/local/bin/$t"; done
  retry curl -fsSL -o /tmp/cmake.tar.gz '"$CMAKE_URL"'
  tar -xzf /tmp/cmake.tar.gz -C /opt && ln -sf /opt/cmake-*/bin/cmake /usr/local/bin/cmake
  retry git clone -q --depth 1 --branch '"$VULKAN_HEADERS"' https://github.com/KhronosGroup/Vulkan-Headers /vh
  export BDEX_SPIRV_DIR=/work/spv BDEX_VULKAN_INCLUDE_DIR=/vh/include BDEX_MAX_GLIBC=2.31
  cd /work/src
  echo "$(g++ --version | head -1), cmake $(cmake --version | head -1 | cut -d" " -f3), $(ldd --version | head -1)"'
echo "== x86_64 (Ubuntu 20.04)"
docker run --rm -v "$ROOT/tools/ci/apt-get:/opt/bdex-ci/apt-get:ro" -v "$work:/work" -v "$OUT:/out" -e PACKAGES="gcc-10 g++-10 g++-10-multilib gcc-multilib g++-multilib pkg-config libvulkan-dev libx11-dev libxcomposite-dev libxdamage-dev libxext-dev libxfixes-dev" -e CROSS="" \
  ubuntu:20.04 bash -euc "$setup"'
  BDEX_ARCH=x86_64 tools/package-linux.sh /out "$1"
  chown '"$owner"' /out/*.tar.gz' _ "$VERSION"
echo "== aarch64 (Ubuntu 20.04, cross)"
docker run --rm -v "$ROOT/tools/ci/apt-get:/opt/bdex-ci/apt-get:ro" -v "$work:/work" -v "$OUT:/out" -e PACKAGES="gcc-10 g++-10 gcc-10-aarch64-linux-gnu g++-10-aarch64-linux-gnu" \
  -e CROSS="aarch64-linux-gnu-gcc aarch64-linux-gnu-g++" ubuntu:20.04 bash -euc "$setup"'
  BDEX_ARCH=aarch64 BDEX_TOOLCHAIN=/work/src/tools/ci/aarch64-linux-gnu.cmake tools/package-linux.sh /out "$1"
  chown '"$owner"' /out/*.tar.gz' _ "$VERSION"
