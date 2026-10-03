#!/usr/bin/env bash
# Builds and installs the layer for the current user (~/.local by default).
#   tools/install.sh [prefix]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$HOME/.local}"
BUILD="$ROOT/build-install"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBDEX_BUILD_TESTS=OFF >/dev/null
cmake --build "$BUILD" -j"$(nproc)"
# A new inode for the libraries: a running game keeps the one it mapped (an
# install over it in place would truncate that mapping: SIGBUS).
rm -f "$PREFIX/lib/libVkLayer_bdex_framegen.so" "$PREFIX/lib32/libVkLayer_bdex_framegen.so"
cmake --install "$BUILD"
echo
echo "Installed:"
echo "  $PREFIX/lib/libVkLayer_bdex_framegen.so"
echo "  $PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen.json"
echo "  $PREFIX/bin/bdex-framegen"
echo "  $PREFIX/bin/bdex-framegen-gui  (graphical launcher)"
[ -x "$PREFIX/bin/bdex-capture" ] && echo "  $PREFIX/bin/bdex-capture  (frame generation by capture: anti-cheat games, a second GPU)"
[ -x "$PREFIX/bin/bdex-calibrate" ] && echo "  $PREFIX/bin/bdex-calibrate  (what generation costs on this GPU: bdex-framegen --calibrate)"

# 32-bit layer for 32-bit games, when the toolchain allows it: a 32-bit C++
# shared library with threads links (32-bit glibc + libstdc++ development
# files: gcc-multilib g++-multilib on Debian/Ubuntu, glibc-devel.i686
# libstdc++-devel.i686 libatomic.i686 on Fedora, lib32-gcc-libs on Arch). The layer does not
# link the Vulkan loader, so its 32-bit copy (wherever the distribution puts
# it: /usr/lib32, /usr/lib/i386-linux-gnu, /usr/lib) is not needed to build.
have_m32() {
  local t; t="$(mktemp -d)"
  printf '#include <thread>\n#include <string>\nextern "C" int f(){std::thread t([]{});t.join();return (int)std::string("x").size();}\n' > "$t/t.cpp"
  "${CXX:-c++}" -m32 -shared -fPIC -pthread "$t/t.cpp" -o "$t/t.so" >/dev/null 2>&1
  local rc=$?; rm -rf "$t"; return $rc
}
if [ "${BDEX_NO_32BIT:-0}" != 1 ] && have_m32; then
  BUILD32="$ROOT/build-install32"
  cmake -S "$ROOT" -B "$BUILD32" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBDEX_32BIT=ON >/dev/null
  cmake --build "$BUILD32" -j"$(nproc)"
  cmake --install "$BUILD32"
  echo "  $PREFIX/lib32/libVkLayer_bdex_framegen.so (32-bit)"
  echo "  $PREFIX/share/vulkan/implicit_layer.d/VK_LAYER_BDEX_framegen_32.json"
else
  echo "  (no 32-bit toolchain: 32-bit games will run without the layer;"
  echo "   install gcc-multilib g++-multilib, glibc-devel.i686 libstdc++-devel.i686 libatomic.i686, or lib32-gcc-libs)"
fi
# Steam, Heroic, Lutris or Bottles installed as a Flatpak: a copy in their
# sandbox.
bash "$ROOT/packaging/linux/flatpak.sh" "$PREFIX"
echo
echo "The layer now loads into every Vulkan game automatically, inert until you"
echo "turn it on. Open the control panel to enable it (globally or per game):"
echo "    bdex-framegen-gui"
echo "No Steam launch options needed. (Per-game env still works: BDEX_FG=1 %command%.)"
