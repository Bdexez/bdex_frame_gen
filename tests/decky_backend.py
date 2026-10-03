#!/usr/bin/env python3
"""The Decky plugin's backend (packaging/decky/main.py) without Decky: its
edits of the configuration match what the layer reads, keep the panel's
lines and the games' sections, and its status reading takes the live game."""
import asyncio
import importlib.util
import os
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("decky_main", os.path.join(ROOT, "packaging", "decky", "main.py"))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


def layer_view(lines, key):
    """What the layer applies for a process with no section of its own: the
    last line for `key` before any section or in a [*] section."""
    value, glob = None, True
    for line in lines:
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            glob = t[1:-1].strip() in ("*", "")
            continue
        if glob and "=" in t and not t.startswith("#"):
            k, v = t.split("=", 1)
            if k.strip().lower() == key:
                value = v.strip()
    return value


# The panel's file: globals under [*], a game section after.
panel = ["# bdex-framegen — written by the control panel (bdex-framegen-gui).", "", "[*]", "enabled = 1",
         "require_app = 1", "multiplier = 4", "target_fps = auto", "mode = interpolate", "", "[kenshi]",
         "multiplier = 3"]
s = m.state_from(panel)
check(s == {"generate": False, "multiplier": "auto", "mode": "interpolate", "paused": False, "hud": False,
            "game": "", "per_game": False}, f"read {s}")
lines = list(panel)
m.apply_state(lines, {"generate": True, "multiplier": "3", "mode": "hybrid", "paused": True, "hud": True})
check(layer_view(lines, "require_app") == "0" and layer_view(lines, "enabled") == "1", "generate")
check(layer_view(lines, "multiplier") == "3" and layer_view(lines, "target_fps") is None, "multiplier x3")
check(layer_view(lines, "mode") == "hybrid" and layer_view(lines, "paused") == "1", "mode, pause")
check(layer_view(lines, "overlay") == "2", "hud")
check(lines[-2:] == ["[kenshi]", "multiplier = 3"], f"the game's section kept: {lines}")
check(m.state_from(lines)["multiplier"] == "3" and m.state_from(lines)["generate"], "read back")
m.apply_state(lines, {"multiplier": "adaptive", "paused": False}, refresh_hz=90)
check(layer_view(lines, "adaptive") == "1" and layer_view(lines, "multiplier") == "4", "adaptive at 90 Hz: x4 ceiling")
check(layer_view(lines, "paused") is None and m.state_from(lines)["multiplier"] == "adaptive", "unpaused, adaptive")
m.apply_state(lines, {"multiplier": "auto"}, refresh_hz=240)
check(layer_view(lines, "multiplier") == "6" and layer_view(lines, "target_fps") == "auto", "auto at 240 Hz")

# Per game: the game's section over the globals, as the layer applies it.
def game_view(lines, game, key):
    """What the layer applies for `game`: its section's last line, else the globals'."""
    v, inside = None, False
    for line in lines:
        t = line.strip()
        if t.startswith("[") and t.endswith("]"):
            inside = t[1:-1].strip().lower() == game
            continue
        if inside and "=" in t:
            k, val = t.split("=", 1)
            if k.strip().lower() == key:
                v = val.strip()
    return v if v is not None else layer_view(lines, key)


g = list(panel)
st = m.state_from(g, "kenshi")
check(st["per_game"] and st["multiplier"] == "auto" and not st["generate"], f"kenshi's section read (the global target_fps = auto applies to it too): {st}")
m.apply_state(g, {"generate": True, "mode": "extrapolate"}, game="kenshi")
check(game_view(g, "kenshi", "require_app") == "0" and layer_view(g, "require_app") == "1", "generate for kenshi only")
check(game_view(g, "kenshi", "mode") == "extrapolate" and layer_view(g, "mode") == "interpolate", "mode for kenshi only")
m.apply_state(g, {"per_game": False}, game="kenshi")
check(not m.per_game(g, "kenshi") and game_view(g, "kenshi", "multiplier") == "4", f"back to the globals: {g}")
g2 = list(panel)
m.apply_state(g2, {"per_game": True}, game="doom")
st = m.state_from(g2, "doom")
check(st["per_game"] and st["multiplier"] == "auto" and g2[:len(panel)] == panel, f"doom's own section from the globals: {g2}")
m.apply_state(g2, {"multiplier": "2", "paused": True}, game="doom")
check(game_view(g2, "doom", "multiplier") == "2" and game_view(g2, "doom", "target_fps") == "0" and
      layer_view(g2, "multiplier") == "4" and layer_view(g2, "paused") is None, f"doom x2, paused: {g2}")
check(m.state_from(g2, "doom")["multiplier"] == "2" and m.state_from(g2, "doom")["paused"], "doom read back")

# Globals before any section, then a [*] section: both are global; the key
# is rewritten once (the last one wins in the layer).
mixed = ["multiplier = 2", "[*]", "multiplier = 3", "[game]", "mode = extrapolate"]
m.set_global(mixed, "multiplier", "4")
check(layer_view(mixed, "multiplier") == "4" and sum(l.startswith("multiplier") for l in mixed) == 1,
      f"duplicates: {mixed}")
check(mixed[-1] == "mode = extrapolate" and layer_view(mixed, "mode") is None, "a game's line is not global")
# A new key never lands in a game's section.
only_game = ["[game]", "mode = extrapolate"]
m.set_global(only_game, "paused", "1")
check(layer_view(only_game, "paused") == "1", f"new key before the first section: {only_game}")
check(m.state_from([])["generate"] is False, "empty file")

# Status: the newest file of a live process with a swapchain.
with tempfile.TemporaryDirectory() as d:
    now = int(time.time())
    with open(os.path.join(d, "1.status"), "w") as f:   # a dead pid
        f.write(f"process=old\npid=999999\ntime={now}\n[swapchain 1]\nstate=generating\n")
    with open(os.path.join(d, f"{os.getpid()}.status"), "w") as f:
        f.write(f"process=game\npid={os.getpid()}\ntime={now}\n[device 1]\nstate=active\n[swapchain 2]\n"
                "state=generating\nmode=hybrid\nmultiplier=3\ngame_fps=40.0\noutput_fps=120.0\nrefresh_hz=120.0\n"
                "latency_ms=21.0\n")
    st = m.read_status(d)
    check(st is not None and st["game"] == "game" and st["refresh_hz"] == 120.0, f"status {st}")
    check(st and st["line"] == "40 → 120 fps (×3, hybrid) · 21 ms", f"status line {st}")
    check(m.read_status(os.path.join(d, "missing")) is None, "no directory")

    # The Plugin's calls, on a private home.
    os.environ["BDEX_FG_CONFIG"] = os.path.join(d, "conf", "bdex-framegen.conf")
    p = m.Plugin()
    r = asyncio.run(p.set_state({"generate": True, "multiplier": "2"}))
    check(r["generate"] and r["multiplier"] == "2", f"set_state {r}")
    g = asyncio.run(p.get_state())
    check(g["generate"] and "installed" in g and "status" in g, f"get_state {g}")

print("decky backend:", "OK" if failures == 0 else f"{failures} failure(s)")
sys.exit(1 if failures else 0)
