#!/usr/bin/env python3
"""bdex_offline end to end, without a display: a textured pattern translating
at constant speed must be interpolated (x2) and extrapolated (x2) far better
than repeating the nearest input frame. On an irregular "game" — uneven frame
times (--jitter), or every image presented twice (--dup 2) — frame_timing and
dedup must beat the fixed timing, in both modes; and on a menu opening over
part of the screen, scene_cut_local must beat the frame-wide scene-cut test.
Usage: test_offline.py <build dir>"""
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


def evaluate(build, frames_dir, extra_env=None, args=()):
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(frames_dir), "--build", str(build), *args]
    env = dict(os.environ, **(extra_env or {}))
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, env=env).stdout
    print(out)
    lines = out.splitlines()
    col = {name: i for i, name in enumerate(lines[0].split())}
    rows = [line.split() for line in lines[1:]]
    if len(rows) > 1:  # variants: psnr of each
        return [float(r[col["psnr"]]) for r in rows]
    row = rows[0]
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
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        frames(d, n=61, speed=2)
        for mode in ("interpolate", "extrapolate"):
            for offline, option in (("--jitter 0.4", "FRAME_TIMING=1"), ("--dup 2", "DEDUP=1")):
                base, opt = evaluate(build, d, {"BDEX_FG_MODE": mode},
                                     ["--offline-args", offline, "--variant", option])
                if not opt > base + 0.3:
                    print(f"{mode} {offline}: {option} {opt:.2f} dB does not beat the fixed timing {base:.2f} dB")
                    return 1
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(d), "--frames", "41"],
                       check=True, stdout=subprocess.DEVNULL)
        base, local = evaluate(build, d / "popup", {"BDEX_FG_MODE": "interpolate"}, ["--variant", "SCENE_CUT_LOCAL=1"])
        if not local > base + 0.5:
            print(f"popup: scene_cut_local {local:.2f} dB does not beat the frame-wide test {base:.2f} dB")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
