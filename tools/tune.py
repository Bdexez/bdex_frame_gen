#!/usr/bin/env python3
"""Coordinate-descent tuning of the flow / synthesis knobs on recorded sequences.

For each parameter in turn, every value of its grid is evaluated (all other
parameters at their current best) on every sequence with bdex_offline, and
the value with the best objective is kept; passes repeat until nothing
changes. The objective is the mean over sequences of the PSNR gain against
the defaults, so each sequence counts equally whatever its absolute dB; the
worst per-sequence gain is shown next to it, because a setting that wins on
average but breaks one kind of content is not a default candidate.

  tools/tune.py SEQ_DIR [SEQ_DIR...] [--mode extrapolate] [--passes 2]
                [--param zero_bias_contrast=0,0.05,0.1,0.2] [--min-worst -0.2]

Without --param, a built-in grid covers the knobs measured so far. --exclude
applies to every sequence (e.g. the demo's frame counter). Candidates whose
worst per-sequence gain is below --min-worst are rejected.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DEFAULT_GRID = {
    "zero_bias_contrast": ["0", "0.05", "0.1", "0.2"],
    "smoothness": ["0.002", "0.004", "0.008"],
    "subpixel": ["0", "1"],
    "pixel_vectors": ["0", "1"],
    "static_guard": ["0", "1"],
    "occlusion_strength": ["0", "1"],
    "temporal_prediction": ["0", "1"],
}
DEFAULTS = {"zero_bias_contrast": "0", "smoothness": "0.004", "subpixel": "0", "pixel_vectors": "0",
            "static_guard": "0", "occlusion_strength": "0", "temporal_prediction": "0",
            "extrapolate_splat": "0", "zero_bias": "0.002", "rot_weight": "0", "grad_weight": "0"}


def spec(cfg):
    return ",".join(f"{k.upper()}={v}" for k, v in sorted(cfg.items()))


def evaluate(args, seq, configs, work):
    """PSNR of each config on one sequence (one bdex_offline run per config)."""
    out = work / "r.json"
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(seq), "--build", args.build,
           "--json", str(out), "--multiplier", str(args.multiplier)]
    for x in args.exclude:
        cmd += ["--exclude", x]
    for c in configs:
        cmd += ["--variant", spec(c)]
    env = dict(os.environ, BDEX_FG_MODE=args.mode)
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    rows = json.loads(out.read_text())
    return rows[0]["psnr"], [r["psnr"] for r in rows[1:]]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sequences", nargs="+")
    ap.add_argument("--build", default=str(ROOT / "build"))
    ap.add_argument("--mode", default="interpolate", choices=("interpolate", "extrapolate"))
    ap.add_argument("--multiplier", type=int, default=2)
    ap.add_argument("--param", action="append", default=[], help="name=v1,v2,... (replaces the built-in grid)")
    ap.add_argument("--passes", type=int, default=2)
    ap.add_argument("--min-worst", type=float, default=-0.2, help="reject a value whose worst sequence loses more (dB)")
    ap.add_argument("--exclude", action="append", default=[])
    args = ap.parse_args()

    grid = DEFAULT_GRID
    if args.param:
        grid = {}
        for p in args.param:
            k, _, v = p.partition("=")
            grid[k.strip().lower()] = [x.strip() for x in v.split(",")]
    if args.mode == "extrapolate" and not args.param:
        grid = {**{k: v for k, v in grid.items() if k != "static_guard"}, "extrapolate_splat": ["0", "1"]}
    best = {k: DEFAULTS.get(k, grid[k][0]) for k in grid}
    seqs = [Path(s) for s in args.sequences]

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        best_mean = 0.0
        for n in range(args.passes):
            changed = False
            for name, values in grid.items():
                configs = [{**best, name: v} for v in values]
                gains = [[] for _ in configs]
                for seq in seqs:
                    base, psnrs = evaluate(args, seq, configs, work)
                    for i, p in enumerate(psnrs):
                        gains[i].append(p - base)
                print(f"pass {n + 1}, {name}:")
                choice, choice_mean = None, None
                for v, g in zip(values, gains):
                    mean, worst = sum(g) / len(g), min(g)
                    ok = worst >= args.min_worst
                    print(f"  {name}={v:<8} mean {mean:+.2f} dB  worst {worst:+.2f} dB{'' if ok else '  (rejected)'}")
                    if ok and (choice_mean is None or mean > choice_mean + 0.02):
                        choice, choice_mean = v, mean
                if choice is not None and choice != best[name]:
                    best[name] = choice
                    changed = True
                if choice_mean is not None:
                    best_mean = choice_mean
            if not changed:
                break
    print(f"\nbest ({args.mode}): {spec(best)}   mean gain {best_mean:+.2f} dB vs defaults")


if __name__ == "__main__":
    main()
