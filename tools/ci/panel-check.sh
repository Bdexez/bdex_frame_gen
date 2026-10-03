#!/usr/bin/env bash
# Starts the control panel in a container, on a virtual display (Xvfb) inside
# it — nothing opens on the host's screen — with libadwaita and without it,
# as on SteamOS (3.9 ships GTK 4 and PyGObject, no libadwaita), and checks
# that it comes up and stays up without a Python error. Then its text mode
# (the panel without GTK 4: Ubuntu 20.04, Debian 11), driven through a
# pseudo-terminal, and its config model through the Game Mode settings
# (tests/panel_logic.py).
#   tools/ci/panel-check.sh <options_ui.json> [image...]   (default: archlinux:latest)
# The JSON comes from a build (<build>/options_ui.json). Needs Docker.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
json="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
shift
images=("$@")
[ ${#images[@]} -gt 0 ] || images=(archlinux:latest)
job="$(mktemp)"
trap 'rm -f "$job"' EXIT
cat > "$job" <<'JOB'
set -euo pipefail
if command -v pacman >/dev/null; then
  pacman -Syu --noconfirm --needed python-gobject gtk4 libadwaita xorg-server-xvfb >/dev/null 2>&1
elif command -v apt-get >/dev/null; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  # No GTK 4 before Ubuntu 22.04 / Debian 12: the text mode alone there.
  apt-get install -y -qq --no-install-recommends python3-gi gir1.2-gtk-4.0 gir1.2-adw-1 xvfb bsdutils \
    >/dev/null 2>&1 || apt-get install -y -qq --no-install-recommends python3 bsdutils >/dev/null 2>&1
elif command -v dnf >/dev/null; then
  dnf install -y -q python3-gobject gtk4 libadwaita xorg-x11-server-Xvfb util-linux-script >/dev/null 2>&1
fi
. /etc/os-release
gtk="$(python3 -c 'import gi; gi.require_version("Gtk", "4.0"); from gi.repository import Gtk; print(f"GTK {Gtk.get_major_version()}.{Gtk.get_minor_version()}.{Gtk.get_micro_version()}, PyGObject {gi.__version__}")' 2>/dev/null || true)"
echo "== $PRETTY_NAME: ${gtk:-no GTK 4}"
export HOME=/tmp/home XDG_CONFIG_HOME=/tmp/home/.config BDEX_FG_OPTIONS_UI=/work/options_ui.json
mkdir -p "$XDG_CONFIG_HOME"
printf '[*]\nenabled = 1\nrequire_app = 0\n[mygame]\nrequire_app = 0\n' > "$XDG_CONFIG_HOME/bdex-framegen.conf"
status=0
variants=(libadwaita plain)
if [ -n "$gtk" ]; then
  Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
  sleep 2
else
  variants=()
fi
for variant in "${variants[@]}"; do
  noadw=""
  [ "$variant" = plain ] && noadw=1
  set +e
  DISPLAY=:99 GDK_BACKEND=x11 GSK_RENDERER=cairo BDEX_FG_GUI_NO_ADW="$noadw" \
    timeout 10 python3 /work/bdex-framegen-gui >/tmp/panel.log 2>&1
  rc=$?
  set -e
  if [ $rc -eq 124 ] && ! grep -q Traceback /tmp/panel.log; then
    echo "panel ($variant GTK 4): up for 10 s, no error"
  else
    echo "panel ($variant GTK 4): exit $rc"; tail -20 /tmp/panel.log; status=1
  fi
done
# Text mode: x3 chosen in the multiplier menu reaches the config file, and
# the session is gone once it quits.
export BDEX_FG_STATUS_DIR=/tmp/run
textenv=(BDEX_FG_GUI_TEXT=1)
[ -n "$gtk" ] || textenv=()
# In French (BDEX_LANG=fr), then in English (the containers' C locale).
for lang in fr en; do
  rm -f "$XDG_CONFIG_HOME/bdex-framegen.conf"
  want='mode texte'; [ $lang = en ] && want='text-mode panel'
  printf 'm\n5\nq\n' | env BDEX_LANG=$lang ${textenv[@]+"${textenv[@]}"} script -qec "python3 /work/bdex-framegen-gui" /dev/null >/tmp/text.log 2>&1 || true
  if grep -q '^multiplier = 3' "$XDG_CONFIG_HOME/bdex-framegen.conf" && grep -q "$want" /tmp/text.log &&
     ! grep -q Traceback /tmp/text.log && [ ! -e /tmp/run/app.session ]; then
    echo "panel (text mode, $lang): options saved, session closed"
  else
    echo "panel (text mode, $lang): failed"; tail -20 /tmp/text.log; status=1
  fi
done
printf '[*]\nenabled = 1\nrequire_app = 0\n[mygame]\nrequire_app = 0\n' > "$XDG_CONFIG_HOME/bdex-framegen.conf"
python3 /work/panel_logic.py /work/bdex-framegen-gui || status=1
exit $status
JOB
status=0
for img in "${images[@]}"; do
  if docker run --rm -v "$job:/work/job.sh:ro" -v "$ROOT/tools/bdex-framegen-gui:/work/bdex-framegen-gui:ro" \
       -v "$ROOT/tests/panel_logic.py:/work/panel_logic.py:ro" -v "$json:/work/options_ui.json:ro" \
       "$img" bash /work/job.sh 2>&1 | sed "s|^|[$img] |"; then
    echo "[$img] OK"
  else
    echo "[$img] FAILED"
    status=1
  fi
done
exit $status
