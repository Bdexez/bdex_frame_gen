#!/usr/bin/env python3
"""Configuration keys agree across the project.

The option registry (layer/src/config.cpp) is the source of truth; the
engine lists it with `bdex_offline --list-options`. Every key the GTK panel,
the Windows launcher or the README option table names must be one the
engine understands (a typo there silently does nothing), and every option
the engine understands must be documented in the README table.

Usage: test_option_keys.py BUILD_DIR
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Keys read by other programs than the engine (the capture app's hotkey,
# the environment switch).
EXTERNAL = {"hotkey", "bdex_fg"}


def main():
    binary = Path(sys.argv[1]) / "tools" / "offline" / "bdex_offline"
    run = lambda *a: set(subprocess.run([str(binary), "--list-options", *a], check=True,
                                        capture_output=True, text=True).stdout.split())
    known, canonical = run() | EXTERNAL, run("--canonical")

    gui = (ROOT / "tools" / "bdex-framegen-gui").read_text()
    gui_keys = set(re.findall(r'(?:spin|switch|choice|entry)\("([a-z_0-9]+)"', gui))
    block = gui[gui.index("GLOBAL_KEYS"):gui.index("+ tuple", gui.index("GLOBAL_KEYS"))]
    gui_keys |= set(re.findall(r'"([a-z_0-9]+)"', block))
    launcher = set(re.findall(r'\{"([a-z_0-9]+)", L"', (ROOT / "capture" / "src" / "launcher.cpp").read_text()))
    rows = re.findall(r'^\| ((?:`[A-Z_0-9]+`(?: / )?)+) \|', (ROOT / "README.md").read_text(), re.M)
    readme = {k.lower() for row in rows for k in re.findall(r"`([A-Z_0-9]+)`", row)}

    failed = False
    for name, keys in (("tools/bdex-framegen-gui", gui_keys), ("capture/src/launcher.cpp", launcher),
                       ("README.md option table", readme)):
        unknown = sorted(keys - known)
        if unknown:
            print(f"{name}: keys the engine does not understand: {', '.join(unknown)}")
            failed = True
    undocumented = sorted(canonical - readme)
    if undocumented:
        print(f"README.md option table: undocumented options: {', '.join(undocumented)}")
        failed = True
    if not gui_keys or not launcher or not readme:
        print("no keys found in one of the sources: the patterns need updating")
        failed = True
    print(f"{len(canonical)} options; GUI {len(gui_keys)}, launcher {len(launcher)}, README {len(readme)} keys checked")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
