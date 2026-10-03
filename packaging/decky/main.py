"""Decky Loader backend of the bdex-framegen quick access menu (Steam Deck,
SteamOS Game Mode): reads and writes the layer's configuration
(~/.config/bdex-framegen.conf) the way the control panel does, and reads
the status files the layer publishes (~/.cache/bdex-framegen/run).

Game Mode runs one application at a time, so the panel's « Démarrer » model
(require_app) cannot work there: « Frame generation » here is the panel's
« Game Mode — without the panel » (require_app = 0 in [*]). The layer reads
the file again on its own: pause, multiplier, mode and the FPS counter apply
to the running game within a second.
"""
import os
import re
import time

try:
    import decky  # Decky Loader's module (absent in the tests)
    HOME = decky.DECKY_USER_HOME
    log = decky.logger
except ImportError:  # pragma: no cover - tests
    decky = None
    HOME = os.path.expanduser("~")
    import logging
    log = logging.getLogger("bdex-framegen")


def config_path(home=None):
    home = home or HOME
    return os.environ.get("BDEX_FG_CONFIG") or os.path.join(home, ".config", "bdex-framegen.conf")


def status_dir(home=None):
    return os.path.join(home or HOME, ".cache", "bdex-framegen", "run")


_SECTION = re.compile(r"\s*\[(.*)\]\s*$")
_KEY = re.compile(r"\s*([A-Za-z0-9_]+)\s*=(.*)$")


def _global_lines(lines):
    """Indices of the lines that apply to every game: before any section, or
    in a [*] section (or an empty-named one)."""
    out, glob = [], True
    for i, line in enumerate(lines):
        m = _SECTION.match(line)
        if m:
            glob = m.group(1).strip() in ("*", "")
            continue
        if glob:
            out.append(i)
    return out


def get_global(lines, key):
    """The value the layer reads for `key` (the last global line), or None."""
    value = None
    for i in _global_lines(lines):
        m = _KEY.match(lines[i])
        if m and m.group(1).lower() == key:
            value = m.group(2).strip()
    return value


def set_global(lines, key, value):
    """Sets (value None: removes) `key` for every game, in place: the last
    global line holding it is rewritten, the others dropped; a new key goes at
    the end of the first global block, before any game's section."""
    idx = [i for i in _global_lines(lines) if (m := _KEY.match(lines[i])) and m.group(1).lower() == key]
    if idx:
        keep = idx[-1]
        for i in reversed(idx[:-1]):
            del lines[i]
        keep -= len(idx) - 1
        if value is None:
            del lines[keep]
        else:
            lines[keep] = f"{key} = {value}"
        return
    if value is None:
        return
    # The end of the first global block: the line before the first section
    # that is not [*] (after a leading [*] header, if any).
    end = len(lines)
    for i, line in enumerate(lines):
        m = _SECTION.match(line)
        if m and m.group(1).strip() not in ("*", ""):
            end = i
            break
    while end > 0 and not lines[end - 1].strip():
        end -= 1   # (before the blank lines that separate the next section)
    lines.insert(end, f"{key} = {value}")


def read_lines(path):
    try:
        with open(path, encoding="utf-8") as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def write_lines(path, lines):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


def _flag(v, default=False):
    return default if v is None else v.strip().lower() in ("1", "true", "on", "yes")


def multiplier_ceiling(refresh_hz):
    """As the panel's « Auto »: x4 up to 165 Hz, x6 up to 250 Hz, x8 above."""
    return 4 if refresh_hz <= 165 else 6 if refresh_hz <= 250 else 8


# The keys the menu manages, in a game's section as in the global lines.
MANAGED = ("require_app", "multiplier", "adaptive", "target_fps", "mode", "paused", "overlay", "hud")


def _section_span(lines, name):
    """[start, end) of the last [name] section's body (case-insensitive), or None."""
    span, start = None, None
    for i, line in enumerate(lines):
        m = _SECTION.match(line)
        if m:
            if start is not None:
                span = (start, i)
                start = None
            if m.group(1).strip().lower() == name.lower():
                start = i + 1
    if start is not None:
        span = (start, len(lines))
    return span


