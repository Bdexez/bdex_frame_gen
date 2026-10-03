#!/usr/bin/env python3
"""Train learned_flow: the small network that corrects the finest flow
(layer/shaders/learned_flow.comp).

    tools/train_flow.py [--build build] [--hidden 16] [--epochs 40] [--write]

Synthetic sequences whose true motion is known (tools/make_corpus.py's pans
at several speeds, sub-pixel pan, swinging camera, zoom — other seeds and
speeds than the evaluation corpus) go through bdex_offline --dump-flow; per
flow texel the network sees what the shader computes (the 8 neighbours
relative to the vector, the matching error and its slope one pixel away in
each direction, the vector's fractional part) and learns the correction to
the true vector (at most 1.5 level pixels). numpy only (Adam, mean squared
error). Reports the end-point error of the engine's flow and of the
corrected one on held-out sequences; --write stores the weights in
layer/shaders/learned_flow_weights.glsl and the record in
layer/learned_flow.json. Then measure with tools/eval_sequence.py
--variant learned_flow=1 on the evaluation corpus (never trained on).
"""
import argparse
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
N_IN = 23
W, H, FRAMES = 960, 540, 13


def corpus_module():
    spec = importlib.util.spec_from_file_location("make_corpus", ROOT / "tools" / "make_corpus.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# The sequences: (name, maker(out), true flow (frame a -> b) in display px at display positions x, y).
def sequences(mc, held_out):
    out = []
    pans = [(14, 301)] if held_out else [(1, 205), (6, 201), (11, 202), (20, 203), (27, 204)]
    for speed, seed in pans:
        out.append((f"pan{speed}", lambda d, s=speed, sd=seed: mc.pan(d, W, H, FRAMES, s, sd),
                    lambda x, y, a, b, s=speed: (np.full_like(x, -(b - a) * s), np.zeros_like(y))))
    seed = 311 if held_out else 211
    out.append(("subpixel", lambda d, sd=seed: mc.subpixel(d, W, H, FRAMES, sd),
                lambda x, y, a, b: (np.full_like(x, -(b - a) * 13 / 4), np.zeros_like(y))))
    amp = 60 * W / 640
    sw = lambda i: amp * np.sin(2 * np.pi * i / 24)
    seed = 321 if held_out else 221
    out.append(("swing", lambda d, sd=seed: mc.swing(d, W, H, FRAMES, sd),
                lambda x, y, a, b: (np.full_like(x, -(sw(b) - sw(a))), np.zeros_like(y))))
    seed = 331 if held_out else 231

    def zoom_flow(x, y, a, b):
        r = 1.02 ** b / 1.02 ** a
        return (x - W / 2) * (r - 1), (y - H / 2) * (r - 1)
    out.append(("zoom", lambda d, sd=seed: mc.zoom(d, W, H, FRAMES, sd), zoom_flow))
    return out


def read_dump(path):
    d = path.read_bytes()
    _, w, h, block, lw, lh = np.frombuffer(d[:24], np.uint32)
    scale = float(np.frombuffer(d[24:28], np.float32)[0])
    o = 28
    fwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2); o += w * h * 8
    bwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2); o += w * h * 8
    lp = np.frombuffer(d[o:o + lw * lh * 4], np.float32).reshape(lh, lw); o += lw * lh * 4
    lc = np.frombuffer(d[o:o + lw * lh * 4], np.float32).reshape(lh, lw)
    return dict(w=int(w), h=int(h), block=int(block), scale=scale, fwd=fwd, bwd=bwd, prev=lp, cur=lc)


def features(flow, ref, tgt, block):
    """The shader's inputs for every texel of `flow` (h, w, 2): (h, w, 23)."""
    h, w, _ = flow.shape
    lh, lw = ref.shape
    py, px = np.mgrid[0:h, 0:w]
    feats = []
    for k in range(9):
        if k == 4:
            continue
        oy, ox = k // 3 - 1, k % 3 - 1
        n = flow[np.clip(py + oy, 0, h - 1), np.clip(px + ox, 0, w - 1)]
        r = np.clip(n - flow, -8, 8) * 0.25
        feats += [r[..., 0], r[..., 1]]
    m = (8 - block) // 2
    wy, wx = np.mgrid[-m:block + m, -m:block + m]
    qy = (py * block)[..., None] + wy.ravel()  # (h, w, 64)
    qx = (px * block)[..., None] + wx.ravel()
    at = lambda img, y, x: img[np.clip(y, 0, lh - 1), np.clip(x, 0, lw - 1)]
    r0 = at(ref, qy, qx)
    contrast = 0.01 + ((np.abs(at(ref, qy, qx + 1) - r0) + np.abs(at(ref, qy + 1, qx) - r0)) / 64).sum(-1)
    vi = np.floor(flow + 0.5).astype(np.int64)  # as the shader: floor(c + 0.5)
    def err(dx, dy):
        return np.abs(r0 - at(tgt, qy + vi[..., 1:2] + dy, qx + vi[..., 0:1] + dx)).sum(-1) / 64
    e0, ex, exm, ey, eym = err(0, 0), err(1, 0), err(-1, 0), err(0, 1), err(0, -1)
    feats += [e0 / contrast, (ex - e0) / contrast, (exm - e0) / contrast, (ey - e0) / contrast, (eym - e0) / contrast]
    feats += [flow[..., 0] - vi[..., 0], flow[..., 1] - vi[..., 1]]
    return np.stack(feats, -1).astype(np.float32)


