#!/usr/bin/env python3
"""depth_oracle.py SEQ OUT [MAXFRAMES] [VARIANTS]: what a depth buffer (or frame
N-2) would give the synthesis at occlusions, on a make_sequence.py sequence.

    bdex_offline SEQ OUT --multiplier 2 --dump-flow OUT/flow
    depth_oracle.py SEQ OUT 6 n2,depth,depthv

Post-corrects the engine's generated frames (t = 0.5) where no candidate
trajectory explains the pixel (best 2x2-quadrant error of both ends > 0.04),
and scores the result against the truth: the whole frame, the +-6 px band
around objects (score.py's), and the change of squared error by truth class of
the pixels it touched (share of the engine's total error, negative = gain):
visible in both inputs, in prev only (covered by cur), in cur only (uncovered).
Candidates at q: the engine's own block vectors, 12 forward texels and 12
backward texels around q (the 2x2 and their edge neighbours).

  n2      frame N-2, a plain copy of the image: the candidate whose prev-side
          sample N-2 confirms (P(a) ~ P2(a - V), textured) gives the one-sided
          sample P(q - tV). Measured 01/11: it cannot tell covered from
          uncovered pixels (content that moved coherently in the past is not
          necessarily still visible at t): still -0.95 dB, the uncovered
          pixels +27 % of the error.
  depth   the truth's z-order as depth of prev and cur (what the game's depth
          buffer gives): a z-buffer over the candidates — two-sided where both
          ends are the same surface, one-sided where the other end shows
          something nearer — the nearest wins (ties: two-sided, then error).
  depthv  depth, and a vector is only used for the layer of the block it was
          measured on (the depth at its texel centre matches the end it
          carries). Measured 01/11 (15 frames): still +1.54 dB, pan +0.82,
          fast +0.48, lowcon +0.17, both occlusion classes down everywhere.
  seg     no depth, an exact per-pixel segmentation (the truth's owners) for
          the layer of each vector and end, the faster layer in front — what
          a two-frame motion segmentation could at best give: still +1.54,
          pan +0.58, fast -0.44, lowcon -0.11 (the guessed order fails once
          objects cross each other or the background is faster).
  quad    no new input: the candidate with the lowest quadrant error
          (re-selection only): -0.16 to +0.06 dB.
Only the pixels within 12 px of an object are evaluated (elsewhere the engine
is at 48-62 dB): the truth limits the work, the rules never read it.
"""
import sys
from pathlib import Path

import numpy as np

R = 6
HUD = (slice(640, 700), slice(980, 1240))
LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)
QUADS = [[(dx + ox, dy + oy) for dy in (0, 1) for dx in (0, 1)] for ox in (-1, 0) for oy in (-1, 0)]


def ppm(p):
    d = open(p, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3], np.uint8).reshape(h, w, 3).astype(np.float32) / 255.0


def read_dump(path):
    d = open(path, "rb").read()
    _, w, h, block, lw, lh = np.frombuffer(d[:24], np.uint32)
    scale = float(np.frombuffer(d[24:28], np.float32)[0])
    o = 28
    fwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2); o += w * h * 8
    bwd = np.frombuffer(d[o:o + w * h * 8], np.float32).reshape(h, w, 2)
    return int(block), scale, fwd * scale, bwd * scale  # vectors in display px


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r: out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def boundary(own):
    b = np.zeros(own.shape, bool)
    for ax in (0, 1):
        d = np.diff(own, axis=ax) != 0
        obj = np.maximum(np.take(own, range(own.shape[ax] - 1), ax), np.take(own, range(1, own.shape[ax]), ax)) >= 2
        e = d & obj
        if ax == 0: b[:-1] |= e; b[1:] |= e
        else: b[:, :-1] |= e; b[:, 1:] |= e
    return b


def bilinear(img, x, y):
    """img HxWxC at pixel-centre coordinates (+0.5 convention), clamped to the edge."""
    H, W = img.shape[:2]
    fx = np.clip(x - 0.5, 0, W - 1.001); fy = np.clip(y - 0.5, 0, H - 1.001)
    x0 = fx.astype(np.int32); y0 = fy.astype(np.int32)
    ax = (fx - x0)[..., None]; ay = (fy - y0)[..., None]
    return ((img[y0, x0] * (1 - ax) + img[y0, x0 + 1] * ax) * (1 - ay) +
            (img[y0 + 1, x0] * (1 - ax) + img[y0 + 1, x0 + 1] * ax) * ay)


def nearest(img, x, y):
    H, W = img.shape[:2]
    return img[np.clip(y.astype(np.int32), 0, H - 1), np.clip(x.astype(np.int32), 0, W - 1)]


def err(a, b):
    return np.abs(a - b) @ LUMA


