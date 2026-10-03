#!/usr/bin/env bash
# bdex-capture --wayland (screen-cast portal + PipeWire) end to end, in a
# container: sway headless with two outputs, the demo shown on the first,
# shared through xdg-desktop-portal-wlr (which picks the output itself) and
# PipeWire, generated x2 and shown on the second. Checks the capture's
# statistics: the game's frame rate captured, x2 out.
#   tools/ci/wayland-capture-check.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
docker build -q -t bdex-wayland "$ROOT/tools/ci/wayland" >/dev/null
work="$(mktemp -d)"
trap 'docker run --rm -v "$work:/work" bdex-wayland rm -rf /work/src >/dev/null 2>&1; rm -rf "$work"' EXIT
mkdir -p "$work/src"
(cd "$ROOT" && git ls-files -co --exclude-standard | grep -v '^build\|^dist' | tar -cf - -T - | tar -xf - -C "$work/src")
cp "$ROOT/tools/ci/wayland/inside.sh" "$work/"
docker run --rm -e BDEX_FG_LOG="${BDEX_FG_LOG:-2}" -e XDPW_LOG="${XDPW_LOG:-ERROR}" -v "$work:/work" bdex-wayland bash -euo pipefail -c '
cd /work/src
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBDEX_BUILD_TESTS=OFF >/dev/null
cmake --build build >/dev/null
export XDG_RUNTIME_DIR=/tmp/rt HOME=/tmp/home XDG_CURRENT_DESKTOP=sway
mkdir -p $XDG_RUNTIME_DIR $HOME/.config/sway $HOME/.config/xdg-desktop-portal-wlr $HOME/.config/xdg-desktop-portal
chmod 700 $XDG_RUNTIME_DIR
export VK_ICD_FILENAMES="$(ls /usr/share/vulkan/icd.d/lvp_icd*.json | head -1)"
cat > $HOME/.config/sway/config <<CFG
output HEADLESS-1 resolution 640x400 position 0 0
output HEADLESS-2 resolution 640x400 position 640 0
xwayland enable
focus_follows_mouse no
workspace 1 output HEADLESS-1
workspace 2 output HEADLESS-2
focus output HEADLESS-1
for_window [title="bdex-capture overlay"] floating enable
for_window [title="^bdex demo"] move container to output HEADLESS-1, fullscreen enable
exec bash /work/inside.sh
CFG
printf "[screencast]\nchooser_type=none\noutput_name=HEADLESS-1\n" > $HOME/.config/xdg-desktop-portal-wlr/config
printf "[preferred]\ndefault=wlr\n" > $HOME/.config/xdg-desktop-portal/portals.conf
# (sway carries cap_sys_nice, which a container refuses to grant: a plain copy)
install -m 755 /usr/bin/sway /usr/local/bin/sway-plain
dbus-run-session -- bash -c "
  pipewire & sleep 1; wireplumber & sleep 2
  WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 timeout 120 sway-plain -c $HOME/.config/sway/config >/work/sway.log 2>&1
"
' || true
status=0
cat "$work/result" 2>/dev/null || { echo "no result (sway did not run the test)"; tail -20 "$work/sway.log" 2>/dev/null; exit 1; }
grep -E "portal|PipeWire|game [0-9.]+ fps|ERROR|WARN|DEBUG\] PipeWire" "$work/capture.log" | tail -14
[ -n "${BDEX_VERBOSE:-}" ] && { echo "-- demo"; tail -5 "$work/demo.log"; echo "-- sway"; tail -15 "$work/sway.log"; echo "-- xdpw"; grep -v "^$" "$work/xdpw.log" | tail -40; }
ratio=$(grep -E "game [0-9.]+ fps -> output" "$work/capture.log" | tail -1 | sed -n 's/.*(x\([0-9.]*\)).*/\1/p')
game=$(grep -E "game [0-9.]+ fps -> output" "$work/capture.log" | tail -1 | sed -n 's/.*game \([0-9.]*\) fps.*/\1/p')
echo "bdex-capture --wayland: game ${game:-?} fps, x${ratio:-?}"
awk -v r="${ratio:-0}" -v g="${game:-0}" 'BEGIN { exit !(r >= 1.6 && g >= 10) }' || { tail -30 "$work/capture.log"; status=1; }
exit $status