def collect(build, seqs, work, tag):
    """(inputs, targets, engine error, valid) over every pair of every sequence."""
    X, Y = [], []
    for name, make, truth in seqs:
        d = Path(work) / f"{tag}-{name}"
        (d / "frames").mkdir(parents=True, exist_ok=True)
        make(d / "frames")
        env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET=os.environ.get("BDEX_FG_PRESET", "auto"),
                   BDEX_FG_LEARNED_FLOW="0")
        subprocess.run([str(Path(build) / "tools" / "offline" / "bdex_offline"), str(d / "frames"), str(d / "out"),
                        "--multiplier", "2", "--dump-flow", str(d / "flow")], check=True, capture_output=True, env=env)
        for f in sorted((d / "flow").glob("flow_*.bin")):
            k = int(f.stem.split("_")[1])
            a, b = 2 * (k - 1), 2 * k
            D = read_dump(f)
            blk, sc = D["block"], D["scale"]
            py, px = np.mgrid[0:D["h"], 0:D["w"]]
            cx, cy = (px * blk + blk / 2) * sc, (py * blk + blk / 2) * sc  # texel centres, display px
            inside = (px >= 2) & (px < D["w"] - 2) & (py >= 2) & (py < D["h"] - 2)
            for flow, ref, tgt, (fa, fb) in ((D["fwd"], D["prev"], D["cur"], (a, b)), (D["bwd"], D["cur"], D["prev"], (b, a))):
                gx, gy = truth(cx, cy, fa, fb)
                gt = np.stack([gx, gy], -1) / sc
                diff = gt - flow
                ok = inside & (np.abs(diff).max(-1) < 3.0)
                X.append(features(flow, ref, tgt, blk)[ok])
                Y.append(np.clip(diff, -1.5, 1.5)[ok].astype(np.float32))
        print(f"  {tag} {name}: {sum(len(x) for x in X)} texels so far", file=sys.stderr)
    return np.concatenate(X), np.concatenate(Y)


def forward(p, X):
    h = np.maximum(X @ p["W1"].T + p["B1"], 0)
    return np.clip(h @ p["W2"].T + p["B2"], -1.5, 1.5), h


def train(X, Y, hidden, epochs, seed=1):
    rng = np.random.default_rng(seed)
    p = {"W1": rng.normal(0, 1 / np.sqrt(N_IN), (hidden, N_IN)).astype(np.float32), "B1": np.zeros(hidden, np.float32),
         "W2": rng.normal(0, 0.01, (2, hidden)).astype(np.float32), "B2": np.zeros(2, np.float32)}
    m = {k: np.zeros_like(v) for k, v in p.items()}
    v = {k: np.zeros_like(v) for k, v in p.items()}
    lr, b1, b2, step = 2e-3, 0.9, 0.999, 0
    for ep in range(epochs):
        order = rng.permutation(len(X))
        for i in range(0, len(X), 4096):
            idx = order[i:i + 4096]
            x, y = X[idx], Y[idx]
            out, hid = forward(p, x)
            g = 2 * (out - y) / len(x) * (np.abs(out) < 1.5)
            grads = {"W2": g.T @ hid, "B2": g.sum(0)}
            gh = (g @ p["W2"]) * (hid > 0)
            grads["W1"] = gh.T @ x
            grads["B1"] = gh.sum(0)
            step += 1
            for k in p:
                m[k] = b1 * m[k] + (1 - b1) * grads[k]
                v[k] = b2 * v[k] + (1 - b2) * grads[k] ** 2
                p[k] -= lr * (m[k] / (1 - b1 ** step)) / (np.sqrt(v[k] / (1 - b2 ** step)) + 1e-8)
        if ep % 10 == 9 or ep == epochs - 1:
            print(f"  epoch {ep + 1}: train mse {np.mean((forward(p, X)[0] - Y) ** 2):.4f}", file=sys.stderr)
    return p


def epe(pred, Y):
    return float(np.mean(np.linalg.norm(Y - pred, axis=-1)))


def write_weights(p, hidden):
    def arr(name, a):
        vals = ", ".join(f"{float(x):.7g}" for x in np.asarray(a).ravel())
        return f"const float {name}[{np.asarray(a).size}] = float[{np.asarray(a).size}]({vals});\n"
    s = ("// learned_flow's network, written by tools/train_flow.py --write (layer/learned_flow.json:\n"
         "// the training record). W1 is hidden x 23 row-major, W2 is 2 x hidden.\n"
         f"const int LF_HIDDEN = {hidden};\n")
    s += arr("LF_W1", p["W1"]) + arr("LF_B1", p["B1"]) + arr("LF_W2", p["W2"]) + arr("LF_B2", p["B2"])
    (ROOT / "layer" / "shaders" / "learned_flow_weights.glsl").write_text(s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--hidden", type=int, default=16)
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    if a.hidden != 16:
        sys.exit("--hidden: the shader's LF_HIDDEN must match (16)")
    mc = corpus_module()
    with tempfile.TemporaryDirectory() as work:
        X, Y = collect(a.build, sequences(mc, False), work, "train")
        Xt, Yt = collect(a.build, sequences(mc, True), work, "test")
    print(f"training on {len(X)} texels, testing on {len(Xt)}", file=sys.stderr)
    p = train(X, Y, a.hidden, a.epochs)
    zero = np.zeros_like(Yt)
    before, after = epe(zero, Yt), epe(forward(p, Xt)[0], Yt)
    print(f"held-out end-point error (level px, clipped to 1.5): engine {before:.4f} -> corrected {after:.4f}")
    if a.write:
        write_weights(p, a.hidden)
        (ROOT / "layer" / "learned_flow.json").write_text(json.dumps({
            "train_texels": int(len(X)), "test_texels": int(len(Xt)), "epochs": a.epochs, "hidden": a.hidden,
            "held_out_epe_engine": round(before, 4), "held_out_epe_corrected": round(after, 4)}, indent=1) + "\n")
        print("weights written to layer/shaders/learned_flow_weights.glsl (rebuild)")


if __name__ == "__main__":
    main()
