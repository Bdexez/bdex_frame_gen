#!/usr/bin/env bash
# Builds the project from source in other distributions' containers — each
# with its own compiler, CMake, Vulkan headers and shader compiler — and runs
# the tests that need no GPU. Catches what one distribution's toolchain lets
# through (an older GCC, a header a newer libstdc++ includes implicitly, old
# Vulkan headers, musl).
#   tools/ci/distro-check.sh [image...]     (default: the list below)
#   BDEX_DISTRO_CC=clang tools/ci/distro-check.sh fedora:latest   # with Clang
# Needs Docker. Works on a copy of the working tree (tracked + untracked files).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
images=("$@")
[ ${#images[@]} -gt 0 ] || images=(ubuntu:22.04 ubuntu:24.04 debian:12 fedora:latest archlinux:latest
                                  opensuse/tumbleweed alpine:latest)
cc="${BDEX_DISTRO_CC:-gcc}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build' | tar -cf - -T - | tar -xf - -C "$work/src")

cat > "$work/job.sh" <<'JOB'
set -euo pipefail
cc="$1"
# The build packages, then (best effort) what runs the demo through the layer
# on a virtual display: GLFW, Xvfb, Mesa's software Vulkan driver (lavapipe).
if command -v apt-get >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends cmake make g++ clang pkg-config libvulkan-dev python3 \
    glslang-tools >/dev/null
  apt-get install -y -qq --no-install-recommends glslc >/dev/null 2>&1 || true  # not in every release
  apt-get install -y -qq --no-install-recommends libglfw3-dev xvfb mesa-vulkan-drivers >/dev/null 2>&1 || true
elif command -v dnf >/dev/null; then
  dnf install -y -q cmake make gcc-c++ clang pkgconf-pkg-config vulkan-headers vulkan-loader-devel glslang \
    python3 >/dev/null
  dnf install -y -q glslc >/dev/null 2>&1 || true
  dnf install -y -q glfw-devel xorg-x11-server-Xvfb mesa-vulkan-drivers >/dev/null 2>&1 || true
elif command -v pacman >/dev/null; then
  pacman -Syu --noconfirm --needed cmake make gcc clang pkgconf vulkan-headers vulkan-icd-loader glslang \
    shaderc python >/dev/null
  pacman -S --noconfirm --needed glfw xorg-server-xvfb vulkan-swrast >/dev/null 2>&1 || true
elif command -v zypper >/dev/null; then
  zypper -n -q in cmake make gcc-c++ clang pkgconf vulkan-devel glslang-devel python3 gawk >/dev/null
  zypper -n -q in shaderc >/dev/null 2>&1 || true
  zypper -n -q in libglfw-devel xorg-x11-server-Xvfb Mesa-vulkan-device-select libvulkan_lvp >/dev/null 2>&1 || true
elif command -v apk >/dev/null; then
  apk add -q cmake make g++ clang pkgconf vulkan-headers vulkan-loader-dev glslang python3 linux-headers
  apk add -q glfw-dev xvfb mesa-vulkan-swrast >/dev/null 2>&1 || true
fi
if [ "$cc" = clang ]; then export CC=clang CXX=clang++; fi
. /etc/os-release
echo "== $PRETTY_NAME: $($cc --version | head -1), cmake $(cmake --version | head -1 | cut -d' ' -f3), loader $(pkg-config --modversion vulkan 2>/dev/null || echo ?), shaders: $(command -v glslc || command -v glslangValidator || command -v glslang)"
cp -r /work/src /tmp/src
cmake -S /tmp/src -B /tmp/b -DCMAKE_BUILD_TYPE=Release -DBDEX_WERROR=ON >/tmp/configure.log 2>&1 ||
  { cat /tmp/configure.log; exit 1; }
grep -E 'Vulkan|Shaders' /tmp/configure.log || true
cmake --build /tmp/b -j"$(nproc)" >/tmp/build.log 2>&1 || { grep -E -B2 -A6 'error' /tmp/build.log | head -60; exit 1; }
# The tests that need no GPU (the others skip themselves or are not built).
(cd /tmp/b && ctest -R 'unit|option_keys|linux_scripts' --output-on-failure) | tail -3
# The layer at work with this distribution's Vulkan loader and Mesa: the demo
# through it on Xvfb, rendered by lavapipe (tests/integration.sh: x2, and
# "generation on"). All inside the container.
icd="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -1 || true)"
# musl gives a new thread 128 KB of stack, and lavapipe compiles shaders with
# LLVM in its own threads: the engine's shaders overflow it (a limit of Mesa's
# software driver on musl, not of the layer; the GPU drivers compile without
# LLVM). Its threads get glibc's default 8 MB here.
preload=""
if ls /lib/ld-musl-*.so.1 >/dev/null 2>&1; then
  printf '%s\n' '#define _GNU_SOURCE' '#include <pthread.h>' \
    '__attribute__((constructor)) static void bigger_stacks(void) {' \
    '    pthread_attr_t a;' '    pthread_attr_init(&a);' '    pthread_attr_setstacksize(&a, 8u << 20);' \
    '    pthread_setattr_default_np(&a);' '}' > /tmp/stack.c
  "${CC:-cc}" -shared -fPIC -o /tmp/libstack.so /tmp/stack.c && preload=/tmp/libstack.so
fi
if [ -x /tmp/b/demo/bdex_demo ] && [ -n "$icd" ] && command -v Xvfb >/dev/null; then
  Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
  sleep 2
  # The whole log on a failure (the verdict, the layer's statistics), its
  # end otherwise.
  if DISPLAY=:99 VK_ICD_FILENAMES="$icd" BDEX_REQUIRE_DISPLAY=1 LD_PRELOAD="$preload" \
       bash /tmp/src/tests/integration.sh /tmp/b/demo/bdex_demo /tmp/b/layer >/tmp/integration.log 2>&1; then
    tail -3 /tmp/integration.log
  else
    grep -vE '^bdex demo - ' /tmp/integration.log | tail -40
    exit 1
  fi
else
  echo "runtime check skipped (no GLFW, Xvfb or lavapipe package here)"
fi
JOB

status=0
for img in "${images[@]}"; do
  # (Alpine has no bash until it is installed.)
  if docker run --rm ${BDEX_DOCKER_ARGS:-} -v "$work:/work:ro" "$img" sh -c \
       'command -v bash >/dev/null || apk add -q bash >/dev/null; exec bash /work/job.sh "$1"' sh "$cc" 2>&1 |
     sed "s|^|[$img] |"; then
    echo "[$img] OK"
  else
    echo "[$img] FAILED"
    status=1
  fi
done
exit $status
