#!/usr/bin/env python3
"""Online benchmark on a recorded sequence: the sequence is played as a game
(bdex_demo --replay: every m-th frame presented, at a steady rate) through
the whole online path — the layer, its pacing, a real swapchain — and the
frames actually presented are scored against the frames left out.

    tools/replay_bench.py SEQ [--multiplier 2] [--fps 30] [--presents N]
                          [--demo build/demo/bdex_demo] [--layer-dir build/layer]
                          [--env KEY=VALUE ...] [--other-layer NAME] [--json FILE]

SEQ is a directory of consecutive fNNNN.ppm frames (« Enregistrer » in the
panel, bdex-framegen --record, tools/make_corpus.py).

Our layer generates the frames: it dumps everything it presents, generated
frames with their time t, and each is compared with recorded frame
(p - 1 + t) * m of its present p. Another frame generation layer can be
measured on the same footing (--other-layer NAME, its own settings with
--env): it is put above ours (VK_INSTANCE_LAYERS=NAME:ours), ours generates
nothing and dumps what the other one presents; the frames identical to a
recorded one are the real ones (anchors), the ones between them are spread
evenly over the recorded frames in between.

Reported per run: PSNR of the generated frames (mean, worst 10 %), against
two references on the same frames — repeating the previous real frame and
cross-fading the two real frames — and how many frames were scored.
Needs a display (an X server, Xvfb, or headless gamescope) and numpy.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if not m:
        raise ValueError(f"{path}: not a binary PPM")
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data, np.uint8, w * h * 3, m.end()).reshape(h, w, 3)


def psnr(a, b):
    mse = np.mean((a.astype(np.float32) - b.astype(np.float32)) ** 2)
    return 99.0 if mse < 1e-10 else float(10 * np.log10(255.0 ** 2 / mse))


def layer_env(layer_dir):
    """The environment that makes the loader use the layer of `layer_dir`
    (tools/layer_env.sh: its manifest in a private XDG_DATA_HOME)."""
    out = subprocess.run(["bash", "-c", f'. "{ROOT}/tools/layer_env.sh"; bdex_use_layer "{layer_dir}"; env -0'],
                         capture_output=True, check=True).stdout
    env = dict(kv.split("=", 1) for kv in out.decode().split("\0") if "=" in kv)
    return env


def run(args, seq, frames, size, dump, other):
    env = layer_env(args.layer_dir)
    env.update({
        "BDEX_FG": "1", "BDEX_FG_CONFIG": "/dev/null", "BDEX_FG_LOG": "2",
        # t exactly i/m (no display-time placement), every presented frame dumped.
        "BDEX_FG_PRESENT_FEEDBACK": "0", "BDEX_FG_FRAME_TIMING": "0",
        "BDEX_FG_DUMP": str(dump), "BDEX_FG_DUMP_FRAMES": str(args.presents * args.multiplier * 2 + 8),
        "BDEX_FG_MULTIPLIER": "1" if other else str(args.multiplier),
    })
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    if other:
        env["VK_INSTANCE_LAYERS"] = f"{other}:VK_LAYER_BDEX_framegen"
    cmd = [args.demo, "--replay", str(seq), "--replay-step", str(args.multiplier), "--fps", str(args.fps),
           "--size", f"{size[0]}x{size[1]}", "--frames", str(args.presents), "--ignore-resize", "--duration", "120"]
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + r.stderr[-2000:])
        raise SystemExit(f"bdex_demo failed ({r.returncode})")
    return r.stdout + r.stderr


def dumps_in_order(dump):
    """The dumps of the swapchain that presented the most, in presentation
    order: (path, generated, t)."""
    rx = re.compile(r"sc(\d+)_\d+x\d+_(\d+)_(gen|real)_t([0-9.]+)\.ppm$")
    by_sc = {}
    for p in dump.iterdir():
        m = rx.match(p.name)
        if m:
            by_sc.setdefault(m.group(1), []).append((int(m.group(2)), p, m.group(3) == "gen", float(m.group(4))))
    if not by_sc:
        return []
    best = max(by_sc.values(), key=len)
    return [(p, g, t) for _, p, g, t in sorted(best, key=lambda x: x[0])]


def score_ours(dumps, frames, m):
    """Ours: real dump r is present r (recorded frame r*m); a frame generated
    at t < 1 belongs to the next real frame's present, at t > 1 to the
    previous one's (extrapolation, hybrid). Truth: (present - 1 + t) * m."""
    reals = [i for i, (_, g, _) in enumerate(dumps) if not g]
    out = []
    for i, (path, gen, t) in enumerate(dumps):
        if not gen:
            continue
        if t < 1.0:
            present = next((r for r, idx in enumerate(reals) if idx > i), None)
        else:
            present = max((r for r, idx in enumerate(reals) if idx < i), default=None)
        if present is None or present < 1:
            continue
        pos = (present - 1 + t) * m
        if abs(pos - round(pos)) > 0.05:
            continue  # (not on a recorded frame: unscorable)
        out.append((path, int(round(pos)), (present - 1) * m, present * m))
    return out


