#!/usr/bin/env python3
"""The opt-in options still waiting for real content, scored on recorded games.

Each option below was measured on the synthetic corpus and the demo, and is
kept off until it is checked on what games actually show (docs/MEMOIRE.md
§9). Give this script recordings — the panel's « Enregistrer » button,
`bdex-framegen --record DIR`, or archives unpacked by tools/corpus_fetch.py —
and it scores every candidate against the defaults on every sequence, in
each mode the option acts in, with bdex_offline (headless, no window), then
gives a verdict per option:

  default?   mean PSNR gain >= +0.10 dB and no sequence below -0.10 dB
  no         mean gain below 0
  neutral    every sequence within 0.02 dB (the option changes nothing there)
  mixed      anything else (wins somewhere, loses elsewhere)

  tools/pending_decisions.py ~/.cache/bdex-framegen/recordings/* [--build build]
                             [--preset auto] [--only dedup,chroma_weight] [--json out.json]

The automatic preset (what a player gets on this GPU) is the reference by
default; the corpus test (tests/test_corpus.py) uses `balanced` on a
software renderer instead. A verdict is a starting point: read the per
sequence gains, and the flicker column (lower is better), before changing a
default — and then the bit-for-bit invariants (AGENTS.md §6).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# option settings, the modes they act in (i interpolate, e extrapolate), and why they wait.
CANDIDATES = [
    ("chroma_weight=1", "ie", "colour-only edges +12 dB; hatched stripes scrolling -1.2 dB"),
    ("dedup=1", "ie", "repeated frames (cutscenes, emulators) +0.9 to +7 dB, identical otherwise"),
    ("flow_temporal=1", "ie", "shimmer -5 %, demo +0.13 dB; costs 0.28 ms at 1440p"),
    ("occlusion_strength=1", "i", "revealed areas +0.8 dB on an iGPU, noise elsewhere"),
    ("static_guard=1", "i", "HUD over moving content; known defect on fast motion"),
    ("synth_bicubic=1", "ie", "sharper sub-pixel trajectories, ~0.6 ms at 1.7 Mpx"),
    ("subpixel_lk=2,synth_bicubic=1", "i", "sub-pixel motion pays only with the bicubic synthesis"),
    ("learned_flow=1", "ie", "trained on synthetic motion only: zoom +1 to +2.6 dB, rotation -0.13"),
    ("learned_correction=1", "i", "trained on the demo: +0.49 dB there, sub-pixel -1.73"),
    ("extrapolate_accel=1", "e", "accelerating camera +2.6 dB, pan -0.78, scroll -1.4"),
    ("hud_mask=4", "ie", "+0.20 dB on a HUD scene at render scale 0.75"),
    ("parallax_search=1", "ie", "railing scene x4 +0.32 dB, neutral elsewhere; ~1 ms of flow at 1440p"),
]
MODES = {"i": "interpolate", "e": "extrapolate"}


def evaluate(build, seq, mode, variants, preset, work):
    out = work / f"{seq.name}-{mode}.json"
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(seq), "--build", str(build), "--json", str(out)]
    for v in variants:
        cmd += ["--variant", v]
    env = dict(os.environ, BDEX_FG_MODE=MODES[mode], BDEX_FG_PRESET=preset, BDEX_FG_CONFIG="/dev/null",
               BDEX_FG_CACHE_DIR="")
    r = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        print(f"  {seq.name} [{MODES[mode]}]: evaluation failed: {r.stderr.strip()[-300:]}", file=sys.stderr)
        return None
    return {row["variant"]: row for row in json.loads(out.read_text())}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sequences", nargs="+", help="directories of consecutive .ppm / .png frames")
    ap.add_argument("--build", default=str(ROOT / "build"), help="build directory (default: ./build)")
    ap.add_argument("--preset", default="auto", help="reference preset (default: auto, as players get it)")
    ap.add_argument("--only", default="", help="comma-separated option names to keep (e.g. dedup,chroma_weight)")
    ap.add_argument("--json", help="also write every score to this file")
    a = ap.parse_args()

    if not (Path(a.build) / "tools" / "offline" / "bdex_offline").exists():
        sys.exit(f"{a.build}/tools/offline/bdex_offline not found: build the project first")
    only = {o.strip() for o in a.only.split(",") if o.strip()}
    cands = [c for c in CANDIDATES if not only or any(kv.split("=")[0] in only for kv in c[0].split(","))]
    seqs = [Path(s) for s in a.sequences if Path(s).is_dir()]
    if not seqs:
        sys.exit("no sequence directory given")

    # gains[option][(seq, mode)] = (psnr gain, flicker change)
    gains = {c[0]: {} for c in cands}
    with tempfile.TemporaryDirectory() as tmp:
        for seq in seqs:
            for mode in "ie":
                variants = [c[0] for c in cands if mode in c[1]]
                if not variants:
                    continue
                print(f"{seq.name} [{MODES[mode]}]: {len(variants)} variants", file=sys.stderr)
                res = evaluate(a.build, seq, mode, variants, a.preset, Path(tmp))
                if not res or "default" not in res:
                    continue
                ref = res["default"]
                for v in variants:
                    if v in res:
                        gains[v][(seq.name, mode)] = (res[v]["psnr"] - ref["psnr"], res[v]["flicker"] - ref["flicker"])

    print(f"\n{'option':<32} {'mean dB':>8} {'worst dB':>9} {'flicker':>8}  verdict    worst on")
    report = []
    for spec, _, why in cands:
        g = gains[spec]
        if not g:
            continue
        psnr = [v[0] for v in g.values()]
        mean = sum(psnr) / len(psnr)
        worst_key = min(g, key=lambda k: g[k][0])
        worst = g[worst_key][0]
        flick = sum(v[1] for v in g.values()) / len(g)
        best = max(psnr)
        verdict = ("neutral" if worst > -0.02 and best < 0.02 else "default?" if mean >= 0.10 and worst >= -0.10
                   else "no" if mean < 0 else "mixed")
        print(f"{spec:<32} {mean:+8.2f} {worst:+9.2f} {flick:+8.2f}  {verdict:<9}  {worst_key[0]} ({MODES[worst_key[1]]})")
        report.append({"option": spec, "why": why, "mean": mean, "worst": worst, "flicker": flick, "verdict": verdict,
                       "per_sequence": {f"{s}/{MODES[m]}": {"psnr": v[0], "flicker": v[1]} for (s, m), v in g.items()}})
    if a.json:
        Path(a.json).write_text(json.dumps({"preset": a.preset, "sequences": [s.name for s in seqs], "options": report},
                                           indent=1))


if __name__ == "__main__":
    main()
