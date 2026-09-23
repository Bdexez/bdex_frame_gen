#!/usr/bin/env python3
"""Generate the synthetic evaluation corpus (deterministic PPM sequences).

Each sequence isolates one regime that the demo does not cover well:
  hud       a static, see-through HUD (text box, minimap) over a panning texture
  pan       a multi-octave ("fractal") texture panning 16 px/frame: smooth,
            low-contrast content in fast motion (where an absolute zero-motion
            bias makes the matcher give up)
  subpixel  the same kind of texture panning 3.25 px/frame (sub-pixel motion)
  fade      a slow pan while the brightness fades out by 4 %/frame
Usage: make_corpus.py <out_dir> [--width W --height H --frames N]
The default size is small enough for CI on a software Vulkan driver.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def fractal(rng, h, w, octaves, channels=3):
    acc = np.zeros((h, w, channels), np.float32)
    for size, amp in octaves:
        g = rng.random((h // size + 3, w // size + 3, channels)).astype(np.float32)
        im = np.stack([np.asarray(Image.fromarray(g[..., c]).resize((g.shape[1] * size, g.shape[0] * size),
                                                                    Image.BICUBIC)) for c in range(channels)], 2)
        acc += amp * im[:h, :w]
    return (acc - acc.min()) / (acc.max() - acc.min()) * 255


def scale(octaves, k):
    return [(max(2, int(s * k)), a) for s, a in octaves]


OCT = [(256, 1.0), (96, 0.6), (32, 0.4), (10, 0.25), (3, 0.12)]


def pan(out, w, h, n, speed, seed, gain=lambda i: 1.0):
    rng = np.random.default_rng(seed)
    k = w / 960
    tex = fractal(rng, h + 8, w + n * speed + 8, scale(OCT, k))
    for i in range(n):
        Image.fromarray(np.clip(tex[:h, i * speed:i * speed + w] * gain(i), 0, 255).astype(np.uint8)).save(
            out / f"f{i:04d}.ppm")


def subpixel(out, w, h, n, seed):
    # Rendered 4x oversampled and box-filtered: 13 / 4 = 3.25 px/frame.
    rng = np.random.default_rng(seed)
    o, step = 4, 13
    k = w / 960
    tex = fractal(rng, h * o, w * o + n * step + 8, scale([(s * o, a) for s, a in OCT], k), channels=1)[..., 0]
    col = np.stack([tex, tex * 0.8 + 30, 255 - tex * 0.7], 2).clip(0, 255)
    for i in range(n):
        crop = col[:, i * step:i * step + w * o]
        Image.fromarray(crop.reshape(h, o, w, o, 3).mean((1, 3)).astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def hud(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    speed = max(1, round(9 * w / 640))
    tex = rng.integers(0, 256, (h // 6 + 4, (w + n * speed) // 6 + 4, 3), dtype=np.uint8)
    tex = np.asarray(Image.fromarray(tex).resize((tex.shape[1] * 6, tex.shape[0] * 6), Image.BICUBIC))
    k = w / 640
    for i in range(n):
        im = Image.fromarray(np.ascontiguousarray(tex[:h, i * speed:i * speed + w]))
        d = ImageDraw.Draw(im)
        d.rectangle([int(20 * k), int(20 * k), int(260 * k), int(70 * k)], outline=(255, 255, 255), width=2)
        d.text((int(32 * k), int(30 * k)), "HP 100  AMMO 30/120", fill=(255, 255, 0))
        d.text((int(32 * k), int(48 * k)), "Quest: reach the tower", fill=(255, 255, 255))
        d.ellipse([w - int(80 * k), h - int(80 * k), w - int(20 * k), h - int(20 * k)], outline=(0, 255, 0), width=3)
        im.save(out / f"f{i:04d}.ppm")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--height", type=int, default=272)
    ap.add_argument("--frames", type=int, default=13)
    a = ap.parse_args()
    root = Path(a.out)
    w, h, n = a.width, a.height, a.frames
    for name, make in (("hud", lambda d: hud(d, w, h, n, 3)),
                       ("pan", lambda d: pan(d, w, h, n, max(2, round(16 * w / 960)), 11)),
                       ("subpixel", lambda d: subpixel(d, w, h, n, 13)),
                       ("fade", lambda d: pan(d, w, h, n, max(1, round(8 * w / 960)), 17, lambda i: 1.0 - 0.04 * i))):
        d = root / name
        d.mkdir(parents=True, exist_ok=True)
        make(d)
    print(f"corpus written to {root} ({w}x{h}, {n} frames per sequence)")


if __name__ == "__main__":
    main()