def quad_err(A, ax, ay, B, bx, by):
    """Best of the four 2x2 windows around the pixel (one centred on a
    boundary holds the other layer)."""
    cache, best = {}, None
    for q in QUADS:
        s = 0.0
        for d in q:
            if d not in cache: cache[d] = err(bilinear(A, ax + d[0], ay + d[1]), bilinear(B, bx + d[0], by + d[1]))
            s = s + cache[d]
        best = s / 4 if best is None else np.minimum(best, s / 4)
    return best


def texture(A, ax, ay):
    v = np.stack([bilinear(A, ax + dx, ay + dy) @ LUMA for dx in (-1, 0, 1) for dy in (-1, 0, 1)])
    return np.abs(v - v.mean(0)).mean(0)


def main():
    seq, out = Path(sys.argv[1]), Path(sys.argv[2])
    maxf = int(sys.argv[3]) if len(sys.argv) > 3 else 99
    variants = sys.argv[4].split(",") if len(sys.argv) > 4 else ["n2", "depth", "depthv", "quad"]
    need3 = "n2" in variants
    T = np.load(seq / "truth.npz"); owners, vel = T["owners"], T["vel"]
    H, W = owners.shape[1:]
    zmap = np.array([100.0, 0.0] + [50.0 - i for i in range(7)], np.float32)  # world, HUD, objects painted in order
    t, eps, th = 0.5, 0.5, 0.04
    tot = {v: [0.0, 0.0] for v in ["engine"] + variants}
    npx = [0, 0]
    cls = {v: np.zeros((3, 2)) for v in variants}
    done = 0
    for line in open(out / "manifest.txt"):
        gen, gt, _, _, _ = line.split()
        g = int(Path(gt).stem[1:])
        if g < 3: continue  # N-2 needed
        if done >= maxf: break
        done += 1
        blk, sc, fwd, bwd = read_dump(out / "flow" / f"flow_{(g + 1) // 2:04d}.bin")
        E, G = ppm(out / gen), ppm(seq / f"f{g:04d}.ppm")
        P, C, P2 = (ppm(seq / f"f{g + d:04d}.ppm") for d in (-1, 1, -3))
        Dp, Dc = zmap[owners[g - 1]], zmap[owners[g + 1]]
        bnd = boundary(owners[g])
        band = dilate(bnd, R); band[HUD] = False
        work = dilate(bnd, 12); work[HUD] = False
        mask = np.ones((H, W), bool); mask[HUD] = False
        o = owners[g].astype(int); v = vel[g][o].astype(np.float32)
        yy, xx = np.mgrid[0:H, 0:W]
        q = np.stack([xx, yy], -1).astype(np.float32) + 0.5
        sa = np.clip((q - v).astype(int), 0, [W - 1, H - 1]); sb = np.clip((q + v).astype(int), 0, [W - 1, H - 1])
        va = owners[g - 1][sa[..., 1], sa[..., 0]] == o; vb = owners[g + 1][sb[..., 1], sb[..., 0]] == o
        klass = np.where(va & vb, 0, np.where(va, 1, np.where(vb, 2, 0)))
        iy, ix = np.nonzero(work)
        px, py = xx[iy, ix] + 0.5, yy[iy, ix] + 0.5
        tex = blk * sc
        tx = np.floor(px / tex - 0.5).astype(np.int32); ty = np.floor(py / tex - 0.5).astype(np.int32)
        fh, fw = fwd.shape[:2]
        cands, zt, ot = [], [], []
        for field, sign, D, Ow in ((fwd, 1.0, Dp, owners[g - 1]), (bwd, -1.0, Dc, owners[g + 1])):
            for j in (-1, 0, 1, 2):
                for i in (-1, 0, 1, 2):
                    if i in (-1, 2) and j in (-1, 2): continue
                    cj, ci = np.clip(ty + j, 0, fh - 1), np.clip(tx + i, 0, fw - 1)
                    cands.append(sign * field[cj, ci])
                    zt.append(nearest(D, (ci + 0.5) * tex, (cj + 0.5) * tex))
                    ot.append(nearest(Ow, (ci + 0.5) * tex, (cj + 0.5) * tex))
        zt, ot = np.stack(zt), np.stack(ot).astype(int)
        spd = np.linalg.norm(vel[g].astype(np.float32), axis=1)  # speed of each owner (seg)
        n, K = len(cands), len(iy)
        e2, e3, ta, za, zb = (np.zeros((n, K), np.float32) for _ in range(5))
        oa, ob = np.zeros((n, K), int), np.zeros((n, K), int)
        sp, sc_ = np.zeros((n, K, 3), np.float32), np.zeros((n, K, 3), np.float32)
        for c, V in enumerate(cands):
            ax_, ay_ = px - t * V[:, 0], py - t * V[:, 1]
            bx_, by_ = px + (1 - t) * V[:, 0], py + (1 - t) * V[:, 1]
            e2[c] = quad_err(P, ax_, ay_, C, bx_, by_)
            if need3:
                e3[c] = quad_err(P, ax_, ay_, P2, ax_ - V[:, 0], ay_ - V[:, 1])
                ta[c] = texture(P, ax_, ay_)
            za[c], zb[c] = nearest(Dp, ax_, ay_), nearest(Dc, bx_, by_)
            oa[c], ob[c] = nearest(owners[g - 1], ax_, ay_), nearest(owners[g + 1], bx_, by_)
            sp[c], sc_[c] = bilinear(P, ax_, ay_), bilinear(C, bx_, by_)
        unexpl = e2.min(0) > th
        take = lambda arr, b: np.take_along_axis(arr, b[None], 0)[0]
        take3 = lambda arr, b: np.take_along_axis(arr, b[None, :, None], 0)[0]
        eE = ((E - G) ** 2).mean(-1) * 255.0 ** 2
        tot["engine"][0] += eE[mask].sum(); tot["engine"][1] += eE[band].sum()
        npx[0] += mask.sum(); npx[1] += band.sum()
        gk, ek, kk = G[iy, ix], eE[iy, ix], klass[iy, ix]
        for name in variants:
            if name == "quad":
                b = e2.argmin(0)
                pick, sel = take3(sp, b) * (1 - t) + take3(sc_, b) * t, unexpl
            elif name == "n2":
                score = np.where(ta > 0.01, e3, 9.0)
                b = score.argmin(0)
                pick, sel = take3(sp, b), unexpl & (take(score, b) < 0.03)
            elif name == "seg":
                # exact per-pixel segmentation (owners), no depth: the faster layer is in front
                same = (oa == ob) & (oa == ot) & (e2 < th)
                onep = (oa != ob) & (oa == ot) & (spd[ob] > spd[oa])
                onec = (oa != ob) & (ob == ot) & (spd[oa] > spd[ob])
                z = -np.where(onec, spd[ob], spd[oa])
                depth = np.where(same | onep | onec, z, 1e9)
                b = (depth * 10.0 + np.where(same, 0.0, 1.0) + e2).argmin(0)
                bs, bp = take(same, b), take(onep, b)
                p_, c_ = take3(sp, b), take3(sc_, b)
                pick = np.where(bs[:, None], p_ * (1 - t) + c_ * t, np.where(bp[:, None], p_, c_))
                sel = unexpl & (take(depth, b) < 1e8)
            else:
                same = (np.abs(za - zb) < eps) & (e2 < th)
                onep, onec = zb < za - eps, za < zb - eps  # the other end shows something nearer
                if name == "depthv":
                    same &= np.abs(zt - za) < eps
                    onep &= np.abs(zt - za) < eps
                    onec &= np.abs(zt - zb) < eps
                depth = np.where(same | onep, za, np.where(onec, zb, 1e9))
                b = (depth * 10.0 + np.where(same, 0.0, 1.0) + e2).argmin(0)
                bs, bp = take(same, b), take(onep, b)
                p_, c_ = take3(sp, b), take3(sc_, b)
                pick = np.where(bs[:, None], p_ * (1 - t) + c_ * t, np.where(bp[:, None], p_, c_))
                sel = unexpl & (take(depth, b) < 1e8)
            d = np.where(sel, ((pick - gk) ** 2).mean(-1) * 255.0 ** 2 - ek, 0.0)
            full = eE.copy(); full[iy, ix] += d
            tot[name][0] += full[mask].sum(); tot[name][1] += full[band].sum()
            for c in range(3):
                m = sel & (kk == c)
                cls[name][c] += [m.sum(), d[m].sum()]
    ps = lambda se, nn: 10 * np.log10(255 ** 2 / max(se / nn, 1e-9))
    b0 = tot["engine"]
    print(f"{seq.name:7s} engine   all {ps(b0[0], npx[0]):6.2f} band {ps(b0[1], npx[1]):6.2f}  ({done} frames)")
    for name in variants:
        a = tot[name]
        parts = "  ".join(f"{nm} {int(cls[name][c, 0] / max(done, 1)):6d}px {100 * cls[name][c, 1] / b0[0]:+6.1f}%"
                          for c, nm in enumerate(("both", "prev-only", "cur-only")))
        print(f"{seq.name:7s} {name:8s} all {ps(a[0], npx[0]) - ps(b0[0], npx[0]):+6.2f} "
              f"band {ps(a[1], npx[1]) - ps(b0[1], npx[1]):+6.2f} | {parts}")


if __name__ == "__main__":
    main()
