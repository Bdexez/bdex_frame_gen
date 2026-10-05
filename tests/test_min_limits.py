#!/usr/bin/env python3
"""The engine within the limits every Vulkan GPU guarantees, headless.

Vulkan guarantees 4 storage images per shader stage and 16 KB of compute
shared memory; small GPUs (Raspberry Pi's V3D, some mobile ones) have just
that. bdex_offline --limit-storage-images 4 --limit-shared 16 makes the engine
see those limits on any GPU, and checks:
  - with the default settings, the frames are byte-identical to the ones the
    engine makes with this GPU's real limits (the lean matcher and every pass
    fit already);
  - in every mode, with every optional pass on, the engine still generates
    its frames (a pass past a limit makes the whole generation fall back to
    passing frames through: FrameGen::checkPassLimits).
Usage: test_min_limits.py <build dir>. Needs a Vulkan device, no display.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIMITS = ["--limit-storage-images", "4", "--limit-shared", "16"]
EVERY_PASS = {
    "BDEX_FG_FLOW_TEMPORAL": "1", "BDEX_FG_SUBPIXEL_LK": "2", "BDEX_FG_HUD_MASK": "24",
    "BDEX_FG_ONLINE_TUNE": "1", "BDEX_FG_DEDUP": "1", "BDEX_FG_FRAME_TIMING": "1",
    "BDEX_FG_SCENE_CUT_LOCAL": "1", "BDEX_FG_CHROMA_WEIGHT": "1", "BDEX_FG_ROT_WEIGHT": "1",
    "BDEX_FG_SYNTH_BICUBIC": "1", "BDEX_FG_STATIC_GUARD": "1", "BDEX_FG_OCCLUSION_STRENGTH": "1",
    "BDEX_FG_EXTRAPOLATE_ACCEL": "1", "BDEX_FG_EXTRAPOLATE_SPLAT": "1", "BDEX_FG_REVEAL_FILL": "1",
    "BDEX_FG_PIXEL_VECTORS": "1", "BDEX_FG_UPSCALE_TEMPORAL": "0.3",
}


def generated(out):
    manifest = out / "manifest.txt"
    return len(manifest.read_text().splitlines()) if manifest.exists() else 0


def main():
    build = Path(sys.argv[1])
    tool = build / "tools" / "offline" / "bdex_offline"
    if not tool.exists():
        print("bdex_offline not built, skipping")
        return 77
    env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET="balanced")
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(tmp / "c")], check=True,
                       stdout=subprocess.DEVNULL)
        seq = tmp / "c" / "hud"
        base = ["--max-inputs", "6"]
        subprocess.run([str(tool), str(seq), str(tmp / "real")] + base, check=True, env=env, stderr=subprocess.DEVNULL)
        subprocess.run([str(tool), str(seq), str(tmp / "min")] + base + LIMITS, check=True, env=env,
                       stderr=subprocess.DEVNULL)
        frames = sorted(p.name for p in (tmp / "real").glob("*.ppm"))
        same = all((tmp / "real" / f).read_bytes() == (tmp / "min" / f).read_bytes() for f in frames)
        print(f"default settings, minimum limits: {len(frames)} frames, {'identical' if same else 'DIFFERENT'}")
        failures += not (frames and same)
        for mode in ("interpolate", "extrapolate", "hybrid"):
            out = tmp / f"every_{mode}"
            r = subprocess.run([str(tool), str(seq), str(out)] + base + LIMITS +
                               ["--multiplier", "3", "--render-scale", "0.75"],
                               env=dict(env, BDEX_FG_MODE=mode, **EVERY_PASS), capture_output=True, text=True)
            n = generated(out)
            print(f"{mode}, every optional pass, minimum limits: {n} generated frame(s)"
                  + ("" if r.returncode == 0 else f" — exit {r.returncode}: {r.stderr.strip().splitlines()[-1:]}"))
            failures += r.returncode != 0 or n == 0
    print("FAIL" if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