def get_section(lines, name, key):
    span = _section_span(lines, name)
    value = None
    for i in range(*span) if span else ():
        m = _KEY.match(lines[i])
        if m and m.group(1).lower() == key:
            value = m.group(2).strip()
    return value


def set_section(lines, name, key, value):
    """Sets (value None: removes) `key` in [name], made at the end if needed."""
    span = _section_span(lines, name)
    if span:
        idx = [i for i in range(*span) if (m := _KEY.match(lines[i])) and m.group(1).lower() == key]
        for i in reversed(idx[1:] if value is not None else idx):
            del lines[i]
        if value is not None:
            if idx:
                lines[idx[0]] = f"{key} = {value}"
            else:
                end = span[1]
                while end > span[0] and not lines[end - 1].strip():
                    end -= 1
                lines.insert(end, f"{key} = {value}")
        return
    if value is not None:
        while lines and not lines[-1].strip():
            lines.pop()
        lines.extend(([""] if lines else []) + [f"[{name}]", f"{key} = {value}"])


def per_game(lines, game):
    """Whether the game's section holds settings of the menu."""
    return bool(game) and any(get_section(lines, game, k) is not None for k in MANAGED)


def state_from(lines, game=None):
    """The menu's controls from the configuration: for `game` (a process
    name, as the layer's [sections] match it), its section over the globals."""
    mine = per_game(lines, game)

    def get(key):
        v = get_section(lines, game, key) if mine else None
        return v if v is not None else get_global(lines, key)

    enabled = _flag(get_global(lines, "enabled"))
    require_app = _flag(get("require_app"), True)
    adaptive = _flag(get("adaptive"))
    target = (get("target_fps") or "").lower()
    try:
        mult = int(float(get("multiplier") or "2"))
    except ValueError:
        mult = 2
    if mult <= 1:
        multiplier = "off"
    elif adaptive:
        multiplier = "adaptive"
    elif target in ("auto", "-1"):
        multiplier = "auto"
    else:
        multiplier = str(min(8, mult))
    mode = (get("mode") or "interpolate").lower()
    mode = "extrapolate" if mode.startswith("extrap") else "hybrid" if mode == "hybrid" else "interpolate"
    hud = get("overlay") or get("hud") or "0"
    return {
        "generate": enabled and not require_app,
        "multiplier": multiplier,
        "mode": mode,
        "paused": _flag(get("paused")),
        "hud": hud.strip() not in ("0", "off", "false", "no", "none", ""),
        "game": game or "",
        "per_game": mine,
    }


def apply_state(lines, s, refresh_hz=60.0, game=None):
    """Writes the menu's controls into the configuration (in place): into the
    game's section when it has the menu's settings (or `per_game` turns them
    on, starting from the effective values), else into the global lines."""
    if game and "per_game" in s:
        if s["per_game"] and not per_game(lines, game):
            current = state_from(lines, game)
            set_section(lines, game, "require_app", "0" if current["generate"] else "1")
            apply_state(lines, {k: current[k] for k in ("multiplier", "mode", "paused", "hud")} | {"per_game": True},
                        refresh_hz, game)
        elif not s["per_game"]:
            for k in MANAGED:
                set_section(lines, game, k, None)
    scoped = bool(game) and per_game(lines, game)

    def put(key, value):
        if scoped:
            set_section(lines, game, key, value)
        else:
            set_global(lines, key, value)

    def get(key):
        v = get_section(lines, game, key) if scoped else None
        return v if v is not None else get_global(lines, key)

    if "generate" in s:
        set_global(lines, "enabled", "1")
        if scoped and get_global(lines, "require_app") is None:
            set_global(lines, "require_app", "1")  # (the other games keep needing the panel)
        put("require_app", "0" if s["generate"] else "1")
    if "multiplier" in s:
        m = s["multiplier"]
        ceiling = multiplier_ceiling(refresh_hz)
        if m == "off":
            put("multiplier", "1")
            put("adaptive", None)
            put("target_fps", None)
        elif m == "adaptive":
            put("multiplier", str(ceiling))
            put("adaptive", "1")
            put("target_fps", None)
        elif m == "auto":
            put("multiplier", str(ceiling))
            put("adaptive", None if not scoped else "0")
            put("target_fps", "auto")
        else:
            put("multiplier", str(max(2, min(8, int(m)))))
            put("adaptive", None if not scoped else "0")
            if (get("target_fps") or "").lower() in ("auto", "-1") or scoped:
                put("target_fps", None if not scoped else "0")
    if "mode" in s and s["mode"] in ("interpolate", "hybrid", "extrapolate"):
        put("mode", s["mode"])
    if "paused" in s:
        put("paused", "1" if s["paused"] else (None if not scoped else "0"))
    if "hud" in s:
        put("hud", None)
        put("overlay", "2" if s["hud"] else (None if not scoped else "0"))
    return lines


