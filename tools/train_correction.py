#!/usr/bin/env python3
"""Train the learned correction (learned_correction): a tiny network that
adds a residual to each generated pixel, from its 3x3 neighbourhood.

    tools/train_correction.py --train SEQ... --test SEQ... [--hidden 8] [--write]

Each sequence is a directory of consecutive PPM frames (tools/make_corpus.py,
a recorded game, the demo's dumps); bdex_offline generates its in-between
frames with the current engine (every other frame as input, the others as
ground truth, x2). Per pixel the network sees, over its 3x3 neighbourhood,
the generated colour (relative to the centre pixel), the plain cross-fade of
the two inputs at t and their luma difference: 63 inputs, a hidden layer of
`--hidden` ReLU units. The "mix" head (default) outputs one weight that moves
the pixel toward the cross-fade, never past it; "residual" a free colour
offset (it made correct pixels worse: subpixel -11 dB). numpy only (Adam,
mean squared error); the test sequences are never trained on.

Reports the PSNR of the test frames without and with the correction (whole
frame and moving pixels). `--write` stores the weights in
layer/shaders/learned_correction_weights.glsl (the pass's shader includes
it) and the training record in layer/learned_correction.json.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
N_IN = 63  # features()


def load_ppm(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def luma(img):
    return img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114


def generate(build, seq, work, mode="interpolate"):
    out = Path(work) / f"{Path(seq).name}-{mode}"
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, BDEX_FG_MODE=mode, BDEX_FG_CONFIG="/dev/null")
    subprocess.run([str(Path(build) / "tools" / "offline" / "bdex_offline"), str(seq), str(out), "--multiplier", "2"],
                   check=True, capture_output=True, env=env)
    frames = []
    for line in (out / "manifest.txt").read_text().splitlines():
        gen, gt, t, prev, cur = line.split()
        if not gen.startswith("gen_"):
            continue
        frames.append((out / gen, Path(gt), float(t), Path(prev), Path(cur)))
    return frames


def fallback(prev, cur, t):
    """What the network moves the pixel toward: the plain cross-fade between
    the inputs (interpolation), the current frame itself past it
    (extrapolation, t > 1: nothing safer to show)."""
    if t > 1.0:
        return cur.astype(np.float32)
    return prev.astype(np.float32) * (1.0 - t) + cur.astype(np.float32) * t


def features(g, prev, cur, t):
    """(H, W, 63) float32: over the 3x3 neighbourhood, the generated colour
    minus the centre's, the cross-fade minus the centre's generated colour,
    and the inputs' luma difference — all in [0, 1] units. (Adding the
    centre's distance to each input made subpixel -11 dB: left out.)"""
    g = g.astype(np.float32) / 255.0
    b = fallback(prev, cur, t) / 255.0
    d = np.abs(luma(prev.astype(np.float32)) - luma(cur.astype(np.float32)))[..., None] / 255.0
    gp, bp, dp = (np.pad(a, ((1, 1), (1, 1), (0, 0)), mode="edge") for a in (g, b, d))
    h, w = g.shape[:2]
    out = []
    for dy in range(3):
        for dx in range(3):
            out.append(gp[dy:dy + h, dx:dx + w] - g)   # 3: neighbour - centre (centre: 0)
            out.append(bp[dy:dy + h, dx:dx + w] - g)   # 3: blend - centre
            out.append(dp[dy:dy + h, dx:dx + w])       # 1
    return np.concatenate(out, axis=2)


class Net:
    def __init__(self, n_in, hidden, rng):
        self.w1 = (rng.standard_normal((n_in, hidden)) * np.sqrt(2.0 / n_in)).astype(np.float32)
        self.b1 = np.zeros(hidden, np.float32)
        self.w2 = np.zeros((hidden, 3), np.float32)  # starts as no correction
        self.b2 = np.zeros(3, np.float32)
        self.params = [self.w1, self.b1, self.w2, self.b2]
        self.m = [np.zeros_like(p) for p in self.params]
        self.v = [np.zeros_like(p) for p in self.params]
        self.step = 0

    def forward(self, x):
        h = np.maximum(x @ self.w1 + self.b1, 0.0)
        return h @ self.w2 + self.b2, h

    def train_batch(self, x, y, lr, decay=1e-5):
        pred, h = self.forward(x)
        err = (pred - y) / len(x)
        gw2 = h.T @ err + decay * self.w2
        gb2 = err.sum(0)
        dh = (err @ self.w2.T) * (h > 0)
        gw1 = x.T @ dh + decay * self.w1
        gb1 = dh.sum(0)
        self.step += 1
        for p, g, m, v in zip(self.params, [gw1, gb1, gw2, gb2], self.m, self.v):
            m *= 0.9
            m += 0.1 * g
            v *= 0.999
            v += 0.001 * g * g
            mh = m / (1 - 0.9 ** self.step)
            vh = v / (1 - 0.999 ** self.step)
            p -= lr * mh / (np.sqrt(vh) + 1e-8)
        return float(((pred - y) ** 2).mean())


