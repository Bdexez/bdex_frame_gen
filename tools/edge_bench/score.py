#!/usr/bin/env python3
"""score.py SEQ OUT [--flow]: bdex_offline's output on a make_sequence.py
sequence. PSNR of the whole frame, of the band of +-6 px around object
boundaries and of the rest, and the band's share of the squared error. With
--flow (bdex_offline --dump-flow OUT/flow): band pixels split by whether the
true vector is among the 2x2 / 4x4 flow texels the synthesis can try, and the
occluded ones (visible in one input only) by the side they are visible on —
the share of the error each class holds (2026-10-05: occlusions ~60 %)."""
import sys, numpy as np
from pathlib import Path

R = 6  # band half-width, display px


def ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).astype(np.float32)


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy > r * r: continue
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def boundary(own):
    b = np.zeros(own.shape, bool)
    for ax in (0, 1):
        d = np.diff(own, axis=ax) != 0
        obj = (np.maximum(np.take(own, range(own.shape[ax] - 1), ax), np.take(own, range(1, own.shape[ax]), ax)) >= 2)
        e = d & obj
        if ax == 0: b[:-1] |= e; b[1:] |= e
        else: b[:, :-1] |= e; b[:, 1:] |= e
    return b


def read_dump(path):
    d = open(path, "rb").read()
    _, w, h, block, lw, lh = np.frombuffer(d[:24], np.uint32)
    scale = float(np.frombuffer(d[24:28], np.float32)[0])
    o = 28
    fwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2); o += w * h * 8
    bwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2)
    return int(w), int(h), int(block), scale, fwd, bwd


def candidates(flow, block, scale, src, V, n):
    """Is V (display px) within 1 px of one of the n x n flow texels around src?"""
    fh, fw = flow.shape[:2]
    u = src / (block * scale) - 0.5
    f0 = np.floor(u).astype(int) - (n - 2) // 2
    hit = np.zeros(src.shape[:-1], bool)
    for j in range(n):
        for i in range(n):
            x = np.clip(f0[..., 0] + i, 0, fw - 1); y = np.clip(f0[..., 1] + j, 0, fh - 1)
            c = flow[y, x] * scale
            hit |= np.abs(c - V).max(-1) <= 1.0
    return hit


def main():
    seq, out = Path(sys.argv[1]), Path(sys.argv[2])
    T = np.load(seq / "truth.npz"); owners, vel = T["owners"], T["vel"]
    H, W = owners.shape[1:]
    se_all = se_band = n_band = n_all = 0.0
    cov = dict()
    for line in open(out / "manifest.txt"):
        gen, gt, t, a, b = line.split()
        g = int(Path(gt).stem[1:])
        e = ((ppm(out / gen) - ppm(gt)) ** 2).mean(-1)
        e[640:700, 980:1240] = 0  # HUD: scored elsewhere
        band = dilate(boundary(owners[g]), R)
        se_all += e.sum(); n_all += e.size
        se_band += e[band].sum(); n_band += band.sum()
        k = (g + 1) // 2
        fp = out / "flow" / f"flow_{k:04d}.bin"
        if "--flow" in sys.argv and fp.exists():
            fw_, fh_, blk, sc, fwd, bwd = read_dump(fp)
            yy, xx = np.mgrid[0:H, 0:W]
            o = owners[g].astype(int)
            v = vel[g][o].astype(np.float32)  # per frame
            q = np.stack([xx, yy], -1).astype(np.float32) + 0.5
            sa = np.clip((q - v).astype(int), 0, [W - 1, H - 1]); sb = np.clip((q + v).astype(int), 0, [W - 1, H - 1])
            vis = (owners[g - 1][sa[..., 1], sa[..., 0]] == o) & (owners[g + 1][sb[..., 1], sb[..., 0]] == o)
            m = band & vis & (o >= 0)
            m[640:700, 980:1240] = False
            V = 2 * v
            c4 = candidates(fwd, blk, sc, q - v, V, 2) | candidates(bwd, blk, sc, q + v, -V, 2)
            c16 = candidates(fwd, blk, sc, q - v, V, 4) | candidates(bwd, blk, sc, q + v, -V, 4)
            va = owners[g - 1][sa[..., 1], sa[..., 0]] == o
            vb = owners[g + 1][sb[..., 1], sb[..., 0]] == o
            occ = band & ~vis
            occ[640:700, 980:1240] = False
            pa = occ & va & ~vb; pb_ = occ & vb & ~va
            ca = candidates(fwd, blk, sc, q - v, V, 2); cb = candidates(bwd, blk, sc, q + v, -V, 2)
            for name, mm in (("cand4", m & c4), ("cand16only", m & c16 & ~c4), ("missing", m & ~c16),
                             ("occ prevonly+cand", pa & ca), ("occ prevonly-nocand", pa & ~ca),
                             ("occ curonly+cand", pb_ & cb), ("occ curonly-nocand", pb_ & ~cb), ("occ neither", occ & ~va & ~vb)):
                s = cov.setdefault(name, [0.0, 0.0])
                s[0] += mm.sum(); s[1] += e[mm].sum()
    psnr = lambda se, n: 10 * np.log10(255 ** 2 / max(se / n, 1e-9))
    print(f"psnr {psnr(se_all, n_all):6.2f}  band {psnr(se_band, n_band):6.2f}  rest {psnr(se_all - se_band, n_all - n_band):6.2f}"
          f"  band px {100 * n_band / n_all:4.1f}%  band err share {100 * se_band / se_all:4.1f}%")
    if cov:
        nb = sum(v[0] for v in cov.values())
        for name, (n, se) in cov.items():
            print(f"   {name:20s} {100 * n / nb:5.1f}% of band px, {100 * se / se_all:5.1f}% of all error, psnr {psnr(se, max(n, 1)):6.2f}")


main()
