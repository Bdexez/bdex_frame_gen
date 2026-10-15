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

# The Flatpak extension, for the app's Freedesktop runtime branch only.
root = os.path.join(home, "flatpak")
arch = os.uname().machine
for app, rt in (("a.Steam", "org.freedesktop.Platform/x86_64/25.08"), ("a.Old", "org.freedesktop.Platform/x86_64/24.08"),
                ("a.Gnome", "org.gnome.Platform/x86_64/49")):
    os.makedirs(os.path.join(root, "app", app, "current", "active"))
    with open(os.path.join(root, "app", app, "current", "active", "metadata"), "w") as f:
        f.write(f"[Application]\nname={app}\nruntime={rt}\n")
os.makedirs(os.path.join(root, "runtime", panel.FLATPAK_EXTENSION, arch, "25.08"))
assert panel.flatpak_extension_branch("a.Steam", (root,)) == "25.08"
assert panel.flatpak_extension_branch("a.Old", (root,)) is None
assert panel.flatpak_extension_branch("a.Gnome", (root,)) is None
assert panel.flatpak_extension_branch("a.Missing", (root,)) is None
print("panel Flatpak extension detection OK")

# Injection where the layer has the game, capture otherwise (or on another GPU).
opt = panel.Options()
armed = [{"kind": "device", "state": "active", "device": "NVIDIA GeForce RTX 3060 Ti"},
         {"kind": "swapchain", "state": "ready"}]
assert panel.plan(armed, opt)[0] == "layer"
opt.gpu = "Radeon"
assert panel.plan(armed, opt)[0] == "capture"          # another GPU than the game's
opt.gpu = "3060"
assert panel.plan(armed, opt)[0] == "layer"            # the game's own GPU
assert panel.plan([{"kind": "device", "state": "anti-cheat"}], panel.Options())[0] == "capture"
assert panel.plan([], panel.Options())[0] == "capture"  # no swapchain of the layer
assert panel.plan([], panel.Options(), capture_available=False)[0] == "layer"
opt = panel.Options()
opt.method = "capture"
assert panel.plan(armed, opt)[0] == "capture"
opt.method = "layer"
assert panel.plan([{"kind": "device", "state": "anti-cheat"}], opt)[0] == "layer"
# The choice round-trips through the configuration's global lines.
glob = {}
opt = panel.Options()
opt.gpu, opt.method = "Radeon RX 6600", "capture"
opt.write_into(glob)
back = panel.Options()
back.load_from(glob)
assert (back.gpu, back.method) == ("Radeon RX 6600", "capture"), glob
default = {}
panel.Options().write_into(default)
assert "gpu" not in default and "method" not in default
print("panel injection / capture choice OK")

# Anti-cheat prevention (03/10): the layer's report first, the warning wording by method.
assert panel.game_anticheat(0, [{"kind": "device", "state": "active", "anticheat": "easyanticheat"}]) == "easyanticheat"
assert panel.game_anticheat(0, [{"kind": "device", "state": "anti-cheat"}]) in ("anti-triche", "anti-cheat")
assert panel.game_anticheat(999999999, []) == ""
assert "injectée" in panel.anticheat_warning("rl", "EAC", "layer") or "injected" in panel.anticheat_warning("rl", "EAC", "layer")
assert "Capture" in panel.anticheat_warning("g", "EAC", "capture") or "captured" in panel.anticheat_warning("g", "EAC", "capture")
print("anticheat warning ok")