class MixNet(Net):
    """The "mix" head: one output s per pixel, alpha = sigmoid(s), and the
    pixel moved alpha of the way from the generated colour to the
    cross-fade — never past either, so a correct generated pixel cannot be
    made worse than the cross-fade would make it, and alpha = 0 (the start:
    sigmoid(-6)) changes nothing."""

    def __init__(self, n_in, hidden, rng):
        super().__init__(n_in, hidden, rng)
        self.w2 = np.zeros((hidden, 1), np.float32)
        self.b2 = np.full(1, -6.0, np.float32)
        self.params = [self.w1, self.b1, self.w2, self.b2]
        self.m = [np.zeros_like(p) for p in self.params]
        self.v = [np.zeros_like(p) for p in self.params]

    @staticmethod
    def blend_delta(x):
        return x[:, 4 * 7 + 3:4 * 7 + 6]  # the centre's cross-fade minus its generated colour

    def forward(self, x):
        h = np.maximum(x @ self.w1 + self.b1, 0.0)
        a = 1.0 / (1.0 + np.exp(-(h @ self.w2 + self.b2)))
        return a * self.blend_delta(x), (h, a)

    def train_batch(self, x, y, lr, decay=1e-5):
        pred, (h, a) = self.forward(x)
        err = (pred - y) / len(x)
        ds = (err * self.blend_delta(x)).sum(1, keepdims=True) * a * (1.0 - a)
        gw2 = h.T @ ds + decay * self.w2
        gb2 = ds.sum(0)
        dh = (ds @ self.w2.T) * (h > 0)
        gw1 = x.T @ dh + decay * self.w1
        gb1 = dh.sum(0)
        self.step += 1
        for p, g, m, v in zip(self.params, [gw1, gb1, gw2, gb2], self.m, self.v):
            m *= 0.9
            m += 0.1 * g
            v *= 0.999
            v += 0.001 * g * g
            mh = m / (1 - 0.9 ** self.step)
            vh = v / (1 - 0.999 ** self.step)
            p -= lr * mh / (np.sqrt(vh) + 1e-8)
        return float(((pred - y) ** 2).mean())


def psnr(a, b, mask=None):
    d = (a.astype(np.float32) - b.astype(np.float32)) ** 2
    if mask is not None:
        if mask.sum() < 64:
            return None
        d = d[mask]
    m = float(d.mean())
    return 99.0 if m == 0 else 10 * np.log10(255.0 ** 2 / m)


