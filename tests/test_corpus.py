#!/usr/bin/env python3
"""Quality regression on the evaluation corpus, headless (no display needed).

Generates the synthetic corpus (tools/make_corpus.py), plus every sequence
directory under $BDEX_CORPUS_DIR when set (recorded game sequences), runs
bdex_offline through tools/eval_sequence.py in interpolation and
extrapolation, for the defaults and for the candidate opt-in settings, and
fails when a PSNR or SSIM drops below its reference in
tests/corpus_reference.json by more than the tolerance. Missing references
are reported, not failed.

Usage: test_corpus.py <build dir> [--update]   (--update rewrites the references)
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REFERENCE = Path(__file__).resolve().parent / "corpus_reference.json"
# Software and hardware drivers round differently; a real regression is
# larger than this (the smallest shipped gains are a few tenths of a dB,
# but a broken default costs dB).
PSNR_TOL = 1.0
SSIM_TOL = 0.01
CONFIGS = {
    "default": "",
    # Opt-in settings that are candidates for the defaults (keep them from rotting).
    "candidate": "ZERO_BIAS_CONTRAST=0.1,SUBPIXEL=1,PIXEL_VECTORS=1,EXTRAPOLATE_SPLAT=1",
}


# What Config::autoConfigure turns on by itself on a discrete GPU.
STRONG_GPU_OFF = {f"BDEX_FG_{k}": v for k, v in {
    "PIXEL_VECTORS": "0", "INTERP_SPLAT": "0", "ROT_WEIGHT": "0", "TEMPORAL_PREDICTION": "0", "ZERO_BIAS": "0.002",
    "SCENE_CUT_LOCAL": "0", "EXTRAPOLATE_SPLAT": "0", "REVEAL_FILL": "0", "GLOBAL_MOTION": "0",
    "HUD_MASK": "0", "STATIC_SOFT": "0", "INTERP_ACCEL": "0", "FALSE_DETAIL": "0", "BORDER_PREDICT": "0", "BORDER_FILL": "0"}.items()}


def evaluate(build, seq, mode, variant, work):
    out = work / f"{seq.name}-{mode}.json"
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(seq), "--build", str(build), "--json", str(out)]
    if variant:
        cmd += ["--variant", variant]
    # Not the user's settings file; and the references are lavapipe's (CI),
    # where none of the discrete-GPU options turns itself on: off here too.
    env = dict(os.environ, BDEX_FG_MODE=mode, BDEX_FG_CONFIG="/dev/null", **STRONG_GPU_OFF)
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, env=env)
    return json.loads(out.read_text())


def main():
    build = Path(sys.argv[1])
    update = "--update" in sys.argv
    if not (build / "tools" / "offline" / "bdex_offline").exists():
        print("bdex_offline not built, skipping")
        return 77
    refs = json.loads(REFERENCE.read_text()) if REFERENCE.exists() else {}
    results, failures = {}, []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(tmp / "corpus")],
                       check=True, stdout=subprocess.DEVNULL)
        seqs = sorted(p for p in (tmp / "corpus").iterdir() if p.is_dir())
        if os.environ.get("BDEX_CORPUS_DIR"):
            seqs += sorted(p for p in Path(os.environ["BDEX_CORPUS_DIR"]).iterdir() if p.is_dir())
        for seq in seqs:
            for mode in ("interpolate", "extrapolate"):
                # One run per mode: the defaults and the candidate variant.
                rows = evaluate(build, seq, mode, CONFIGS["candidate"], tmp)
                for cfg, row in zip(CONFIGS, rows):
                    key = f"{seq.name}/{mode}/{cfg}"
                    results[key] = {"psnr": round(row["psnr"], 2), "ssim": round(row["ssim"], 4)}
                    ref = refs.get(key)
                    status = "no reference"
                    if ref:
                        dp, ds = row["psnr"] - ref["psnr"], row["ssim"] - ref["ssim"]
                        status = f"{dp:+.2f} dB, ssim {ds:+.4f}"
                        if dp < -PSNR_TOL or ds < -SSIM_TOL:
                            failures.append(f"{key}: {status}")
                            status += "  REGRESSION"
                    print(f"{key:<40} psnr {row['psnr']:6.2f}  ssim {row['ssim']:.4f}  ({status})")
    if update:
        refs.update(results)
        REFERENCE.write_text(json.dumps(refs, indent=1, sort_keys=True) + "\n")
        print(f"references written to {REFERENCE}")
        return 0
    if failures:
        print("quality regressions:\n  " + "\n  ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
