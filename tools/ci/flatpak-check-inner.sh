#!/usr/bin/env bash
# Inside the container of tools/ci/flatpak-check.sh (the checkout in /src).
set -euo pipefail
ID=org.freedesktop.Platform.VulkanLayer.BdexFramegen
BRANCH=25.08
dnf -y -q install flatpak flatpak-builder git-core appstream python3 >/dev/null
flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak --user install -y --noninteractive flathub \
  "org.freedesktop.Platform//$BRANCH" "org.freedesktop.Sdk//$BRANCH" \
  "org.freedesktop.Sdk.Compat.i386//$BRANCH" "org.freedesktop.Sdk.Extension.toolchain-i386//$BRANCH" \
  "org.freedesktop.Platform.Compat.i386//$BRANCH" "org.freedesktop.Platform.GL.default//$BRANCH" \
  "org.freedesktop.Platform.GL32.default//$BRANCH" >/dev/null

# The manifest, building this checkout instead of the git source.
mkdir -p /b && cp -r /src /b/src && rm -rf /b/src/build* /b/src/dist
cd /b/src/packaging/flatpak
python3 - "$ID.yml" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
s, n = re.subn(r"      - type: git\n        url: [^\n]*\n        branch: [^\n]*\n", "      - type: dir\n        path: ../..\n", s)
assert n == 1, "git source not found in the manifest"
open(sys.argv[1], "w").write(s)
PY
flatpak-builder --user --install --force-clean --disable-rofiles-fuse /b/build "$ID.yml" > /b/build.log 2>&1 ||
  { tail -60 /b/build.log; exit 1; }

base="/root/.local/share/flatpak/runtime/$ID/x86_64/$BRANCH/active/files"
echo "== installed:"; (cd "$base" && find . -type f ! -name .ref | sort)
for so in lib/x86_64-linux-gnu lib/i386-linux-gnu; do
  [ -f "$base/$so/libVkLayer_bdex_framegen.so" ] || { echo "FAIL: $so missing"; exit 1; }
done
grep -h library_path "$base"/share/vulkan/implicit_layer.d/*.json

# The runtime's loader, as an app on the Platform sees the extension (merged
# into /usr/lib/extensions/vulkan): the layer among the instance layers. The
# SDK has no vulkaninfo: a probe built inside it, the 32-bit one with the
# i386 toolchain against the Compat.i386 loader.
cp /src/tools/ci/vk_layer_probe.c /b/
# (--allow=multiarch: 32-bit syscalls, as Steam's Flatpak has them; seccomp kills
# the 32-bit probe otherwise)
sdk() { flatpak run --devel --allow=multiarch --filesystem=/b --command=sh "org.freedesktop.Sdk//$BRANCH" -c "$1" 2>&1 || true; }
out64="$(sdk 'cc /b/vk_layer_probe.c -o /b/probe64 -lvulkan && VK_LOADER_DEBUG=layer /b/probe64')"
out32="$(sdk 'PATH=$PATH:/usr/lib/sdk/toolchain-i386/bin; i686-unknown-linux-gnu-gcc /b/vk_layer_probe.c -o /b/probe32 \
  -L/usr/lib/i386-linux-gnu -l:libvulkan.so.1 && file /b/probe32 && VK_LOADER_DEBUG=layer /b/probe32')"
# 64-bit: listed, loaded, instance created (lavapipe from GL.default).
if ! grep -qx "layer VK_LAYER_BDEX_framegen" <<<"$out64" || ! grep -q "vkCreateInstance 0" <<<"$out64" ||
   ! grep -q "Loading layer library .*/BdexFramegen/lib/x86_64-linux-gnu/" <<<"$out64"; then
  echo "FAIL (64-bit): layer not loaded"; tail -40 <<<"$out64"; exit 1
fi
echo "64-bit: $(grep -m1 "Loading layer library .*bdex" <<<"$out64" | sed 's/^.*LAYER: *//')"
# 32-bit: the 32-bit manifest only (library_arch keeps the 64-bit one out).
# The SDK mounts no 32-bit driver (GL32 is for apps that declare it, like
# Steam): no instance here.
if ! grep -qx "layer VK_LAYER_BDEX_framegen_32" <<<"$out32" || grep -qx "layer VK_LAYER_BDEX_framegen" <<<"$out32"; then
  echo "FAIL (32-bit): wrong layers"; tail -40 <<<"$out32"; exit 1
fi
echo "32-bit: VK_LAYER_BDEX_framegen_32 listed by the i386 loader"
echo "flatpak extension OK"