def _alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def read_status(directory, now=None):
    """The running game's frame generation, from the newest status file of a
    live process: {"game", "line", "refresh_hz"} or None."""
    now = now or time.time()
    best = None
    try:
        names = os.listdir(directory)
    except OSError:
        return None
    for name in names:
        if not name.endswith(".status"):
            continue
        try:
            with open(os.path.join(directory, name), encoding="utf-8") as f:
                text = f.read()
        except OSError:
            continue
        head, cur, sections = {}, None, []
        for line in text.splitlines():
            if line.startswith("[") and line.endswith("]"):
                cur = {"kind": line[1:-1].split()[0]}
                sections.append(cur)
            elif "=" in line:
                k, v = line.split("=", 1)
                (cur if cur is not None else head)[k] = v
        try:
            pid, written = int(head.get("pid", "0")), int(head.get("time", "0"))
        except ValueError:
            continue
        if pid <= 0 or now - written > 30 or not _alive(pid):
            continue
        sc = next((s for s in sections if s.get("kind") == "swapchain"), None)
        if sc is None:
            continue
        if best is None or written > best[0]:
            best = (written, head.get("process", "?"), sc)
    if best is None:
        return None
    _, game, sc = best
    state = sc.get("state", "")
    try:
        if state in ("generating", "paused"):
            line = "{:.0f} → {:.0f} fps (×{}, {})".format(float(sc.get("game_fps", 0)), float(sc.get("output_fps", 0)),
                                                          sc.get("multiplier", "?"), sc.get("mode", "interpolate"))
            if state == "paused":
                line = "paused — " + line
            lat = float(sc.get("latency_ms", -1))
            if lat >= 0:
                line += f" · {lat:.0f} ms"
        elif state == "pass-through":
            line = "pass-through — " + sc.get("reason", "")
        else:
            line = state or "armed"
        hz = float(sc.get("refresh_hz", 0) or 0)
    except ValueError:
        line, hz = state, 0.0
    return {"game": game, "line": line, "refresh_hz": hz}


def layer_installed(home=None):
    """Whether the Vulkan loader can find the layer's manifest."""
    home = home or HOME
    dirs = [os.path.join(home, ".local", "share", "vulkan", "implicit_layer.d"),
            "/usr/share/vulkan/implicit_layer.d", "/usr/local/share/vulkan/implicit_layer.d",
            "/etc/vulkan/implicit_layer.d"]
    for d in dirs:
        try:
            if any("bdex" in f.lower() for f in os.listdir(d)):
                return True
        except OSError:
            pass
    return False


class Plugin:
    async def get_state(self):
        status = read_status(status_dir())
        s = state_from(read_lines(config_path()), (status or {}).get("game"))
        s["installed"] = layer_installed()
        s["status"] = status
        return s

    async def set_state(self, changes):
        path = config_path()
        lines = read_lines(path)
        status = read_status(status_dir())
        game = (status or {}).get("game")
        apply_state(lines, changes, (status or {}).get("refresh_hz") or 60.0, game)
        write_lines(path, lines)
        log.info("bdex-framegen: %s%s", changes, f" ({game})" if game else "")
        return state_from(lines, game)

    async def _main(self):
        log.info("bdex-framegen menu loaded (config %s)", config_path())

    async def _unload(self):
        pass
