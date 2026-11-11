#!/usr/bin/env python3
"""bdex_offline end to end, without a display: a textured pattern translating
at constant speed must be interpolated (x2) and extrapolated (x2) far better
than repeating the nearest input frame. On an irregular "game" — uneven frame
times (--jitter), or every image presented twice (--dup 2) — frame_timing and
dedup must beat the fixed timing, in both modes; and on a menu opening over
part of the screen, scene_cut_local must beat the frame-wide scene-cut test;
on a swinging camera, extrapolate_accel must beat constant velocity; on
colours of equal brightness, chroma_weight must beat luma-only matching.
Usage: test_offline.py <build dir>"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def frames(dir_, n=13, w=320, h=192, speed=3):
    rng = np.random.default_rng(7)
    # Smooth texture (block noise, upsampled) wider than the view, panned.
    tex = rng.integers(0, 256, (h // 8 + 2, (w + n * speed) // 8 + 2, 3), dtype=np.uint8)
    tex = np.asarray(Image.fromarray(tex).resize((tex.shape[1] * 8, tex.shape[0] * 8), Image.BILINEAR))
    for i in range(n):
        Image.fromarray(tex[:h, i * speed:i * speed + w]).save(dir_ / f"f{i:04d}.ppm")


def evaluate(build, frames_dir, extra_env=None, args=()):
    cmd = [sys.executable, str(ROOT / "tools" / "eval_sequence.py"), str(frames_dir), "--build", str(build), *args]
    # (BDEX_FG_CONFIG: never the user's own settings file)
    env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", **(extra_env or {}))
    out = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, env=env).stdout
    print(out)
    lines = out.splitlines()
    col = {name: i for i, name in enumerate(lines[0].split())}
    rows = [line.split() for line in lines[1:]]
    if len(rows) > 1:  # variants: psnr of each
        return [float(r[col["psnr"]]) for r in rows]
    row = rows[0]
    return float(row[col["psnr"]]), float(row[col["dup"]]), int(row[col["frames"]])


def main():
    build = Path(sys.argv[1])
    if not (build / "tools" / "offline" / "bdex_offline").exists():
        print("bdex_offline not built, skipping")
        return 77
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        frames(d)
        for mode in ("interpolate", "extrapolate"):
            psnr, dup, n = evaluate(build, d, {"BDEX_FG_MODE": mode})
            if n < 5 or not psnr > dup + 3.0:
                print(f"{mode}: generation {psnr:.2f} dB is not clearly better than repetition {dup:.2f} dB ({n} frames)")
                return 1
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        frames(d, n=81, speed=4)
        for mode in ("interpolate", "extrapolate"):
            # --hud-counter: a HUD redrawn on every present over the repeating
            # world (the repeat is then partial, detected per tile).
            # The interval filter now follows confirmed cadence changes. Use
            # larger spikes so the content-clock test still needs resampling.
            for offline, option in (("--jitter 0.6 --seed 2", "FRAME_TIMING=1"), ("--dup 2", "DEDUP=1"),
                                    ("--dup 2 --hud-counter", "DEDUP=1")):
                base, opt = evaluate(build, d, {"BDEX_FG_MODE": mode},
                                     ["--offline-args", offline, "--variant", option,
                                      "--exclude", "8,8,72,24"])
                if not opt > base + 0.3:
                    print(f"{mode} {offline}: {option} {opt:.2f} dB does not beat the fixed timing {base:.2f} dB")
                    return 1
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        frames(d, n=81, w=384, h=256, speed=4)
        # Constant velocity sampled at irregular times has unequal pair
        # displacements, but no acceleration. Timing must not bend it.
        # Equally spaced content with jittered presents must also stay flat.
        # Include the entering border: acceleration must preserve reveal_fill
        # even where the measured acceleration is locally zero.
        for mode, option in (("interpolate", "INTERP_ACCEL=1"), ("extrapolate", "EXTRAPOLATE_ACCEL=1")):
            for clock in ("", " --steady-content"):
                base, accel = evaluate(build, d, {"BDEX_FG_MODE": mode, "BDEX_FG_FRAME_TIMING": "1",
                                                  "BDEX_FG_INTERP_ACCEL": "0", "BDEX_FG_EXTRAPOLATE_ACCEL": "0",
                                                  "BDEX_FG_REVEAL_FILL": "1"},
                                       ["--multiplier", "4", "--offline-args", "--jitter 0.6 --spacing 4" + clock,
                                        "--variant", option])
                if accel < base - 0.04:
                    print(f"{mode}{clock}: acceleration bent constant motion: {base:.2f} -> {accel:.2f} dB")
                    return 1
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(d), "--frames", "29"],
                       check=True, stdout=subprocess.DEVNULL)
        # (off explicitly in the base: a discrete GPU turns it on by itself;
        # false_detail and border_predict, which move both sides, too)
        base, local = evaluate(build, d / "popup", {"BDEX_FG_MODE": "interpolate", "BDEX_FG_SCENE_CUT_LOCAL": "0",
                                                    "BDEX_FG_FALSE_DETAIL": "0", "BDEX_FG_BORDER_PREDICT": "0",
                                                    "BDEX_FG_BORDER_FILL": "0", "BDEX_FG_STILL_OCCLUDER": "0", "BDEX_FG_HUD_AVOID": "0",
                                                    "BDEX_FG_MOSAIC_BLUR": "0", "BDEX_FG_HUD_LAYER": "0"},
                               ["--variant", "SCENE_CUT_LOCAL=1"])
        if not local > base + 0.5:
            print(f"popup: scene_cut_local {local:.2f} dB does not beat the frame-wide test {base:.2f} dB")
            return 1
        base, accel = evaluate(build, d / "swing", {"BDEX_FG_MODE": "extrapolate"}, ["--variant", "EXTRAPOLATE_ACCEL=1"])
        if not accel > base + 0.5:
            print(f"swing: extrapolate_accel {accel:.2f} dB does not beat constant velocity {base:.2f} dB")
            return 1
        # border_predict: the pan's entering / leaving content keeps the
        # camera's motion instead of a match against the clamped edge
        # (off explicitly in the base: a discrete GPU turns it on).
        base, border = evaluate(build, d / "pan", {"BDEX_FG_MODE": "extrapolate", "BDEX_FG_BORDER_PREDICT": "0"},
                                ["--variant", "BORDER_PREDICT=1"])
        if not border > base + 0.2:
            print(f"pan: border_predict {border:.2f} dB does not beat matching against the edge {base:.2f} dB")
            return 1
        # parallax_search: a railing's thin posts in a sideways move (x4, the
        # game-like scene, 21 frames: 2 s to make)
        subprocess.run([sys.executable, str(ROOT / "tools" / "edge_bench" / "make_game.py"), str(d / "fence"), "fence",
                        "21"], check=True, stdout=subprocess.DEVNULL)
        base, px = evaluate(build, d / "fence", {"BDEX_FG_MODE": "interpolate", "BDEX_FG_PARALLAX_SEARCH": "0"},
                            ["--multiplier", "4", "--variant", "PARALLAX_SEARCH=1"])
        if not px > base + 0.04:  # (+0.26 dB on a discrete GPU, +0.08 under lavapipe without its options)
            print(f"fence: parallax_search {px:.2f} dB does not beat the hierarchical search alone {base:.2f} dB")
            return 1
        base, chroma = evaluate(build, d / "chroma", {"BDEX_FG_MODE": "interpolate"}, ["--variant", "CHROMA_WEIGHT=1"])
        if not chroma > base + 3.0:
            print(f"chroma: chroma_weight {chroma:.2f} dB does not beat luma-only matching {base:.2f} dB")
            return 1
        # depth: the game's depth buffer (an exact one, tools/edge_bench's)
        # orders the layers at the edges of objects moving on their own
        # (+1.6 dB on 33 frames on a discrete GPU).
        subprocess.run([sys.executable, str(ROOT / "tools" / "edge_bench" / "make_sequence.py"), str(d / "edges"), "still",
                        "9"], check=True, stdout=subprocess.DEVNULL)
        env = {"BDEX_FG_MODE": "interpolate"}
        base = evaluate(build, d / "edges", env)[0]
        depth = evaluate(build, d / "edges", env, ["--offline-args", f"--depth {d / 'edges' / 'depth'}"])[0]
        if not depth > base + 0.4:
            print(f"edges: with the depth {depth:.2f} dB, without {base:.2f} dB")
            return 1
        # profile = 2: the GPU time of each engine step and optional pass —
        # named, the flow and the synthesis among them, adding up to about
        # the stages' — and the same frames as without the timestamps.
        outs = {}
        for profile in ("0", "2"):
            out = d / f"profile{profile}"
            out.mkdir()
            env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PROFILE=profile)
            log = subprocess.run([str(build / "tools" / "offline" / "bdex_offline"), str(d / "edges"), str(out)],
                                 check=True, stderr=subprocess.PIPE, text=True, env=env).stderr
            outs[profile] = {p.name: p.read_bytes() for p in sorted(out.glob("*.ppm"))}
            line = next((l for l in log.splitlines() if "gpu ms per input" in l), "")
            if profile == "0" and line:
                print(f"profile=0 printed GPU timings: {line}")
                return 1
            if profile == "2" and line:  # (none: a GPU without timestamps)
                stages, _, passes = line.partition("| per pass:")
                words = passes.split()
                per_pass = dict(zip(words[0::2], map(float, words[1::2])))
                words = stages.split("input:")[1].split()
                per_stage = dict(zip(words[0::2], map(float, words[1::2])))
                print(line)
                total, known = sum(per_stage.values()), sum(per_pass.values())
                if not {"flow", "synthesis"} <= set(per_pass) or not 0.5 * total <= known <= 1.5 * total + 0.1:
                    print(f"profile=2: per pass {known:.2f} ms against {total:.2f} ms per stage")
                    return 1
        if not outs["0"] or outs["0"] != outs["2"]:
            print("profile=2 changed the generated frames")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
