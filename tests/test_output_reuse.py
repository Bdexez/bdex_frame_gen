#!/usr/bin/env python3
"""Shared GPU moments must keep every packed pixel and every logical output.

Compare reuse with full synthesis on the same recorded inputs/timing. Raw
packed images catch HDR/FP16 differences hidden by SDR PPM quantisation.
No window; the caller can select a software Vulkan ICD.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

from test_offline import frames


def main():
    tool = Path(sys.argv[1]).resolve() / "tools/offline/bdex_offline"
    rich = dict(PIXEL_VECTORS="1", SYNTH_BICUBIC="1", INTERP_SPLAT="1", INTERP_ACCEL="1",
                HUD_MASK="4", STATIC_SOFT="1", FALSE_DETAIL="1", MOSAIC_BLUR="1", HUD_LAYER="1",
                HUD_AVOID="1", BORDER_FILL="1", SCENE_CUT_LOCAL="1")
    # name, mode, multiplier, settings, extra CLI arguments, expect copies
    cases = [
        ("x4", "interpolate", 4, {}, [], True),
        ("x8", "interpolate", 8, {}, [], True),
        ("hybrid", "hybrid", 8, {}, [], True),
        ("hybrid-predict1", "hybrid", 4, {"HYBRID_PREDICT": "1"}, [], True),
        ("predictions", "extrapolate", 8, {"EXTRAPOLATE_SPLAT": "1", "EXTRAPOLATE_ACCEL": "1"}, [], True),
        ("rich", "interpolate", 8, rich, [], True),
        ("rich-predictions", "extrapolate", 8, dict(rich, EXTRAPOLATE_SPLAT="1", REVEAL_FILL="1"), [], True),
        ("temporal-cas", "interpolate", 8, dict(rich, UPSCALE_TEMPORAL="0.3", SHARPNESS="0.5"),
         ["--render-scale", "0.5"], True),
        ("upscale-lanczos", "interpolate", 8, {"UPSCALE_FILTER": "lanczos", "SHARPNESS": "0.5"},
         ["--render-scale", "0.75"], True),
        ("bgra8", "interpolate", 8, rich, ["--format", "bgra8"], True),
        ("srgb", "interpolate", 8, rich, ["--format", "srgb"], True),
        ("rgb10", "interpolate", 8, rich, ["--format", "rgb10"], True),
        ("fp16", "interpolate", 8, rich, ["--format", "rgba16f"], True),
        ("scrgb", "interpolate", 8, rich, ["--format", "rgba16f", "--colorspace", "scrgb"], True),
        ("hdr10", "interpolate", 8, rich, ["--format", "rgb10", "--colorspace", "hdr10"], True),
        ("fp16-cas", "interpolate", 8, {"UPSCALE_TEMPORAL": "0.3", "SHARPNESS": "0.5"},
         ["--format", "rgba16f", "--render-scale", "0.5"], True),
        ("repeat", "interpolate", 8, dict(rich, DEDUP="1"), ["--dup", "2"], True),
        ("repeat-predictions", "extrapolate", 8, {"DEDUP": "1", "EXTRAPOLATE_SPLAT": "1"},
         ["--dup", "2"], True),
        ("tuning", "interpolate", 8, dict(rich, ONLINE_TUNE="1", FLOW_TEMPORAL="1"), [], True),
        ("learned-fallback", "interpolate", 8, dict(rich, LEARNED_CORRECTION="0.4"), [], False),
        ("learned-unusable", "interpolate", 8, {"LEARNED_CORRECTION": "0.4"}, ["--format", "rgb10"], True),
        ("uneven-intervals", "interpolate", 8, dict(rich, FRAME_TIMING="1"), ["--jitter", "0.6"], True),
        ("distinct-times", "interpolate", 8, {}, ["--refresh", "960"], False),
        ("small-device", "interpolate", 8, {}, ["--limit-storage-images", "4", "--limit-shared", "16"], True),
    ]
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        seq = tmp / "source"
        seq.mkdir()
        frames(seq, n=9, w=160, h=96, speed=3)
        for index, p in enumerate(sorted(seq.glob("*.ppm"))):
            image = Image.open(p).convert("RGB")
            draw = ImageDraw.Draw(image)
            draw.rectangle((5, 5, 65, 24), fill=(30, 40, 50))
            draw.text((8, 8), "HUD 42" if index < 5 else "HUD 43", fill=(255, 255, 255))
            image.save(p)
        base_env = {k: v for k, v in os.environ.items() if not k.startswith("BDEX_FG")}
        base_env.update(BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET="balanced", BDEX_FG_LOG="1",
                        XDG_CACHE_HOME=str(tmp / "cache"), XDG_CONFIG_HOME=str(tmp / "config"),
                        XDG_DATA_HOME=str(tmp / "data"))
        total_files = total_reused = 0
        for name, mode, mult, settings, extra, expected in cases:
            env = dict(base_env, BDEX_FG_MODE=mode)
            env.update({"BDEX_FG_" + k: v for k, v in settings.items()})
            paths = []
            for reuse in (False, True):
                out = tmp / ("reuse" if reuse else "full")
                out.mkdir()
                args = [str(tool), str(seq), str(out), "--multiplier", str(mult), "--spacing", "1",
                        "--refresh", "144", "--refresh-known", "--max-inputs", "6", "--verbose", "--raw-dumps", *extra]
                if not reuse:
                    args += ["--no-output-reuse"]
                result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120)
                if result.returncode == 77:
                    print("no Vulkan device, skipping")
                    return 77
                if result.returncode:
                    raise RuntimeError(f"{name}: engine exit {result.returncode}\n{result.stderr}")
                if re.search(r"VUID-|SYNC-HAZARD", result.stderr):
                    raise RuntimeError(f"{name}: Vulkan validation message\n{result.stderr}")
                match = re.search(r"reused (\d+) of (\d+) generated outputs \(copy supported (\d)\)", result.stderr)
                if not match:
                    raise RuntimeError(f"{name}: reuse diagnostics missing\n{result.stderr}")
                count, generated, supported = map(int, match.groups())
                if generated <= 0 or (not reuse and count) or (reuse and not expected and count):
                    raise RuntimeError(f"{name}: unexpected reuse count {count}/{generated}")
                if reuse and expected and supported and not count:
                    raise RuntimeError(f"{name}: shared moments did not exercise reuse")
                if reuse:
                    total_reused += count
                paths.append(out)
            full = {p.name: p.read_bytes() for p in paths[0].glob("gen_*.raw")}
            copied = {p.name: p.read_bytes() for p in paths[1].glob("gen_*.raw")}
            if not full or full.keys() != copied.keys():
                raise RuntimeError(f"{name}: generated output count differs")
            changed = [name for name in full if full[name] != copied[name]]
            if changed:
                raise RuntimeError(f"{name}: packed pixels differ: {changed}")
            total_files += len(full)
            print(f"{name}: {len(full)} packed images identical, {count}/{generated} reused", flush=True)
            for out in paths:
                for p in out.iterdir():
                    p.unlink()
                out.rmdir()
        # Without a display grid, even explicit content timing keeps the
        # normal distinct output phases. Reuse must stay inactive.
        env = dict(base_env, BDEX_FG_FRAME_TIMING="1")
        result = subprocess.run([str(tool), str(seq), str(tmp / "no-grid"), "--multiplier", "8",
                                 "--spacing", "1", "--max-inputs", "3", "--verbose"],
                                env=env, capture_output=True, text=True, timeout=120)
        if result.returncode or not re.search(r"reused 0 of [1-9]\d* generated outputs", result.stderr):
            raise RuntimeError(f"no-grid: unexpected reuse\n{result.stderr}")
        if not total_reused:
            print("output copies unsupported, fallback checked; skipping copy coverage")
            return 77
        print(f"output reuse: {total_files} packed images identical, {total_reused} duplicate syntheses avoided")
    return 0


if __name__ == "__main__":
    sys.exit(main())
