#!/usr/bin/env python3
"""Offline quality evaluation on a recorded frame sequence (any game).

Runs bdex_offline (the frame-generation engine, headless) on a directory of
consecutive PPM frames, keeping every `multiplier`-th frame as input and
scoring the generated frames against the skipped ones. No window is involved,
so the scores do not depend on the compositor, and the same sequence can be
replayed for every variant of an experiment.

Record a sequence from a game through the layer (real frames only):
    BDEX_FG=1 BDEX_FG_MULTIPLIER=1 BDEX_FG_DUMP=/tmp/kenshi BDEX_FG_DUMP_FRAMES=240 %command%
Score it, and A/B an option against the defaults on the same frames:
    tools/eval_sequence.py /tmp/kenshi --variant OCCLUSION_STRENGTH=1

Reported per variant (mean over the generated frames, dB):
  psnr     whole frame
  motion   pixels that change between the two input frames (dilated)
  edges    strong edges of the ground truth inside the motion mask
  dup      whole-frame PSNR of repeating the nearest input (what the player
           sees without frame generation) — the gain over it is `psnr - dup`
  blend    whole-frame PSNR of a plain cross-fade of the two inputs
Absolute dB depend on the scene; compare variants on the same sequence.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def psnr(a, b, mask=None):
    d = a.astype(np.float32) - b.astype(np.float32)
    d = d * d
    if mask is not None:
        if mask.sum() < 64:
            return None
        d = d[mask]
    mse = float(d.mean())
    return 99.0 if mse == 0 else 10.0 * np.log10(255.0 ** 2 / mse)


def luma(img):
    f = img.astype(np.float32)
    return 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]


def dilate(mask, r):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def masks(prev, cur, gt, exclude):
    motion = np.abs(luma(prev) - luma(cur)) > 12.0
    motion = dilate(motion, 4)
    y = luma(gt)
    gx = np.zeros_like(y)
    gy = np.zeros_like(y)
    gx[:, 1:-1] = y[:, 2:] - y[:, :-2]
    gy[1:-1, :] = y[2:, :] - y[:-2, :]
    edges = (np.hypot(gx, gy) > 40.0) & motion
    for x0, y0, x1, y1 in exclude:
        motion[y0:y1, x0:x1] = False
        edges[y0:y1, x0:x1] = False
    return motion, edges


def score(out_dir, exclude):
    rows = []
    for line in (Path(out_dir) / "manifest.txt").read_text().splitlines():
        gen_name, gt_path, t, prev_path, cur_path = line.split()
        t = float(t)
        gen, gt = load(Path(out_dir) / gen_name), load(gt_path)
        prev, cur = load(prev_path), load(cur_path)
        valid = np.ones(gt.shape[:2], bool)
        for x0, y0, x1, y1 in exclude:
            valid[y0:y1, x0:x1] = False
        motion, edges = masks(prev, cur, gt, exclude)
        # Interpolation: t in (0,1) between prev and cur; extrapolation: t in
        # (1,2) after cur, where "repeat" is cur and the blend is meaningless.
        dup = prev if t < 0.5 else cur
        blend = (prev.astype(np.float32) * (1 - t) + cur.astype(np.float32) * t) if t < 1 else None
        rows.append((psnr(gen, gt, valid), psnr(gen, gt, motion), psnr(gen, gt, edges),
                     psnr(dup, gt, valid), psnr(blend, gt, valid) if blend is not None else None))
    return rows


def mean(values):
    v = [x for x in values if x is not None]
    return float(np.mean(v)) if v else float("nan")


def run(binary, frames, out_dir, multiplier, env_extra, max_inputs):
    env = {k: v for k, v in os.environ.items()}
    env.setdefault("BDEX_FG_PRESET", "balanced")  # a fixed operating point, as run_eval.sh
    env.update(env_extra)
    cmd = [str(binary), str(frames), str(out_dir), "--multiplier", str(multiplier)]
    if max_inputs:
        cmd += ["--max-inputs", str(max_inputs)]
    subprocess.run(cmd, check=True, env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", help="directory of consecutive .ppm frames")
    ap.add_argument("--build", default=str(ROOT / "build"), help="build directory (default: ./build)")
    ap.add_argument("--multiplier", type=int, default=2, choices=(2, 3, 4))
    ap.add_argument("--variant", action="append", default=[],
                    help="KEY=VALUE[,KEY=VALUE...] BDEX_FG_* overrides to compare with the defaults (repeatable)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="x0,y0,x1,y1 rectangle left out of every metric (e.g. a frame counter)")
    ap.add_argument("--max-inputs", type=int, default=0, help="limit the number of input frames")
    ap.add_argument("--keep", help="keep the generated frames under this directory")
    args = ap.parse_args()

    binary = Path(args.build) / "tools" / "offline" / "bdex_offline"
    if not binary.exists():
        sys.exit(f"{binary} not found: build the project first")
    exclude = [tuple(int(v) for v in r.split(",")) for r in args.exclude]
    variants = [("default", {})]
    for spec in args.variant:
        env = {}
        for kv in spec.split(","):
            k, _, v = kv.partition("=")
            k = k.strip().upper()
            env[k if k.startswith("BDEX_FG_") else "BDEX_FG_" + k] = v.strip()
        variants.append((spec, env))

    base = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="bdex-seq-"))
    results = []
    for i, (name, env) in enumerate(variants):
        out = base / f"v{i}"
        run(binary, args.frames, out, args.multiplier, env, args.max_inputs)
        rows = score(out, exclude)
        results.append((name, rows))

    print(f"{'variant':<32} {'psnr':>7} {'motion':>7} {'edges':>7} {'dup':>7} {'blend':>7}  frames")
    ref = None
    for name, rows in results:
        m = [mean(c) for c in zip(*rows)]
        delta = "" if ref is None else "  Δ " + " ".join(f"{a - b:+.2f}" for a, b in zip(m[:3], ref[:3]))
        ref = ref or m
        print(f"{name:<32} {m[0]:7.2f} {m[1]:7.2f} {m[2]:7.2f} {m[3]:7.2f} {m[4]:7.2f}  {len(rows)}{delta}")
    if not args.keep:
        import shutil
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    main()
