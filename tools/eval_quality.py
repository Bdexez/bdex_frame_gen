#!/usr/bin/env python3
"""Measures the quality of generated frames against ground truth.

Usage: eval_quality.py <gt_dump_dir> <test_dump_dir>

Both directories must come from bdex_demo runs with --deterministic and the
layer's BDEX_FG_DUMP option: the ground truth at twice the frame rate of the
test run (e.g. --fps 60 vs --fps 30). The demo draws its frame counter on
screen; it is decoded here to align the two sequences, so the runs must use
the same window size.

Reports PSNR (dB) of the generated frames versus ground truth, and for
reference the PSNR of two trivial strategies: duplicating the previous frame
and blending the two neighbouring frames.
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

FONT = [0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF]
SCALE = 6


def digit_bits(d):
    return [[(FONT[d] >> (3 * (4 - r) + (2 - c))) & 1 for c in range(3)] for r in range(5)]


def decode_counter(img):
    """Reads the 6-digit frame counter drawn by the demo (bottom-left)."""
    h = img.shape[0]
    value = 0
    for d in range(6):
        best, bestErr = None, 1e9
        pattern = [[1 if img[h - 16 - SCALE * r - 3, 16 + d * 4 * SCALE + c * SCALE + 3].mean() > 160 else 0
                    for c in range(3)] for r in range(5)]
        for k in range(10):
            err = sum(abs(a - b) for ra, rb in zip(pattern, digit_bits(k)) for a, b in zip(ra, rb))
            if err < bestErr:
                best, bestErr = k, err
        value = value * 10 + best
    return value


def load_dir(path):
    frames = []
    for f in sorted(glob.glob(os.path.join(path, "*.ppm"))):
        m = re.search(r"sc(\d+)_(\d+)x(\d+)_(\d+)_(gen|real)_t([\d.]+)\.ppm$", f)
        if not m:
            continue
        frames.append({"path": f, "sc": int(m.group(1)), "size": (int(m.group(2)), int(m.group(3))),
                       "idx": int(m.group(4)), "gen": m.group(5) == "gen", "t": float(m.group(6))})
    # keep the last swapchain only (window managers resize on map)
    if not frames:
        return []
    last = max(fr["sc"] for fr in frames)
    frames = [fr for fr in frames if fr["sc"] == last]
    for fr in frames:
        fr["img"] = np.asarray(Image.open(fr["path"]), dtype=np.float32)
    return frames


def psnr(a, b):
    mse = np.mean((a - b) ** 2)
    return 99.0 if mse < 1e-6 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    gt = load_dir(sys.argv[1])
    test = load_dir(sys.argv[2])
    gtByCounter = {}
    for fr in gt:
        if not fr["gen"]:
            gtByCounter[decode_counter(fr["img"])] = fr["img"]
    if not gtByCounter:
        print("no ground truth frames")
        return 1
    reals = [fr for fr in test if not fr["gen"]]
    for fr in reals:
        fr["counter"] = decode_counter(fr["img"])
    gens = [fr for fr in test if fr["gen"]]
    rows = []
    for g in gens:
        prev = max((r for r in reals if r["idx"] < g["idx"]), key=lambda r: r["idx"], default=None)
        nxt = min((r for r in reals if r["idx"] > g["idx"]), key=lambda r: r["idx"], default=None)
        if prev is None or nxt is None or nxt["counter"] != prev["counter"] + 1:
            continue
        # generated frame sits between real frames prev (time c) and nxt (time c+1), at fraction t
        target = 2 * prev["counter"] + 1 if abs(g["t"] - 0.5) < 1e-3 else None
        if target is None or target not in gtByCounter:
            continue
        ref = gtByCounter[target]
        if ref.shape != g["img"].shape:
            continue
        rows.append((prev["counter"], psnr(g["img"], ref), psnr(prev["img"], ref), psnr(nxt["img"], ref),
                     psnr(0.5 * (prev["img"] + nxt["img"]), ref)))
    if not rows:
        print("no comparable frames (check --deterministic, window size and frame rates)")
        return 1
    print(f"{'frame':>6} {'generated':>10} {'dup prev':>10} {'dup next':>10} {'blend':>10}")
    for r in rows:
        print(f"{r[0]:6d} {r[1]:10.2f} {r[2]:10.2f} {r[3]:10.2f} {r[4]:10.2f}")
    arr = np.array([r[1:] for r in rows])
    m = arr.mean(axis=0)
    print(f"{'mean':>6} {m[0]:10.2f} {m[1]:10.2f} {m[2]:10.2f} {m[3]:10.2f}   ({len(rows)} frames)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