def apply(net, g, prev, cur, t):
    x = features(g, prev, cur, t)
    h, w = g.shape[:2]
    delta, _ = net.forward(x.reshape(-1, x.shape[2]))
    out = g.astype(np.float32) + 255.0 * delta.reshape(h, w, 3)
    return np.clip(np.round(out), 0, 255).astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--train", nargs="+", required=True)
    ap.add_argument("--test", nargs="+", required=True)
    ap.add_argument("--hidden", type=int, default=8)
    ap.add_argument("--head", choices=["mix", "residual"], default="mix",
                    help="mix: a weight toward the cross-fade (bounded); residual: a free colour offset")
    ap.add_argument("--samples", type=int, default=20000, help="training pixels per frame")
    ap.add_argument("--epochs", type=int, default=12)
    ap.add_argument("--lr", type=float, default=2e-3)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--mode", choices=["interpolate", "extrapolate"], default="interpolate",
                    help="extrapolate: the predicted frames (t > 1), moved toward the current frame")
    ap.add_argument("--write", action="store_true", help="store the weights for the shader")
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)
    work = tempfile.mkdtemp(prefix="bdex-train-")
    xs, ys = [], []
    for seq in a.train:
        for gen, gt, t, prev, cur in generate(a.build, seq, work, a.mode):
            g, y, p, c = load_ppm(gen), load_ppm(gt), load_ppm(prev), load_ppm(cur)
            x = features(g, p, c, t).reshape(-1, N_IN)
            target = ((y.astype(np.float32) - g.astype(np.float32)) / 255.0).reshape(-1, 3)
            # (half of the samples where the inputs differ: where the motion is)
            moving = np.flatnonzero(np.abs(luma(p.astype(np.float32)) - luma(c.astype(np.float32))).ravel() > 8)
            k = a.samples // 2
            idx = np.concatenate([rng.choice(len(x), k), rng.choice(moving, k) if len(moving) else rng.choice(len(x), k)])
            xs.append(x[idx])
            ys.append(target[idx])
    x, y = np.concatenate(xs), np.concatenate(ys)
    print(f"training on {len(x)} pixels from {len(a.train)} sequences", file=sys.stderr)
    net = (MixNet if a.head == "mix" else Net)(N_IN, a.hidden, rng)
    for epoch in range(a.epochs):
        order = rng.permutation(len(x))
        loss = 0.0
        for i in range(0, len(x), 4096):
            j = order[i:i + 4096]
            loss += net.train_batch(x[j], y[j], a.lr * (0.5 if epoch >= a.epochs * 2 // 3 else 1.0)) * len(j)
        print(f"  epoch {epoch + 1}: mse {loss / len(x):.6f}", file=sys.stderr)

    results = {}
    print(f"{'test sequence':24s} {'psnr':>7s} {'+corr':>7s} {'Δ':>6s}   {'motion':>7s} {'+corr':>7s} {'Δ':>6s}")
    for seq in a.test:
        before, after, mb, ma = [], [], [], []
        for gen, gt, t, prev, cur in generate(a.build, seq, work, a.mode):
            g, y, p, c = load_ppm(gen), load_ppm(gt), load_ppm(prev), load_ppm(cur)
            fixed = apply(net, g, p, c, t)
            motion = np.abs(luma(p.astype(np.float32)) - luma(c.astype(np.float32))) > 12
            before.append(psnr(g, y))
            after.append(psnr(fixed, y))
            pb, pa = psnr(g, y, motion), psnr(fixed, y, motion)
            if pb is not None:
                mb.append(pb)
                ma.append(pa)
        r = {"psnr": float(np.mean(before)), "psnr_corrected": float(np.mean(after)),
             "motion": float(np.mean(mb)) if mb else None, "motion_corrected": float(np.mean(ma)) if ma else None}
        results[Path(seq).name] = r
        dm = f"{r['motion_corrected'] - r['motion']:+6.2f}" if mb else "     —"
        print(f"{Path(seq).name:24s} {r['psnr']:7.2f} {r['psnr_corrected']:7.2f} {r['psnr_corrected'] - r['psnr']:+6.2f}"
              f"   {r['motion'] or 0:7.2f} {r['motion_corrected'] or 0:7.2f} {dm}")
    if a.write:
        # (one file and prefix per mode: LC_ interpolation, LCX_ extrapolation)
        ext = a.mode == "extrapolate"
        pre, suffix = ("LCX_", "_extrap") if ext else ("LC_", "")
        glsl = ROOT / "layer" / "shaders" / f"learned_correction{suffix}_weights.glsl"
        def arr(name, v):
            return f"const float {pre}{name}[{v.size}] = float[](" + ", ".join(f"{f:.8g}" for f in v.ravel()) + ");\n"
        outs = net.w2.shape[1]
        glsl.write_text(f"// Generated by tools/train_correction.py --mode {a.mode} (see layer/learned_correction{suffix}.json):\n"
                        f"// do not edit. Row-major: W1[63][H], B1[H], W2[H][{outs}], B2[{outs}].\n"
                        f"#define {pre}HIDDEN {a.hidden}\n#define {pre}MIX {1 if a.head == 'mix' else 0}\n" +
                        arr("W1", net.w1) + arr("B1", net.b1) + arr("W2", net.w2) + arr("B2", net.b2))
        (ROOT / "layer" / f"learned_correction{suffix}.json").write_text(json.dumps(
            {"mode": a.mode, "hidden": a.hidden, "head": a.head, "train": [Path(s).name for s in a.train], "test": results,
             "samples_per_frame": a.samples, "epochs": a.epochs, "seed": a.seed}, indent=1) + "\n")
        print(f"weights written to {glsl}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
