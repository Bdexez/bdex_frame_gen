#!/usr/bin/env python3
"""Every output encoding and the upscaling path, headless.

Runs bdex_offline on the corpus's HUD sequence with each swapchain format the
engine packs for (RGBA8, BGRA8, sRGB, A2B10G10R10, RGBA16F) and with the
"game" rendering at 0.75 and 0.5 of the display size, and checks:
  - BGRA8 is byte-identical to RGBA8 (same data, other channel order);
  - 10-bit and FP16 match RGBA8 up to quantisation (>= 50 dB);
  - sRGB (blended in linear light) scores within 1 dB of RGBA8 against the
    ground truth;
  - HDR swapchains — scRGB (FP16, 1 = 80 nits) and HDR10 (PQ, 10-bit), the
    SDR frames converted with white at 203 nits — score within 0.5 dB of the
    sRGB format: the matching brings HDR values to the same perceptual scale
    (without it scRGB costs are 2.5x too large, PQ mid-tones too flat);
  - upscaled output stays within 3 dB of native and well above frame
    repetition;
  - border_fill (content entering at the edges in a camera move, which
    this pan has) is no worse against the ground truth in any of them. The
    checks above run without it: at the edges both the flow (off-screen
    matching) and its decision depend on each format's values, and its gain
    (about 2 dB native here, 1 dB upscaled) would move their comparisons.
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
    env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET="balanced", BDEX_FG_BORDER_FILL="0", BDEX_FG_STILL_OCCLUDER="0",
               BDEX_FG_MOSAIC_BLUR="0", BDEX_FG_HUD_LAYER="0")  # (packed formats only: the encodings compared without them)
    envFill = dict(env, BDEX_FG_BORDER_FILL="1")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(tmp / "c")], check=True,
                       stdout=subprocess.DEVNULL)
        seq = tmp / "c" / "hud"
        runs = {"rgba8": [], "bgra8": ["--format", "bgra8"], "srgb": ["--format", "srgb"],
                "rgb10": ["--format", "rgb10"], "rgba16f": ["--format", "rgba16f"],
                "scrgb": ["--format", "rgba16f", "--colorspace", "scrgb"],
                "hdr10": ["--format", "rgb10", "--colorspace", "hdr10"],
                "up0.75": ["--render-scale", "0.75"], "up0.5": ["--render-scale", "0.5"]}
        for name, args in runs.items():
            subprocess.run([str(tool), str(seq), str(tmp / name)] + args, check=True, env=env,
                           stderr=subprocess.DEVNULL)
            subprocess.run([str(tool), str(seq), str(tmp / (name + "-fill"))] + args, check=True, env=envFill,
                           stderr=subprocess.DEVNULL)
        pairs = [line.split()[:2] for line in (tmp / "rgba8" / "manifest.txt").read_text().splitlines()]
        vs8, vsgt, fill = {}, {}, {}
        for name in runs:
            vs8[name] = np.mean([psnr(load(tmp / name / g), load(tmp / "rgba8" / g)) for g, _ in pairs])
            vsgt[name] = np.mean([psnr(load(tmp / name / g), load(gt)) for g, gt in pairs])
            fill[name] = np.mean([psnr(load(tmp / (name + "-fill") / g), load(gt)) for g, gt in pairs])
            print(f"{name:8s} vs rgba8 {vs8[name]:6.2f} dB, vs ground truth {vsgt[name]:6.2f} dB, "
                  f"with border_fill {fill[name]:6.2f} dB")
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
    for f in ("scrgb", "hdr10"):
        if abs(vsgt[f] - vsgt["srgb"]) > 0.5:
            failures.append(f"{f} {vsgt[f]:.2f} dB vs sRGB {vsgt['srgb']:.2f} dB")
    for f in ("up0.75", "up0.5"):
        if vsgt[f] < vsgt["rgba8"] - 3 or vsgt[f] < dup + 6:
            failures.append(f"{f}: {vsgt[f]:.2f} dB (native {vsgt['rgba8']:.2f}, repetition {dup:.2f})")
    for f in runs:
        if fill[f] < vsgt[f] - 0.05:
            failures.append(f"border_fill: {f} {fill[f]:.2f} dB, without {vsgt[f]:.2f} dB")
    if failures:
        print("FAILED:\n  " + "\n  ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
