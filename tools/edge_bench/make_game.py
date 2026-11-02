#!/usr/bin/env python3
"""A 30 fps game seen at 120 Hz, for x4: every 4th frame is what the game
presents, the three between are the exact pictures of the moments the
generated frames show (score with eval_sequence.py --multiplier 4).

    make_game.py DIR turn|walk|dark|strafe|fps|fence|flick|subs [N=97, subs 241]

1280x720: a textured world scrolled by the camera, a near layer moving
faster (parallax), a character moving on its own, a static HUD (a
see-through panel, glyph-like text, a rotating minimap frame left out — the
HUD stays still), and the game's motion blur: each picture integrates the
motion over half a 30 fps frame (8 sub-steps), as a 30 fps game blurs.
`turn` is a fast camera turn (~120 px a game frame), `walk` slow, `dark` the
turn at a quarter of the brightness (low contrast), `strafe` a sideways move
where the near layer moves 2.5x the background, `fps` the turn seen in first
person: a weapon held at the bottom right, nearly still on screen (a slight
sway, not motion-blurred), the world turning fast behind it; `fence` a
sideways move past a railing: the near layer is thin bars (3-8 px, a
fence, poles, cables' supports) 20-60 px apart, moving 2.5x the background
— fine structure in parallax, which 8x8 blocks straddle (motion blur off,
the setting the README advises); `flick` mouse flicks — still, then a
600 px turn in 133 ms (16 frames at 120 Hz, easing in and out: motion
starting and stopping inside a game frame), then still again, every 48
frames; `subs` the turn with subtitles: a see-through box at the bottom with
two lines of anti-aliased white text that change every second (120 frames)
and vanish for its last fifth — HUD text appearing over the moving scene,
what an overlay mask must learn anew each time (241 frames by default)."""
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
    out = Path(sys.argv[1]); kind = sys.argv[2]; n = int(sys.argv[3]) if len(sys.argv) > 3 else (241 if kind == "subs" else 97)
    rng = np.random.default_rng({"turn": 11, "walk": 12, "dark": 13, "strafe": 14, "fps": 15, "fence": 16, "flick": 17,
                                 "subs": 18}[kind])
    out.mkdir(parents=True, exist_ok=True)
    # Camera speed in px per 120 Hz frame (x4 on a 30 fps game: 4 of them per game frame).
    cam = {"turn": (30.0, 3.0), "walk": (6.0, 1.5), "dark": (30.0, 3.0), "strafe": (12.0, 0.0), "fps": (30.0, 2.0),
           "fence": (8.0, 0.0), "flick": (13.0, 1.0), "subs": (30.0, 3.0)}[kind]
    near_k = 2.5 if kind in ("strafe", "fence") else 1.6  # the near layer's speed relative to the background
    span = int(abs(cam[0]) * (n + 2) * near_k) + W + 400
    tall = int(abs(cam[1]) * (n + 2) * near_k) + H + 400
    far = texture(tall, span, rng, (0.7, 0.75, 0.9), rects=int(span * tall / 9000), lights=int(span * 0.03))
    near = texture(tall, span, rng, (0.9, 0.6, 0.5), rects=int(span * tall / 15000), lights=int(span * 0.01))
    # The near layer: pillars and crates (alpha) over the background.
    near_a = np.zeros((tall, span), np.float32)
    x = 0
    if kind == "fence":
        # a railing: a top rail and thin posts, the lower half of the picture
        top = tall - int(H * 0.55)
        near_a[top:top + 6, :] = 1.0
        while x < span - 10:
            w = int(rng.integers(3, 9))
            near_a[top:, x:x + w] = 1.0
            x += w + int(rng.integers(20, 60))
    while kind != "fence" and x < span - 80:
        w = int(rng.integers(40, 160)); h = int(rng.integers(200, H + 300))
        near_a[tall - h:, x:x + w] = 1.0
        x += w + int(rng.integers(120, 420))
    char = texture(260, 120, rng, (0.4, 0.9, 0.6), rects=12)
    yy, xx = np.mgrid[0:260, 0:120]
    char_a = (((yy - 130) / 130.0) ** 2 + ((xx - 60) / 60.0) ** 2 <= 1).astype(np.float32)
    # HUD: a see-through panel with glyph-like text, opaque icons — drawn at
    # 3x and averaged down, so their edges are anti-aliased as a game's are
    # (an edge pixel mixes the HUD and the moving scene behind it).
    SS = 3
    hud3 = np.zeros((H * SS, W * SS, 3), np.float32); a3 = np.zeros((H * SS, W * SS), np.float32)
    def rect(y, x, h, w, col, alpha):
        hud3[y * SS:(y + h) * SS, x * SS:(x + w) * SS] = col; a3[y * SS:(y + h) * SS, x * SS:(x + w) * SS] = alpha
    rect(40, 1040, 110, 200, (0.05, 0.1, 0.15), 0.45)
    for row in range(5):
        for col in range(14):
            if rng.random() < 0.8:
                y0, x0 = (52 + row * 19) * SS + 1, (1052 + col * 13) * SS + 1
                glyph = rng.random((11, 8)) < 0.45
                g3 = np.kron(glyph, np.ones((SS, SS), bool))  # glyph strokes at 3x, offset by a third of a pixel
                hud3[y0:y0 + 11 * SS, x0:x0 + 8 * SS][g3] = (1.0, 0.85, 0.3); a3[y0:y0 + 11 * SS, x0:x0 + 8 * SS][g3] = 1.0
    for i in range(4):
        y0, x0 = 600 * SS + 1, (40 + i * 52) * SS + 2
        hud3[y0:y0 + 40 * SS, x0:x0 + 40 * SS] = (0.2, 0.8, 0.9); a3[y0:y0 + 40 * SS, x0:x0 + 40 * SS] = 1.0
        hud3[y0 + 8 * SS:y0 + 32 * SS, x0 + 8 * SS:x0 + 32 * SS] = (0.02, 0.05, 0.08)
    # premultiplied, then box-averaged to 1x
    pm = (hud3 * a3[..., None]).reshape(H, SS, W, SS, 3).mean((1, 3))
    hud_a = a3.reshape(H, SS, W, SS).mean((1, 3))
    hud = np.where(hud_a[..., None] > 0, pm / np.maximum(hud_a[..., None], 1e-6), 0.0)
    gain = 0.25 if kind == "dark" else 1.0
    # subs: the subtitles' lines, one set a second, drawn at 3x like the HUD
    subs = []
    if kind == "subs":
        for line in range(n // 120 + 1):
            s3 = np.zeros((H * SS, W * SS, 3), np.float32); sa = np.zeros((H * SS, W * SS), np.float32)
            sa[600 * SS:672 * SS, 340 * SS:940 * SS] = 0.6  # the box (black, see-through)
            for row in range(2):
                cols = int(rng.integers(24, 40))
                x0 = (640 - cols * 7) * SS
                for col in range(cols):
                    if rng.random() < 0.15:
                        continue  # (a space between words)
                    g = np.kron(rng.random((14, 6)) < 0.5, np.ones((SS, SS), bool))
                    y1, x1 = (610 + row * 30) * SS + 1, x0 + col * 14 * SS // 2 + 1
                    s3[y1:y1 + 14 * SS, x1:x1 + 6 * SS][g] = (0.95, 0.95, 0.95)
                    sa[y1:y1 + 14 * SS, x1:x1 + 6 * SS][g] = 1.0
            spm = (s3 * sa[..., None]).reshape(H, SS, W, SS, 3).mean((1, 3))
            s_a = sa.reshape(H, SS, W, SS).mean((1, 3))
            subs.append((np.where(s_a[..., None] > 0, spm / np.maximum(s_a[..., None], 1e-6), 0.0), s_a))
    # fps: the weapon, a textured shape anchored at the bottom right
    wh, ww = 300, 420
    weap = texture(wh, ww, rng, (0.45, 0.45, 0.5), rects=25)
    yy2, xx2 = np.mgrid[0:wh, 0:ww]
    weap_a = ((yy2 > wh - (xx2 * 0.9) - 40) & (yy2 > 60) & (xx2 > 40)).astype(np.float32)  # a slanted barrel and body
    weap_a[:, :40] = 0
    sub = 8  # sub-steps over the exposure (half a 30 fps frame = 2 frames at 120 Hz)
    if kind == "fence":
        sub = 1  # (motion blur off, as advised: the bars stay sharp)
    def campos(tf):
        if kind != "flick":
            return cam[0] * tf, cam[1] * tf
        # (a flick every 48 frames: 600 px over 16 frames, smootherstep;
        # cam[] is only used for the textures' extent above)
        k, u = divmod(tf - 12.0, 48.0)
        s = min(max(u / 16.0, 0.0), 1.0)
        e = s * s * s * (s * (s * 6 - 15) + 10)
        return 600.0 * (k + e), 30.0 * (k + e)
    for f in range(n):
        acc = np.zeros((H, W, 3), np.float32)
        for s in range(sub):
            tf = f - 1.0 + 2.0 * (s + 0.5) / sub  # exposure centred on the frame's time
            ox, oy = campos(tf)
            cx, cy = 200 + ox, 200 + oy
            img = shift(far, cx, cy)
            nx, ny = 200 + ox * near_k, 200 + oy * near_k
            na = shift(near_a[..., None], nx, ny)
            img = img * (1 - na) + shift(near, nx, ny) * na
            # the character walks across, against the camera
            px = 900 - 7.0 * tf - 0.2 * ox; py = 300 + 1.5 * tf
            x0, y0 = int(np.floor(px)), int(np.floor(py))
            if -120 < x0 < W and -260 < y0 < H:
                sx0, sy0 = max(0, -x0), max(0, -y0)
                ex, ey = min(120, W - x0), min(260, H - y0)
                if ex > sx0 and ey > sy0:
                    a = char_a[sy0:ey, sx0:ex, None]
                    img[y0 + sy0:y0 + ey, x0 + sx0:x0 + ex] = img[y0 + sy0:y0 + ey, x0 + sx0:x0 + ex] * (1 - a) + char[sy0:ey, sx0:ex] * a
            acc += img
        img = acc / sub * gain
        if kind == "fps":  # the sway: a few pixels, slow (not blurred: drawn once at the frame's time)
            sx, sy = 3.0 * np.sin(f * 0.11), 2.0 * np.sin(f * 0.07 + 1.0)
            x0, y0 = int(round(W - ww + 20 + sx)), int(round(H - wh + 30 + sy))
            ex, ey = min(ww, W - x0), min(wh, H - y0)
            a = weap_a[:ey, :ex, None]
            img[y0:y0 + ey, x0:x0 + ex] = img[y0:y0 + ey, x0:x0 + ex] * (1 - a) + weap[:ey, :ex] * a
        if subs and f % 120 < 96:
            sc, s_a = subs[f // 120]
            img = img * (1 - s_a[..., None]) + sc * s_a[..., None]
        img = img * (1 - hud_a[..., None]) + hud * hud_a[..., None]
        a8 = (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)
        with open(out / f"f{f:04d}.ppm", "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (W, H)); fh.write(a8.tobytes())


main()
