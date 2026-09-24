#!/usr/bin/env python3
"""Every output encoding and the upscaling path, headless.

Runs bdex_offline on the corpus's HUD sequence with each swapchain format the
engine packs for (RGBA8, BGRA8, sRGB, A2B10G10R10, RGBA16F) and with the
"game" rendering at 0.75 and 0.5 of the display size, and checks:
  - BGRA8 is byte-identical to RGBA8 (same data, other channel order);
  - 10-bit and FP16 match RGBA8 up to quantisation (>= 50 dB);
  - sRGB (blended in linear light) scores within 1 dB of RGBA8 against the
    ground truth;
  - upscaled output stays within 3 dB of native and well above frame
    repetition.
Usage: test_formats.py <build dir>. Needs a Vulkan device, no display.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def load(p):
    return np.asarray(Image.open(p).convert("RGB"), np.float64)


def psnr(a, b):
    m = ((a - b) ** 2).mean()
    return 99.0 if m == 0 else 10 * np.log10(255 ** 2 / m)


def main():
    build = Path(sys.argv[1])
    tool = build / "tools" / "offline" / "bdex_offline"
    if not tool.exists():
        print("bdex_offline not built, skipping")
        return 77
    env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET="balanced")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(tmp / "c")], check=True,
                       stdout=subprocess.DEVNULL)
        seq = tmp / "c" / "hud"
        runs = {"rgba8": [], "bgra8": ["--format", "bgra8"], "srgb": ["--format", "srgb"],
                "rgb10": ["--format", "rgb10"], "rgba16f": ["--format", "rgba16f"],
                "up0.75": ["--render-scale", "0.75"], "up0.5": ["--render-scale", "0.5"]}
        for name, args in runs.items():
            subprocess.run([str(tool), str(seq), str(tmp / name)] + args, check=True, env=env,
                           stderr=subprocess.DEVNULL)
        pairs = [line.split()[:2] for line in (tmp / "rgba8" / "manifest.txt").read_text().splitlines()]
        vs8, vsgt = {}, {}
        for name in runs:
            vs8[name] = np.mean([psnr(load(tmp / name / g), load(tmp / "rgba8" / g)) for g, _ in pairs])
            vsgt[name] = np.mean([psnr(load(tmp / name / g), load(gt)) for g, gt in pairs])
            print(f"{name:8s} vs rgba8 {vs8[name]:6.2f} dB, vs ground truth {vsgt[name]:6.2f} dB")
        dup = np.mean([psnr(load(seq / f"f{2 * int(g.split('_')[1]) - 2:04d}.ppm"), load(gt)) for g, gt in pairs])
        print(f"frame repetition vs ground truth {dup:.2f} dB")
    failures = []
    if vs8["bgra8"] < 99:
        failures.append("BGRA8 differs from RGBA8")
    for f in ("rgb10", "rgba16f"):
        if vs8[f] < 50:
            failures.append(f"{f} deviates from RGBA8 beyond quantisation ({vs8[f]:.1f} dB)")
    if abs(vsgt["srgb"] - vsgt["rgba8"]) > 1:
        failures.append(f"sRGB {vsgt['srgb']:.2f} dB vs RGBA8 {vsgt['rgba8']:.2f} dB")
    for f in ("up0.75", "up0.5"):
        if vsgt[f] < vsgt["rgba8"] - 3 or vsgt[f] < dup + 6:
            failures.append(f"{f}: {vsgt[f]:.2f} dB (native {vsgt['rgba8']:.2f}, repetition {dup:.2f})")
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
