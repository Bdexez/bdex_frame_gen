#!/usr/bin/env python3
"""Measures the quality of generated frames against ground truth.

Usage: eval_quality.py <gt_dump_dir> <test_dump_dir> [multiplier]
                       [--expected-generated N] [--min-comparable N]

Both directories must come from bdex_demo runs with --deterministic and the
layer's BDEX_FG_DUMP option: the ground truth at multiplier times the frame
rate of the test run. The demo draws its frame counter on
screen; it is decoded here to align the two sequences, so the runs must use
the same window size.

Reports whole-frame PSNR and PSNR in changing regions, moving edges, pixels
revealed by the demo's yellow bar, and the rotating square's moving edges.
The local masks are derived from the reference and the two real frames; they
are evaluation-only.
"""
import glob
import argparse
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
        # Keep the dumps compact; convert to float only for arithmetic.
        fr["img"] = np.asarray(Image.open(fr["path"]), dtype=np.uint8)
    return frames


def psnr(a, b):
    diff = a.astype(np.float32) - b.astype(np.float32)
    mse = np.mean(diff ** 2)
    return 99.0 if mse < 1e-6 else 10 * np.log10(255.0 ** 2 / mse)


def target_counter(previous_counter, t, multiplier):
    """Reference counter for a generated frame between adjacent real frames."""
    fraction = t if t < 1.0 else t - 1.0
    step = round(fraction * multiplier)
    if step <= 0 or step >= multiplier:
        return None
    return multiplier * previous_counter + step


def local_masks(prev, nxt, ref):
    def luma(img):
        return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722
    a, b, r = luma(prev), luma(nxt), luma(ref)
    changing = np.abs(a - b) >= 12.0
    gy, gx = np.gradient(r)
    moving_edges = changing & (np.hypot(gx, gy) >= 8.0)
    # The demo's six-digit frame counter is an alignment marker, not scene
    # motion. Keep it in whole-frame PSNR for historical comparability, but
    # exclude its exact drawn rectangle from the local diagnostics.
    h, w = changing.shape
    if h >= 16 + 5 * SCALE and w >= 16 + 6 * 4 * SCALE:
        counter = np.s_[h - 16 - 5 * SCALE:h - 16, 16:16 + 6 * 4 * SCALE]
        changing[counter] = False
        moving_edges[counter] = False
    return changing, moving_edges


def targeted_masks(prev, nxt, ref, moving_edges):
    """Scene-specific diagnostics for disocclusion and non-translating motion."""
    def yellow_bar(img):
        red, green, blue = np.moveaxis(img, -1, 0)
        # The bar's nominal RGB8 is (242, 217, 51); the rotating square's
        # yellow cells are (242, 229, 89). Allow filtered edge pixels.
        return (red > 175) & (green > 140) & (green < 224) & \
               (blue < 75) & (red > green)

    revealed = yellow_bar(prev) & ~yellow_bar(ref) & ~yellow_bar(nxt)
    h, w = revealed.shape
    yy, xx = np.ogrid[:h, :w]
    revealed &= (yy > 0.24 * h) & (yy < 0.76 * h)
    # The square is centered at vUV=(0.24, 0.32) in the demo shader;
    # image rows run top-down and its 0.1-unit radius is relative to height.
    rotating = moving_edges & (np.abs(xx - 0.24 * w) < 0.15 * h) & \
               (np.abs(yy - 0.68 * h) < 0.15 * h)
    return revealed, rotating


def masked_psnr(a, b, mask):
    if not np.any(mask):
        return float("nan")
    diff = a[mask].astype(np.float32) - b[mask].astype(np.float32)
    mse = np.mean(diff ** 2)
    return 99.0 if mse < 1e-6 else 10 * np.log10(255.0 ** 2 / mse)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gt_dump_dir")
    parser.add_argument("test_dump_dir")
    parser.add_argument("multiplier", nargs="?", type=int, default=2, choices=range(2, 9))
    parser.add_argument("--expected-generated", type=int, default=None,
                        help="generated frames expected from the requested run length")
    parser.add_argument("--min-comparable", type=int, default=None,
                        help="minimum aligned generated frames required for a valid report")
    args = parser.parse_args()
    if args.expected_generated is not None and args.expected_generated < 1:
        parser.error("--expected-generated must be positive")
    if args.min_comparable is not None and args.min_comparable < 1:
        parser.error("--min-comparable must be positive")
    multiplier = args.multiplier
    gt = load_dir(args.gt_dump_dir)
    test = load_dir(args.test_dump_dir)
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
        # Interpolation uses t in (0,1), extrapolation t in (1,2).
        target = target_counter(prev["counter"], g["t"], multiplier)
        if target is None or target not in gtByCounter:
            continue
        ref = gtByCounter[target]
        if ref.shape != g["img"].shape:
            continue
        changing, moving_edges = local_masks(prev["img"], nxt["img"], ref)
        revealed, rotating = targeted_masks(prev["img"], nxt["img"], ref, moving_edges)
        rows.append((prev["counter"], psnr(g["img"], ref), psnr(prev["img"], ref), psnr(nxt["img"], ref),
                     psnr((1 - (target % multiplier) / multiplier) * prev["img"] +
                          (target % multiplier) / multiplier * nxt["img"], ref),
                     masked_psnr(g["img"], ref, changing), masked_psnr(g["img"], ref, moving_edges),
                     masked_psnr(g["img"], ref, revealed), masked_psnr(g["img"], ref, rotating)))
    expected = args.expected_generated if args.expected_generated is not None else len(gens)
    minimum = args.min_comparable if args.min_comparable is not None else max(2, (expected * 4 + 4) // 5)
    print(f"comparison coverage: {len(rows)}/{expected} expected generated frames "
          f"({len(gens)} dumped; minimum {minimum})")
    if len(rows) < minimum:
        print("too few comparable frames (check --deterministic, window size and frame rates)")
        return 1
    print(f"{'frame':>6} {'generated':>10} {'dup prev':>10} {'dup next':>10} {'blend':>10} {'changing':>10} {'edges':>10} {'revealed':>10} {'rotation':>10}")
    for r in rows:
        print(f"{r[0]:6d} {r[1]:10.2f} {r[2]:10.2f} {r[3]:10.2f} {r[4]:10.2f} {r[5]:10.2f} {r[6]:10.2f} {r[7]:10.2f} {r[8]:10.2f}")
    arr = np.array([r[1:] for r in rows])
    m = np.array([np.mean(col[np.isfinite(col)]) if np.any(np.isfinite(col)) else float("nan")
                  for col in arr.T])
    print(f"{'mean':>6} {m[0]:10.2f} {m[1]:10.2f} {m[2]:10.2f} {m[3]:10.2f} {m[4]:10.2f} {m[5]:10.2f} {m[6]:10.2f} {m[7]:10.2f}   ({len(rows)} frames)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
