#!/usr/bin/env python3
"""The control panel's config model, without a window: the Game Mode settings
(« sans le panneau ») round-trip through the file, hand-written game settings
survive. Exits 77 (skipped) without PyGObject and GTK 4."""
import importlib.machinery
import importlib.util
import os
import sys
import tempfile

home = tempfile.mkdtemp()
os.environ["HOME"] = home
os.environ["XDG_CONFIG_HOME"] = os.path.join(home, ".config")
try:
    loader = importlib.machinery.SourceFileLoader("panel", sys.argv[1])
    spec = importlib.util.spec_from_loader("panel", loader)
    panel = importlib.util.module_from_spec(spec)
    loader.exec_module(panel)
except (ImportError, ValueError) as e:   # no gi, no GTK 4 typelib
    print(f"skipped: {e}")
    sys.exit(77)

path = os.path.join(home, "test.conf")
with open(path, "w") as f:
    f.write("[*]\nenabled = 1\nrequire_app = 1\nmultiplier = 2\n[mygame]\nmultiplier = 3\n")
c = panel.ConfigFile.load(path)
o = panel.Options()
o.load_from(c.glob)
assert not o.without_panel
o.without_panel = True
o.write_into(c.glob)
assert c.glob["require_app"] == "0" and c.glob["enabled"] == "1", c.glob
c.set_always("mygame", True)
c.set_always("other", True)
assert sorted(c.always_games()) == ["mygame", "other"]
c.write(path)
c = panel.ConfigFile.load(path)
assert c.game_dict("mygame") == {"multiplier": "3", "require_app": "0"}, c.game_dict("mygame")
c.set_always("other", False)
c.set_always("mygame", False)
assert c.always_games() == [] and c.game_dict("other") is None
assert c.game_dict("mygame") == {"multiplier": "3"}   # the hand-written setting stays
o = panel.Options()
o.load_from(c.glob)
assert o.without_panel
print("panel config model OK")

# Refresh rates, as each compositor's tool prints them (text mode, Wayland).
assert panel.rates_hyprctl('[{"refreshRate": 143.912, "disabled": false}, {"refreshRate": 60.0, "disabled": true}]') == [143.912]
assert panel.rates_kscreen('{"outputs": [{"enabled": true, "currentModeId": "2", "modes": ['
                           '{"id": "1", "refreshRate": 60.0}, {"id": "2", "refreshRate": 165.0}]},'
                           '{"enabled": false, "currentModeId": "1", "modes": [{"id": "1", "refreshRate": 240}]}]}') == [165.0]
assert panel.rates_wlr('[{"name": "DP-1", "enabled": true, "modes": [{"refresh": 59.95, "current": false},'
                       '{"refresh": 119.98, "current": true}]}]') == [119.98]
mutter = ("(uint32 7, [(('DP-1', 'MSI', 'G27', '0x1'), [('2560x1440@59.951', 2560, 1440, 59.951, 1.0, [1.0, 2.0], "
          "{'is-preferred': <true>}), ('2560x1440@143.912', 2560, 1440, 143.91200256347656, 1.0, [1.0, 2.0], "
          "{'is-current': <true>})], @a{sv} {})], [], @a{sv} {})")
assert panel.rates_mutter(mutter) == [143.91200256347656], panel.rates_mutter(mutter)
assert panel.rates_xrandr("HDMI-1 connected\n   1920x1080     60.00 + 144.00*\n") == [144.0]
assert panel.rates_hyprctl("not json") == [] and panel.rates_kscreen("") == [] and panel.rates_wlr("{}") == []

# Terminals: $TERMINAL first (with its own way of taking a command), then the list.
t = panel.terminal_commands({"TERMINAL": "/usr/bin/foot"})
assert t[0] == ["/usr/bin/foot"] and ["xdg-terminal-exec"] in t, t[:3]
assert panel.terminal_commands({"TERMINAL": "myterm"})[0] == ["myterm", "-e"]
assert panel.terminal_commands({})[0] == ["xdg-terminal-exec"]
print("panel display / terminal probes OK")
