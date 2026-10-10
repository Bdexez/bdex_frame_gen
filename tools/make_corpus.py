#!/usr/bin/env python3
"""Generate the synthetic evaluation corpus (deterministic PPM sequences).

Each sequence isolates one regime that the demo does not cover well:
  hud       a static, see-through HUD (text box, minimap) over a panning texture
  pan       a multi-octave ("fractal") texture panning 16 px/frame: smooth,
            low-contrast content in fast motion (where an absolute zero-motion
            bias makes the matcher give up)
  subpixel  the same kind of texture panning 3.25 px/frame (sub-pixel motion)
  fade      a slow pan while the brightness fades out by 4 %/frame
  swing     a textured camera swinging back and forth (sinusoidal pan, period
            24 frames): the speed changes all the time, so extrapolating at
            constant velocity overshoots and lags
  chroma    colour blobs of (nearly) equal brightness panning 6 px/frame, with
            a faint luma texture: the edges exist only in chroma, where a
            luma-only matcher sees nothing moving
  occlude   a textured object crossing fast over a slowly panning background,
            with masks of the areas it uncovers / covers (occlusion/)
  popup     the HUD scene's pan with an opaque menu panel (a quarter of the
            screen) opening and closing every 4 frames, between two inputs:
            a scene cut confined to part of the screen
  scroll    lines of thin, high-contrast text scrolling up 5 px/frame over a
            still backdrop (credits, a chat): sharp edges the eye reads, on
            a moving layer that flat areas surround
  particles bright dots falling diagonally at their own speeds over a slow
            pan (rain, sparks): many small objects, each moving differently
            from what is around it
  smoke     a semi-transparent drifting layer over a background panning the
            other way: two motions in the same pixel, which one vector per
            pixel cannot hold
  rotate    a textured disc turning 3 degrees a frame over a still
            backdrop: no translation for a block matcher to find
  flash     the pan with a one-frame flash (brightness x1.7) every 6 frames,
            between two inputs: the same content, brighter
  pulse     the pan while the brightness oscillates by 15 % (period 8
            frames): lighting changes on moving content
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


def swing(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    k = w / 640
    amp = 60 * k
    tex = fractal(rng, h + 8, w + int(2 * amp) + 16, scale(OCT, w / 960))
    for i in range(n):
        x = amp * np.sin(2 * np.pi * i / 24)  # sub-pixel positions: sampled from a 4x-finer shift
        x0 = int(np.floor(x + amp)) + 4
        f = (x + amp) + 4 - x0
        crop = tex[:h, x0:x0 + w] * (1 - f) + tex[:h, x0 + 1:x0 + 1 + w] * f
        Image.fromarray(crop.clip(0, 255).astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def chroma(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    speed = max(1, round(6 * w / 480))
    k = w / 960
    # Hue from a smooth field, quantised into flat blobs with sharp edges.
    hue = fractal(rng, h + 8, w + n * speed + 8, scale([(96, 1.0), (32, 0.5)], k), channels=1)[..., 0] / 255
    hue = np.floor(hue * 6) / 6
    faint = fractal(rng, h + 8, w + n * speed + 8, scale([(10, 1.0), (3, 0.5)], k), channels=1)[..., 0] / 255
    # Isoluminant colours (BT.601 luma ~0.5) around the hue circle.
    ang = 2 * np.pi * hue
    cb, cr = 0.22 * np.cos(ang), 0.22 * np.sin(ang)
    y = 0.5 + 0.015 * (faint - 0.5)
    r = y + 1.402 * cr
    g = y - 0.344136 * cb - 0.714136 * cr
    b = y + 1.772 * cb
    rgb = (np.stack([r, g, b], 2).clip(0, 1) * 255)
    for i in range(n):
        Image.fromarray(rgb[:h, i * speed:i * speed + w].astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def popup(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    speed = max(1, round(9 * w / 640))
    tex = rng.integers(0, 256, (h // 6 + 4, (w + n * speed) // 6 + 4, 3), dtype=np.uint8)
    tex = np.asarray(Image.fromarray(tex).resize((tex.shape[1] * 6, tex.shape[0] * 6), Image.BICUBIC))
    k = w / 640
    for i in range(n):
        im = Image.fromarray(np.ascontiguousarray(tex[:h, i * speed:i * speed + w]))
        if ((i + 1) // 4) % 2 == 1:  # open on frames 3-6, 11-14, ...: always between two x2 inputs
            d = ImageDraw.Draw(im)
            box = [int(0.25 * w), int(0.2 * h), int(0.7 * w), int(0.75 * h)]
            d.rectangle(box, fill=(24, 28, 40), outline=(200, 200, 220), width=3)
            for j, line in enumerate(("PAUSED", "Resume", "Options", "Quit")):
                d.text((box[0] + int(20 * k), box[1] + int((16 + 24 * j) * k)), line, fill=(230, 230, 230))
        im.save(out / f"f{i:04d}.ppm")


def occlude(out, w, h, n, seed):
    """A textured object crossing fast over a slowly panning textured
    background: the areas it uncovers (and covers) are what neither frame
    shows, the regime where the synthesis has to invent content. Writes
    occlusion/fNNNN.png too: the background pixels the object hides in the
    frame before or after (eval_sequence.py scores them apart)."""
    rng = np.random.default_rng(seed)
    k = w / 480
    bg_speed, obj_speed = max(1, round(2 * k)), max(2, round(14 * k))
    bg = fractal(rng, h + 8, w + n * bg_speed + 8, scale(OCT, w / 960))
    size = int(0.3 * h)
    obj = fractal(rng, size, size, scale([(24, 1.0), (8, 0.6), (3, 0.3)], k))
    y0 = (h - size) // 2
    x_at = lambda i: (int(0.05 * w) + i * obj_speed) % (w + size) - size  # wraps around
    (out / "occlusion").mkdir(exist_ok=True)
    def covered(i):
        m = np.zeros((h, w), bool)
        if 0 <= i < n:
            x = x_at(i)
            m[y0:y0 + size, max(0, x):max(0, min(w, x + size))] = True
        return m
    for i in range(n):
        im = bg[:h, i * bg_speed:i * bg_speed + w].copy()
        x = x_at(i)
        a, b = max(0, x), min(w, x + size)
        if b > a:
            im[y0:y0 + size, a:b] = obj[:, a - x:b - x]
        Image.fromarray(im.clip(0, 255).astype(np.uint8)).save(out / f"f{i:04d}.ppm")
        occl = (covered(i - 1) | covered(i + 1)) & ~covered(i)
        Image.fromarray((occl * 255).astype(np.uint8)).save(out / "occlusion" / f"f{i:04d}.png")


def scroll(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    k = w / 480
    speed = max(1, round(5 * k))
    bg = fractal(rng, h, w, scale([(64, 1.0), (16, 0.4)], w / 960)) * 0.35
    line_h = int(14 * k)
    words = ["the", "frame", "light", "tower", "quest", "player", "joined", "said", "hello", "ready", "map", "ok"]
    lines = [" ".join(rng.choice(words, size=int(rng.integers(3, 7)))) for _ in range(n * speed // line_h + h // line_h + 4)]
    sheet = Image.new("L", (w, len(lines) * line_h + h), 0)
    d = ImageDraw.Draw(sheet)
    for j, text in enumerate(lines):
        d.text((int(24 * k), h + j * line_h), text, fill=255)
    sheet = np.asarray(sheet, np.float32)
    for i in range(n):
        txt = sheet[i * speed:i * speed + h][..., None] / 255.0
        im = bg[..., :3] * (1 - txt) + np.array([240, 240, 200], np.float32) * txt
        Image.fromarray(im.clip(0, 255).astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def particles(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    k = w / 480
    bg_speed = max(1, round(2 * k))
    bg = fractal(rng, h + 8, w + n * bg_speed + 8, scale(OCT, w / 960)) * 0.6
    m = int(260 * k * k)
    pos = rng.random((m, 2)) * [w, h]
    vel = np.stack([rng.uniform(2, 6, m) * k, rng.uniform(8, 18, m) * k], 1)
    size = rng.integers(1, 3, m)
    for i in range(n):
        im = bg[:h, i * bg_speed:i * bg_speed + w].copy()
        p = (pos + vel * i) % [w, h]
        for (x, y), r in zip(p.astype(int), size):
            im[max(0, y - r):y + r + 1, max(0, x - r):x + r + 1] = 245
        Image.fromarray(im.clip(0, 255).astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def smoke(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    k = w / 960
    bg_speed, smoke_speed = max(1, round(6 * w / 480)), max(1, round(4 * w / 480))
    bg = fractal(rng, h + 8, w + n * bg_speed + 8, scale(OCT, k))
    alpha = fractal(rng, h + 8, w + n * smoke_speed + 8, scale([(96, 1.0), (32, 0.5)], k), channels=1)[..., 0] / 255
    alpha = np.clip((alpha - 0.3) * 1.6, 0, 0.85)[..., None]
    for i in range(n):
        b = bg[:h, i * bg_speed:i * bg_speed + w]                          # pans left
        x0 = n * smoke_speed - i * smoke_speed                             # the smoke drifts right
        a = alpha[:h, x0:x0 + w]
        im = b * (1 - a) + np.array([200, 200, 205], np.float32) * a
        Image.fromarray(im.clip(0, 255).astype(np.uint8)).save(out / f"f{i:04d}.ppm")


def rotate(out, w, h, n, seed):
    rng = np.random.default_rng(seed)
    bg = Image.fromarray(fractal(rng, h, w, scale([(64, 1.0), (16, 0.4)], w / 960)).astype(np.uint8))
    r = int(0.42 * h)
    disc = Image.fromarray(fractal(rng, 2 * r, 2 * r, scale([(24, 1.0), (8, 0.6), (3, 0.3)], w / 480)).astype(np.uint8))
    mask = Image.new("L", (2 * r, 2 * r), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, 2 * r - 1, 2 * r - 1], fill=255)
    for i in range(n):
        im = bg.copy()
        im.paste(disc.rotate(3.0 * i, resample=Image.BICUBIC), (w // 2 - r, h // 2 - r), mask)
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
                       ("fade", lambda d: pan(d, w, h, n, max(1, round(8 * w / 960)), 17, lambda i: 1.0 - 0.04 * i)),
                       ("popup", lambda d: popup(d, w, h, n, 19)),
                       ("chroma", lambda d: chroma(d, w, h, n, 29)),
                       ("swing", lambda d: swing(d, w, h, n, 23)),
                       ("occlude", lambda d: occlude(d, w, h, n, 31)),
                       ("scroll", lambda d: scroll(d, w, h, n, 37)),
                       ("particles", lambda d: particles(d, w, h, n, 41)),
                       ("smoke", lambda d: smoke(d, w, h, n, 43)),
                       ("rotate", lambda d: rotate(d, w, h, n, 47)),
                       # one flash on frames 3, 9, ...: always between two x2 inputs
                       ("flash", lambda d: pan(d, w, h, n, max(1, round(8 * w / 960)), 53,
                                              lambda i: 1.7 if i % 6 == 3 else 1.0)),
                       ("pulse", lambda d: pan(d, w, h, n, max(1, round(8 * w / 960)), 59,
                                              lambda i: 1.0 + 0.15 * np.sin(2 * np.pi * i / 8)))):
        d = root / name
        d.mkdir(parents=True, exist_ok=True)
        make(d)
    print(f"corpus written to {root} ({w}x{h}, {n} frames per sequence)")


if __name__ == "__main__":
    main()
