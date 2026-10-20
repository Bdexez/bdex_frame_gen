#!/usr/bin/env python3
"""A 30 fps game seen at 120 Hz, for x4: every 4th frame is what the game
presents, the three between are the exact pictures of the moments the
generated frames show (score with eval_sequence.py --multiplier 4).

    make_game.py DIR turn|walk|dark|strafe [N=97]

1280x720: a textured world scrolled by the camera, a near layer moving
faster (parallax), a character moving on its own, a static HUD (a
see-through panel, glyph-like text, a rotating minimap frame left out — the
HUD stays still), and the game's motion blur: each picture integrates the
motion over half a 30 fps frame (8 sub-steps), as a 30 fps game blurs.
`turn` is a fast camera turn (~120 px a game frame), `walk` slow, `dark` the
turn at a quarter of the brightness (low contrast), `strafe` a sideways move
where the near layer moves 2.5x the background."""
import sys
from pathlib import Path

import numpy as np

W, H = 1280, 720


def noise(h, w, rng, octaves=5, base=8):
    out = np.zeros((h, w), np.float32)
    amp = 1.0
    for o in range(octaves):
        gh, gw = max(2, h // (base << (octaves - 1 - o)) + 2), max(2, w // (base << (octaves - 1 - o)) + 2)
        g = rng.random((gh, gw)).astype(np.float32)
        ys = np.linspace(0, gh - 1.001, h); xs = np.linspace(0, gw - 1.001, w)
        y0 = ys.astype(int); x0 = xs.astype(int); fy = (ys - y0)[:, None]; fx = (xs - x0)[None, :]
        a = g[y0][:, x0]; b = g[y0][:, x0 + 1]; c = g[y0 + 1][:, x0]; d = g[y0 + 1][:, x0 + 1]
        out += amp * ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy)
        amp *= 0.55
    out -= out.min(); out /= out.max()
    return out


