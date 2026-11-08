#!/usr/bin/env python3
"""Game-like 1280x720 sequence with exact ground truth, for measuring the error
at the edges of independently moving objects: textured world scrolled by the
camera, seven textured objects (z-ordered, integer motion), a static HUD.

    make_sequence.py DIR still|pan|fast|lowcon [N=33]

Writes DIR/fNNNN.ppm (score with x2: odd frames are the ground truth),
DIR/truth.npz (owner of every pixel, per frame; velocity of every owner) and
DIR/depth/fNNNN.f32, the depth buffer a game would have (reversed Z, D3D's:
near / z, near = 0.1; the world at 100, object i at 20 - 2i, painted in that
order; the HUD is drawn without depth: the world's there) — bdex_offline
--depth DIR/depth.
Then: bdex_offline DIR OUT --multiplier 2 [--dump-flow OUT/flow]; score.py DIR OUT [--flow]."""
import sys, numpy as np
from pathlib import Path

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


def texture(h, w, rng, tint, contrast=1.0, rects=40):
    n = noise(h, w, rng)
    img = np.stack([n * tint[0], n * tint[1], n * tint[2]], -1)
    for _ in range(rects):  # sharp features (bricks, props): edges to match on
        y, x = rng.integers(0, h), rng.integers(0, w)
        rh, rw = rng.integers(3, max(4, min(40, h // 4))), rng.integers(3, max(4, min(40, w // 4)))
        img[y:y + rh, x:x + rw] = rng.random(3) * 0.8 + 0.1
    m = img.mean()
    img = m + (img - m) * contrast
    return np.clip(img, 0, 1)


def main():
    out = Path(sys.argv[1]); kind = sys.argv[2]; n = int(sys.argv[3]) if len(sys.argv) > 3 else 33
    seed = {"still": 1, "pan": 2, "fast": 3, "lowcon": 4}[kind]
    rng = np.random.default_rng(seed)
    out.mkdir(parents=True, exist_ok=True)
    cam = {"still": (0, 0), "pan": (4, 1), "fast": (7, -2), "lowcon": (3, 0)}[kind]
    vmax = {"still": 5, "pan": 5, "fast": 11, "lowcon": 5}[kind]
    bg = texture(H + 400, W + 600, rng, (0.9, 0.8, 0.6), rects=900)
    objs = []
    for i in range(7):
        h, w = int(rng.integers(50, 200)), int(rng.integers(50, 220))
        con = 0.35 if kind == "lowcon" else 1.0
        tint = rng.random(3) * 0.6 + 0.4
        if kind == "lowcon": tint = np.array([0.85, 0.8, 0.65])  # close to the world's colours
        tex = texture(h, w, rng, tint, con, rects=6)
        disc = rng.random() < 0.4
        mask = np.ones((h, w), bool)
        if disc:
            yy, xx = np.mgrid[0:h, 0:w]
            mask = ((yy - h / 2) / (h / 2)) ** 2 + ((xx - w / 2) / (w / 2)) ** 2 <= 1
        v = rng.integers(-vmax, vmax + 1, 2)
        while abs(v).max() < 2: v = rng.integers(-vmax, vmax + 1, 2)
        p0 = np.array([rng.integers(100, H - 100 - h // 2), rng.integers(100, W - 100 - w // 2)]) - v[::-1] * n // 2
        objs.append(dict(tex=tex, mask=mask, v=np.array([v[0], v[1]]), p0=p0))  # v = (vx, vy) px/frame
    hud = texture(60, 260, rng, (1, 1, 1), 1.0, rects=30)
    owners = np.zeros((n, H, W), np.int8)
    vel = np.zeros((n, 9, 2), np.int32)  # per owner: 0 world, 1 hud, 2+i objects
    for f in range(n):
        ox, oy = 200 + cam[0] * f, 200 + cam[1] * f
        img = bg[oy:oy + H, ox:ox + W].copy()
        own = np.zeros((H, W), np.int8)
        vel[f, 0] = (-cam[0], -cam[1]); vel[f, 1] = (0, 0)
        for i, o in enumerate(objs):
            y, x = o["p0"][0] + o["v"][1] * f, o["p0"][1] + o["v"][0] * f
            vel[f, 2 + i] = o["v"]
            h, w = o["mask"].shape
            y0, x0, y1, x1 = max(y, 0), max(x, 0), min(y + h, H), min(x + w, W)
            if y1 <= y0 or x1 <= x0: continue
            m = o["mask"][y0 - y:y1 - y, x0 - x:x1 - x]
            img[y0:y1, x0:x1][m] = o["tex"][y0 - y:y1 - y, x0 - x:x1 - x][m]
            own[y0:y1, x0:x1][m] = 2 + i
        img[640:700, 980:1240] = hud; own[640:700, 980:1240] = 1
        owners[f] = own
        z = np.where(own >= 2, 20.0 - 2.0 * (own.astype(np.float32) - 2.0), 100.0)  # (HUD pixels: the world)
        (out / "depth").mkdir(exist_ok=True)
        (0.1 / z).astype("<f4").tofile(out / "depth" / f"f{f:04d}.f32")
        a = (img * 255 + 0.5).astype(np.uint8)
        with open(out / f"f{f:04d}.ppm", "wb") as fh:
            fh.write(b"P6\n%d %d\n255\n" % (W, H)); fh.write(a.tobytes())
    np.savez_compressed(out / "truth.npz", owners=owners, vel=vel)


main()
