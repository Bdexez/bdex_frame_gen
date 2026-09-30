#!/usr/bin/env bash
# Installs a prebuilt archive in other distributions, as a user would
# (./install.sh) — or a distribution package (tools/package-native.sh: .deb
# with apt, .rpm with dnf or zypper) —, and checks it there on Mesa's software driver, with their
# glibc, Vulkan loader and Mesa — no GPU, no window on the host:
#   - the layer loads and turns active (vulkaninfo creates a device);
#   - it generates frames: vkcube through it on a virtual display (Xvfb), x2
#     (skipped where Mesa cannot present to Xvfb: Mesa 20, Debian 11).
#   tools/ci/archive-check.sh <x86_64 archive> [image...]    (default: ubuntu:20.04 debian:11 ubuntu:24.04)
#   tools/ci/archive-check.sh <aarch64 archive> [suite...]   (Debian suites, default: bullseye bookworm)
#   tools/ci/archive-check.sh <.deb | .rpm> [image...]       (default: ubuntu:22.04 debian:12 | fedora:latest opensuse/tumbleweed)
# An aarch64 archive is checked in a Debian arm64 tree run by qemu-user inside
# an x86_64 container: no binfmt registration on the host, Docker is all it
# needs.
set -uo pipefail
archive="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
targets=("$@")
case "$archive" in
  *.deb) arm64=0; [ ${#targets[@]} -gt 0 ] || targets=(ubuntu:22.04 debian:12) ;;
  *.rpm) arm64=0; [ ${#targets[@]} -gt 0 ] || targets=(fedora:latest opensuse/tumbleweed) ;;
  *aarch64*) arm64=1; [ ${#targets[@]} -gt 0 ] || targets=(bullseye bookworm) ;;
  *) arm64=0; [ ${#targets[@]} -gt 0 ] || targets=(ubuntu:20.04 debian:11 ubuntu:24.04) ;;
esac
job="$(mktemp)"
trap 'rm -f "$job"' EXIT
cat > "$job" <<'JOB'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
suite="${1:-}"
if [ -n "$suite" ]; then
  # arm64: a Debian tree unpacked (nothing run in it), its programs run by
  # qemu-user with the tree as their root for the files they open.
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends qemu-user-static debootstrap ca-certificates binutils \
    xvfb xauth >/dev/null
  debootstrap --arch=arm64 --foreign --variant=minbase --include=mesa-vulkan-drivers,vulkan-tools,libvulkan1 \
    "$suite" /r http://deb.debian.org/debian >/tmp/debootstrap.log 2>&1 || { tail -5 /tmp/debootstrap.log; exit 1; }
  for d in /r/var/cache/apt/archives/*.deb; do dpkg-deb -x "$d" /r; done
  . /r/etc/os-release
  libc="$(ls /r/lib/aarch64-linux-gnu/libc.so.6 /r/usr/lib/aarch64-linux-gnu/libc.so.6 2>/dev/null | head -1)"
  echo "== $PRETTY_NAME (arm64 under qemu-user), glibc $(strings "$libc" | grep -oE 'release version [0-9]+\.[0-9]+' | head -1 | cut -d' ' -f3)"
  emu=(env LD_LIBRARY_PATH=/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu qemu-aarch64-static -L /r)
  bin=/r/usr/bin
  icd="/$(cd /r && ls usr/share/vulkan/icd.d/lvp_icd*.json | head -1)"
  secs=60 stats=4  # (emulated: ~2 game frames a second)
  # install.sh checks the machine (uname -m) against the archive. The layer is
  # installed at real paths: qemu-user looks up open() in the tree first but
  # not faccessat(), with which the loader checks that a manifest exists.
  mkdir -p /tmp/fake && printf '#!/bin/sh\necho aarch64\n' > /tmp/fake/uname && chmod +x /tmp/fake/uname
  install_path=/tmp/fake:$PATH
else
  if command -v apt-get >/dev/null; then
    apt-get update -qq
    if ! apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers vulkan-tools xvfb xauth >/dev/null 2>&1; then
      # A release past its end of life: its security updates are being moved
      # away (their index names packages no longer served); the release's own
      # packages suffice here.
      sed -i '/-security/d' /etc/apt/sources.list
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends mesa-vulkan-drivers vulkan-tools xvfb xauth >/dev/null
    fi
  elif command -v dnf >/dev/null; then
    dnf install -y -q mesa-vulkan-drivers vulkan-tools xorg-x11-server-Xvfb >/tmp/dnf.log 2>&1 ||
      { tail -5 /tmp/dnf.log; exit 1; }
  elif command -v zypper >/dev/null; then
    zypper -q -n install libvulkan_lvp vulkan-tools xorg-x11-server-Xvfb gawk >/tmp/zypper.log 2>&1 ||
      { tail -5 /tmp/zypper.log; exit 1; }
  fi
  . /etc/os-release
  echo "== $PRETTY_NAME ($(uname -m)), $(ldd --version | head -1)"
  emu=()
  bin=/usr/bin
  icd="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)"
  secs=12 stats=2
  install_path=$PATH
fi
export HOME=/tmp/home XDG_DATA_HOME=/tmp/home/.local/share
mkdir -p "$HOME"
case "$(ls /work/pkg.*)" in
  *.deb) apt-get install -y -qq --no-install-recommends /work/pkg.deb >/tmp/pkg.log 2>&1 || { tail -5 /tmp/pkg.log; exit 1; }
         echo "installed $(dpkg-query -W bdex-framegen), $(dpkg -L bdex-framegen | grep '\.so$' | tr '\n' ' ')" ;;
  *.rpm) if command -v dnf >/dev/null; then dnf install -y -q /work/pkg.rpm >/tmp/pkg.log 2>&1
         else zypper -q -n --no-gpg-checks install --allow-unsigned-rpm /work/pkg.rpm >/tmp/pkg.log 2>&1; fi ||
           { tail -5 /tmp/pkg.log; exit 1; }
         echo "installed $(rpm -q bdex-framegen), $(rpm -ql bdex-framegen | grep '\.so$' | tr '\n' ' ')" ;;
  *) mkdir -p /tmp/a && tar -xzf /work/pkg.tar.gz -C /tmp/a
     PATH=$install_path bash /tmp/a/*/install.sh >/dev/null ;;
esac
export BDEX_FG=1 BDEX_FG_LOG=2 VK_ICD_FILENAMES="$icd"
run() { ${emu[@]+"${emu[@]}"} "$bin/$1" "${@:2}"; }
out="$(VK_LOADER_DEBUG=warn run vulkaninfo --summary 2>&1 || true)"
if grep -qE '^USAGE: .*vulkaninfo' <<<"$out"; then  # (no --summary before 1.2.1xx)
  out="$(VK_LOADER_DEBUG=warn run vulkaninfo 2>&1 || true)"
fi
grep -E 'bdex-fg.*(loaded|layer (in)?active|error|failed)' <<<"$out" | cut -c1-140 | head -4
# The shipped profiles are found from the library's own directory, however
# deep the distribution puts it (lib/x86_64-linux-gnu): a profile for
# vulkaninfo shows in its startup line.
profiles="$(ls /usr/share/bdex-framegen/profiles.conf "$HOME"/.local/share/bdex-framegen/profiles.conf 2>/dev/null | head -1 || true)"
if [ -n "$profiles" ] && [ -z "${emu[*]:-}" ]; then
  cp "$profiles" /tmp/profiles.orig
  printf '\n[vulkaninfo]\nmultiplier = 3\n' >> "$profiles"
  grep -q 'loaded (enabled=1 multiplier=3' <<<"$(run vulkaninfo --summary 2>&1; run vulkaninfo 2>&1 | head -40)" ||
    { echo "the shipped profiles ($profiles) are not read"; exit 1; }
  cp /tmp/profiles.orig "$profiles"
  echo "profiles found: $profiles"
fi
# Every loader accepts the manifest's format (1.2.x: loaders before 1.3 warn
# that it is invalid).
if grep -E 'VK_LAYER_BDEX_framegen[_0-9]*\.json.*invalid' <<<"$out" | cut -c1-160 | head -2 | grep .; then
  exit 1
fi
# Through vkCreateDevice: active, or inactive for want of VK_KHR_swapchain
# (older vulkaninfo creates its device without it).
grep -qE "device '.*': layer (active|inactive)" <<<"$out"

# Generation: vkcube through the layer on Xvfb (in this container).
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
cube="$(DISPLAY=:99 BDEX_FG_STATS_INTERVAL=$stats timeout $secs ${emu[@]+"${emu[@]}"} "$bin/vkcube" 2>&1 || true)"
if grep -qE 'No DRI3 support' <<<"$cube"; then
  echo "(generation not checked: this Mesa cannot present to Xvfb)"
  exit 0
fi
grep -E 'swapchain .*generation|fps ->' <<<"$cube" | tail -2 | cut -c1-110 || true
grep -q 'generation on' <<<"$cube" || { echo "no generation:"; tail -5 <<<"$cube"; exit 1; }
ratio="$(grep -o '(x[0-9.]*)' <<<"$cube" | tail -1 | tr -d '(x)' || true)"
awk -v r="${ratio:-0}" 'BEGIN { exit !(r >= 1.9) }' || { echo "output/game ratio x$ratio"; exit 1; }
JOB
status=0
for target in "${targets[@]}"; do
  if [ "$arm64" = 1 ]; then
    cmd=(ubuntu:24.04 bash /work/job.sh "$target")
  else
    cmd=("$target" bash /work/job.sh)
  fi
  ext=tar.gz
  case "$archive" in *.deb) ext=deb ;; *.rpm) ext=rpm ;; esac
  if docker run --rm -v "$archive:/work/pkg.$ext:ro" -v "$job:/work/job.sh:ro" "${cmd[@]}" 2>&1 |
     sed "s|^|[$target] |"; then
    echo "[$target] OK"
  else
    echo "[$target] FAILED"
    status=1
  fi
done
exit $status