def texture(h, w, rng, tint, rects=40, lights=0):
    n = noise(h, w, rng)
    img = np.stack([n * tint[0], n * tint[1], n * tint[2]], -1)
    for _ in range(rects):  # props, windows: edges to match on
        y, x = rng.integers(0, h), rng.integers(0, w)
        rh, rw = rng.integers(3, max(4, min(60, h // 4))), rng.integers(3, max(4, min(60, w // 4)))
        img[y:y + rh, x:x + rw] = rng.random(3) * 0.8 + 0.1
    for _ in range(lights):  # small bright lights (neon, lamps)
        y, x = rng.integers(0, h - 6), rng.integers(0, w - 30)
        img[y:y + rng.integers(2, 6), x:x + rng.integers(8, 30)] = np.clip(rng.random(3) + 0.6, 0, 1)
    return np.clip(img, 0, 1)


def shift(img, x, y):
    """img sampled at (u + x, v + y), bilinear, edges clamped (the big textures have margins)."""
    h, w = img.shape[:2]
    xi, yi = int(np.floor(x)), int(np.floor(y)); fx, fy = x - xi, y - yi
    a = img[yi:yi + H, xi:xi + W]; b = img[yi:yi + H, xi + 1:xi + 1 + W]
    c = img[yi + 1:yi + 1 + H, xi:xi + W]; d = img[yi + 1:yi + 1 + H, xi + 1:xi + 1 + W]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def main():
    out = Path(sys.argv[1]); kind = sys.argv[2]; n = int(sys.argv[3]) if len(sys.argv) > 3 else 97
    rng = np.random.default_rng({"turn": 11, "walk": 12, "dark": 13, "strafe": 14}[kind])
    out.mkdir(parents=True, exist_ok=True)
    # Camera speed in px per 120 Hz frame (x4 on a 30 fps game: 4 of them per game frame).
    cam = {"turn": (30.0, 3.0), "walk": (6.0, 1.5), "dark": (30.0, 3.0), "strafe": (12.0, 0.0)}[kind]
    near_k = 2.5 if kind == "strafe" else 1.6  # the near layer's speed relative to the background
    span = int(abs(cam[0]) * (n + 2) * near_k) + W + 400
    tall = int(abs(cam[1]) * (n + 2) * near_k) + H + 400
    far = texture(tall, span, rng, (0.7, 0.75, 0.9), rects=int(span * tall / 9000), lights=int(span * 0.03))
    near = texture(tall, span, rng, (0.9, 0.6, 0.5), rects=int(span * tall / 15000), lights=int(span * 0.01))
    # The near layer: pillars and crates (alpha) over the background.
    near_a = np.zeros((tall, span), np.float32)
    x = 0
    while x < span - 80:
        w = int(rng.integers(40, 160)); h = int(rng.integers(200, H + 300))
        near_a[tall - h:, x:x + w] = 1.0
        x += w + int(rng.integers(120, 420))
    char = texture(260, 120, rng, (0.4, 0.9, 0.6), rects=12)
    yy, xx = np.mgrid[0:260, 0:120]
    char_a = (((yy - 130) / 130.0) ** 2 + ((xx - 60) / 60.0) ** 2 <= 1).astype(np.float32)
    # HUD: a see-through panel with glyph-like text, opaque icons.
    hud = np.zeros((H, W, 3), np.float32); hud_a = np.zeros((H, W), np.float32)
    hud_a[40:150, 1040:1240] = 0.45; hud[40:150, 1040:1240] = (0.05, 0.1, 0.15)
    for row in range(5):
        for col in range(14):
            if rng.random() < 0.8:
                y0, x0 = 52 + row * 19, 1052 + col * 13
                glyph = rng.random((11, 8)) < 0.45
                hud[y0:y0 + 11, x0:x0 + 8][glyph] = (1.0, 0.85, 0.3); hud_a[y0:y0 + 11, x0:x0 + 8][glyph] = 1.0
    for i in range(4):
        y0, x0 = 600, 40 + i * 52
        hud[y0:y0 + 40, x0:x0 + 40] = (0.2, 0.8, 0.9); hud_a[y0:y0 + 40, x0:x0 + 40] = 1.0
        hud[y0 + 8:y0 + 32, x0 + 8:x0 + 32] = (0.02, 0.05, 0.08)
    gain = 0.25 if kind == "dark" else 1.0
    sub = 8  # sub-steps over the exposure (half a 30 fps frame = 2 frames at 120 Hz)
    for f in range(n):
        acc = np.zeros((H, W, 3), np.float32)
        for s in range(sub):
            tf = f - 1.0 + 2.0 * (s + 0.5) / sub  # exposure centred on the frame's time
            cx, cy = 200 + cam[0] * tf, 200 + cam[1] * tf
            img = shift(far, cx, cy)
            nx, ny = 200 + cam[0] * near_k * tf, 200 + cam[1] * near_k * tf
            na = shift(near_a[..., None], nx, ny)
            img = img * (1 - na) + shift(near, nx, ny) * na
            # the character walks across, against the camera
            px = 900 - 7.0 * tf - (cam[0] * 0.2) * tf; py = 300 + 1.5 * tf
            x0, y0 = int(np.floor(px)), int(np.floor(py))
            if -120 < x0 < W and -260 < y0 < H:
                sx0, sy0 = max(0, -x0), max(0, -y0)
                ex, ey = min(120, W - x0), min(260, H - y0)
                if ex > sx0 and ey > sy0:
                    a = char_a[sy0:ey, sx0:ex, None]
                    img[y0 + sy0:y0 + ey, x0 + sx0:x0 + ex] = img[y0 + sy0:y0 + ey, x0 + sx0:x0 + ex] * (1 - a) + char[sy0:ey, sx0:ex] * a
            acc += img
        img = acc / sub * gain
        img = img * (1 - hud_a[..., None]) + hud * hud_a[..., None]
        a8 = (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
        with open(out / f"f{f:04d}.ppm", "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (W, H)); fh.write(a8.tobytes())


main()