def score_other(dumps, frames, m, ref):
    """Another layer's frames, all dumped as real: the anchors are the ones
    equal to a recorded frame k*m (the game's own), the others spread evenly
    between two consecutive anchors."""
    out, anchors = [], []
    k = 0
    for i, (path, _, _) in enumerate(dumps):
        img = read_ppm(path)
        for cand in (k, k + 1, k + 2):
            if cand * m < len(frames) and psnr(img, ref(cand * m)) > 45.0:
                anchors.append((i, cand))
                k = cand
                break
    score_other.anchors = len(anchors)
    for (i0, k0), (i1, k1) in zip(anchors, anchors[1:]):
        if k1 != k0 + 1 or i1 - i0 < 2:
            continue
        between = i1 - i0 - 1
        for j in range(1, between + 1):
            pos = (k0 + j / (between + 1)) * m
            if abs(pos - round(pos)) <= 0.05:
                out.append((dumps[i0 + j][0], int(round(pos)), k0 * m, k1 * m))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("seq", type=Path)
    ap.add_argument("--multiplier", type=int, default=2)
    ap.add_argument("--fps", type=float, default=30.0, help="the replayed game's frame rate")
    ap.add_argument("--presents", type=int, default=0, help="presents to run (default: as many as the sequence has)")
    ap.add_argument("--demo", default=str(ROOT / "build" / "demo" / "bdex_demo"))
    ap.add_argument("--layer-dir", default=str(ROOT / "build" / "layer"))
    ap.add_argument("--env", action="append", default=[], help="KEY=VALUE for the run (our settings, or the other layer's)")
    ap.add_argument("--other-layer", default="", help="another frame generation layer to measure, above ours")
    ap.add_argument("--json", default="")
    ap.add_argument("--keep", action="store_true", help="keep the dumps (printed)")
    a = ap.parse_args()
    frames = sorted(p for p in a.seq.iterdir() if p.suffix == ".ppm")
    if len(frames) < 2 * a.multiplier + 1:
        raise SystemExit("too few frames in the sequence")
    if not a.presents:
        a.presents = (len(frames) - 1) // a.multiplier + 1
    first = read_ppm(frames[0])
    size = (first.shape[1], first.shape[0])
    cache = {}

    def ref(i):
        if i not in cache:
            cache[i] = read_ppm(frames[min(i, len(frames) - 1)])
        return cache[i]

    dump = Path(tempfile.mkdtemp(prefix="bdex-replay-"))
    try:
        log = run(a, a.seq, frames, size, dump, a.other_layer)
        dumps = dumps_in_order(dump)
        if not dumps:
            sys.stderr.write(log[-3000:])
            raise SystemExit("nothing was dumped (is the layer loaded? BDEX_FG_LOG output above)")
        if read_ppm(dumps[0][0]).shape != first.shape:
            raise SystemExit(f"the window was not given the sequence's size ({size[0]}x{size[1]}): "
                             "run under Xvfb or headless gamescope, not a tiling desktop")
        scored = score_other(dumps, frames, a.multiplier, ref) if a.other_layer else score_ours(dumps, frames, a.multiplier)
        rows = []
        for path, gt, prev, cur in scored:
            if gt >= len(frames) or cur >= len(frames):
                continue
            img, truth = read_ppm(path), ref(gt)
            if img.shape != truth.shape:
                continue
            fade = ((ref(prev).astype(np.float32) + ref(cur).astype(np.float32)) / 2).round().astype(np.uint8)
            rows.append((psnr(img, truth), psnr(ref(prev), truth), psnr(fade, truth)))
        if not rows:
            extra = f", {score_other.anchors} identical to a recorded frame" if a.other_layer else ""
            raise SystemExit(f"no generated frame could be scored ({len(dumps)} frames presented{extra})")
        g = np.array(rows)
        worst = np.sort(g[:, 0])[: max(1, len(g) // 10)].mean()
        result = {
            "sequence": a.seq.name, "layer": a.other_layer or "bdex-framegen", "multiplier": a.multiplier,
            "presented": len(dumps), "scored": len(rows), "expected": (a.presents - 1) * (a.multiplier - 1),
            "psnr_mean": round(float(g[:, 0].mean()), 3), "psnr_worst10": round(float(worst), 3),
            "repeat_psnr": round(float(g[:, 1].mean()), 3), "fade_psnr": round(float(g[:, 2].mean()), 3),
        }
        print(f"{result['sequence']} x{a.multiplier} ({result['layer']}): {result['psnr_mean']:.2f} dB "
              f"(worst 10 % {result['psnr_worst10']:.2f}), repeat {result['repeat_psnr']:.2f}, "
              f"fade {result['fade_psnr']:.2f} — {len(rows)}/{result['expected']} generated frames scored, "
              f"{len(dumps)} presented")
        if a.json:
            Path(a.json).write_text(json.dumps(result, indent=1) + "\n")
        if a.keep:
            print(f"dumps kept in {dump}")
    finally:
        if not a.keep:
            shutil.rmtree(dump, ignore_errors=True)


if __name__ == "__main__":
    main()
