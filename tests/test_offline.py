#!/usr/bin/env python3
"""bdex_offline end to end, without a display: a textured pattern translating
at constant speed must be interpolated (x2) and extrapolated (x2) far better
than repeating the nearest input frame. Usage: test_offline.py <build dir>"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def frames(dir_, n=13, w=320, h=192, speed=3):
    rng = np.random.default_rng(7)
    # Smooth texture (block noise, upsampled) wider than the view, panned.
    tex = rng.integers(0, 256, (h // 8 + 2, (w + n * speed) // 8 + 2, 3), dtype=np.uint8)
    tex = np.asarray(Image.fromarray(tex).resize((tex.shape[1] * 8, tex.shape[0] * 8), Image.BILINEAR))
    for i in range(n):
        Image.fromarray(tex[:h, i * speed:i * speed + w]).save(dir_ / f"f{i:04d}.ppm")


def evaluate(build, frames_dir, extra_env=None):
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(frames_dir), "--build", str(build)]
    env = dict(os.environ, **(extra_env or {}))
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, env=env).stdout
    print(out)
    lines = out.splitlines()
    col = {name: i for i, name in enumerate(lines[0].split())}
    row = lines[1].split()
    return float(row[col["psnr"]]), float(row[col["dup"]]), int(row[col["frames"]])


def main():
    build = Path(sys.argv[1])
    if not (build / "tools" / "offline" / "bdex_offline").exists():
        print("bdex_offline not built, skipping")
        return 77
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        frames(d)
        for mode in ("interpolate", "extrapolate"):
            psnr, dup, n = evaluate(build, d, {"BDEX_FG_MODE": mode})
            if n < 5 or not psnr > dup + 3.0:
                print(f"{mode}: generation {psnr:.2f} dB is not clearly better than repetition {dup:.2f} dB ({n} frames)")
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
