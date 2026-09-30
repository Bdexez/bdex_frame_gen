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
