#!/usr/bin/env bash
# Run by sway (exec in its config: Xwayland's DISPLAY and WAYLAND_DISPLAY
# set): the demo on output 1 through Xwayland, bdex-capture --wayland
# --screen sharing output 1 through the portal, its overlay on output 2.
set -u
out=/work/result
{
  cd /work/src
  /usr/lib/xdg-desktop-portal-wlr -l ${XDPW_LOG:-ERROR} >/work/xdpw.log 2>&1 & sleep 1
  /usr/lib/xdg-desktop-portal -r & sleep 2
  BDEX_FG=0 BDEX_FG_CONFIG=/dev/null timeout 40 build/demo/bdex_demo --fps 30 --size 640x400 --duration 30 >/work/demo.log 2>&1 &
  sleep 3
  BDEX_FG_CONFIG=/dev/null BDEX_FG_LOG=${BDEX_FG_LOG:-2} timeout 30 build/capture/linux/bdex-capture --wayland --screen \
    --rect 640,0,640,400 -m 2 --duration 10 >/work/capture.log 2>&1
  echo "capture exit $?"
} >"$out" 2>&1
swaymsg exit
