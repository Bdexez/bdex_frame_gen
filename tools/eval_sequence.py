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
  ssim     luma SSIM (8x8 box windows) of the generated frames, 1 = identical
  occl     occlusion areas — only for sequences shipping masks in
           <frames>/occlusion/<frame>.png (make_corpus.py's occlude)
  flicker  mean |e_k - e_(k-1)| of the luma error between successive
           generated frames (0..255, lower is better): a stable error such
           as a slight blur does not flicker, an error that jumps from frame
           to frame does — PSNR alone cannot tell them apart
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


def box(img, r):
    """Mean over (2r)x(2r) windows, same size (edges clamped), via an integral image.

    Summed in float64: the integral of squared 8-bit values reaches ~1e11 on
    a 2 Mpx frame, where float32 keeps 4 significant digits and the window
    differences (the local variances of SSIM) drown in rounding noise."""
    p = np.pad(img.astype(np.float64), ((r, r), (r, r)), mode="edge")
    c = np.pad(p.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    h, w = img.shape
    n = 2 * r
    s = c[n:n + h, n:n + w] - c[0:h, n:n + w] - c[n:n + h, 0:w] + c[0:h, 0:w]
    return s / (n * n)


def ssim(a, b, mask):
    x, y = luma(a), luma(b)
    mx, my = box(x, 4), box(y, 4)
    vx = box(x * x, 4) - mx * mx
    vy = box(y * y, 4) - my * my
    cxy = box(x * y, 4) - mx * my
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    m = ((2 * mx * my + c1) * (2 * cxy + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2))
    return float(m[mask].mean())


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
    prev_err = None
    lines = (Path(out_dir) / "manifest.txt").read_text().splitlines()
    # With an irregular game (bdex_offline --jitter/--dup) real frames are
    # scored too, and an exact one would count 99 dB: cap every frame at
    # 60 dB so the mean still reflects the errors.
    cap = 60.0 if any(not line.startswith("gen_") for line in lines) else None
    for line in lines:
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
        err = luma(gen) - luma(gt)
        flicker = float(np.abs(err - prev_err)[valid].mean()) if prev_err is not None else None
        prev_err = err
        row = [psnr(gen, gt, valid), psnr(gen, gt, motion), psnr(gen, gt, edges),
               psnr(dup, gt, valid), psnr(blend, gt, valid) if blend is not None else None]
        if cap is not None:
            row = [None if v is None else min(v, cap) for v in row]
        # Occlusion areas, when the sequence ships their masks
        # (<frames>/occlusion/<frame>.png: make_corpus.py's occlude).
        occl = None
        mask_path = Path(gt_path).parent / "occlusion" / (Path(gt_path).stem + ".png")
        if mask_path.exists():
            occl_mask = (np.asarray(Image.open(mask_path).convert("L")) > 127) & valid
            if occl_mask.any():
                occl = psnr(gen, gt, occl_mask)
                if cap is not None:
                    occl = min(occl, cap)
        rows.append((*row, ssim(gen, gt, valid), flicker, occl))
    return rows


def mean(values):
    v = [x for x in values if x is not None]
    return float(np.mean(v)) if v else float("nan")


def run(binary, frames, out_dir, multiplier, env_extra, max_inputs, extra_args=()):
    env = {k: v for k, v in os.environ.items()}
    env.setdefault("BDEX_FG_PRESET", "balanced")  # a fixed operating point, as run_eval.sh
    env.update(env_extra)
    cmd = [str(binary), str(frames), str(out_dir), "--multiplier", str(multiplier)]
    if max_inputs:
        cmd += ["--max-inputs", str(max_inputs)]
    subprocess.run(cmd + list(extra_args), check=True, env=env)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", help="directory of consecutive .ppm frames")
    ap.add_argument("--build", default=str(ROOT / "build"), help="build directory (default: ./build)")
    ap.add_argument("--multiplier", type=int, default=2, choices=range(2, 9))
    ap.add_argument("--variant", action="append", default=[],
                    help="KEY=VALUE[,KEY=VALUE...] BDEX_FG_* overrides to compare with the defaults (repeatable)")
    ap.add_argument("--exclude", action="append", default=[],
                    help="x0,y0,x1,y1 rectangle left out of every metric (e.g. a frame counter)")
    ap.add_argument("--max-inputs", type=int, default=0, help="limit the number of input frames")
    ap.add_argument("--offline-args", default="",
                    help="extra bdex_offline arguments, e.g. \"--jitter 0.4 --spacing 4\" (an irregular game: "
                         "every displayed frame, real ones included, is then scored)")
    ap.add_argument("--keep", help="keep the generated frames under this directory")
    ap.add_argument("--json", help="also write the per-variant means to this JSON file")
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

    # Generated frames go to disk (~/.cache), not /tmp: /tmp is often a tmpfs.
    cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    cache.mkdir(parents=True, exist_ok=True)
    base = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(prefix="bdex-seq-", dir=cache))
    results = []
    for i, (name, env) in enumerate(variants):
        out = base / f"v{i}"
        run(binary, args.frames, out, args.multiplier, env, args.max_inputs, args.offline_args.split())
        rows = score(out, exclude)
        results.append((name, rows))

    # The occlusion column only for sequences that ship occlusion masks.
    with_occl = any(r[7] is not None for _, rows in results for r in rows)
    print(f"{'variant':<32} {'psnr':>7} {'motion':>7} {'edges':>7} {'dup':>7} {'blend':>7} {'ssim':>7} {'flicker':>7}"
          + (f" {'occl':>7}" if with_occl else "") + "  frames")
    ref = None
    report = []
    for name, rows in results:
        m = [mean(c) for c in zip(*rows)]
        delta = "" if ref is None else ("  Δ " + " ".join(f"{a - b:+.2f}" for a, b in zip(m[:3], ref[:3]))
                                        + f" ssim {m[5] - ref[5]:+.4f} flicker {m[6] - ref[6]:+.2f}"
                                        + (f" occl {m[7] - ref[7]:+.2f}" if with_occl else ""))
        ref = ref or m
        print(f"{name:<32} {m[0]:7.2f} {m[1]:7.2f} {m[2]:7.2f} {m[3]:7.2f} {m[4]:7.2f} {m[5]:7.4f} {m[6]:7.2f}"
              + (f" {m[7]:7.2f}" if with_occl else "") + f"  {len(rows)}{delta}")
        if args.json:
            keys = ("psnr", "motion", "edges", "dup", "blend", "ssim", "flicker") + (("occl",) if with_occl else ())
            report.append({"variant": name, "frames": len(rows), **dict(zip(keys, m))})
    if args.json:
        import json
        Path(args.json).write_text(json.dumps(report, indent=1))
    if not args.keep:
        import shutil
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    main()
