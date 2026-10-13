#!/usr/bin/env python3
"""Every engine option gives the same frames twice.

Runs bdex_offline twice per setting on a small synthetic sequence (the
corpus's `occlude`: fast and slow motion, occlusions) and compares the
generated frames byte for byte. A shader reading outside what it wrote, a
missing barrier or a race between invocations shows as a run-to-run
difference long before anyone sees a flicker (rot_weight's out-of-tile
reads were found that way). Every option of the registry must be covered:
exercised here, or listed as not affecting the generated frames.

Usage: test_determinism.py BUILD_DIR       (exit 77 without a Vulkan device)
"""
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Each setting, the modes it acts in (i interpolate, e extrapolate, h hybrid),
# extra bdex_offline arguments.
SETTINGS = [
    # Hybrid (frame_order.h) at x4: one interpolated output, then predictions.
    ("zero_bias_contrast=0.02", "h", ["--multiplier", "4"]),
    # x8: seven generated frames a present (the state's eight slots).
    ("zero_bias_contrast=0.02", "i", ["--multiplier", "8"]),
    ("frame_timing=1,dedup=1,extrapolate_splat=1,extrapolate_accel=1,reveal_fill=1", "h", ["--multiplier", "4"]),
    ("zero_bias_contrast=0.1", "ie"), ("temporal_prediction=1", "ie"), ("hud_mask=8,static_soft=1", "i"), ("global_motion=1", "ie"), ("subpixel=1", "ie"),
    ("subpixel_lk=2", "ie"), ("flow_temporal=1", "ie"), ("pixel_vectors=1", "i"),
    ("pixel_vectors=1,synth_bicubic=1", "i"), ("synth_bicubic=1", "ie"), ("occlusion_strength=1", "i"),
    ("static_guard=1", "i"), ("rot_weight=1", "ie"), ("census_weight=1", "ie"), ("grad_weight=1", "ie"),
    ("chroma_weight=1", "ie"), ("hud_mask=4", "ie"), ("reveal_fill=1", "e"), ("extrapolate_splat=1", "e"),
    ("extrapolate_accel=1", "e"), ("frame_timing=1", "ie"), ("dedup=1", "ie"),
    ("scene_cut_local=1", "ie"), ("smoothness=0.01", "ie"), ("zero_bias=0.004", "ie"),
    ("zero_bias_cap=0.002", "ie"), ("zero_bias_cap=0", "ie"), ("scene_cut_hist=0.1", "ie"), ("scene_cut_hist=0", "ie"),
    ("census_margin=0.1,census_weight=1", "ie"), ("refine_bias=1", "ie"), ("levels=5", "ie"),
    ("search=3,search_fine=3", "ie"), ("flow_iterations=2", "ie"), ("refine=0", "ie"), ("refine_all=0", "ie"),
    ("scene_cut_low=0.02,scene_cut_high=0.04", "ie"), ("online_tune=1", "ie"),
    ("flow_confidence=1", "ie"),
    ("learned_correction=1", "i"),
    ("luma_match=0.003", "ie"),
    ("learned_flow=1", "ie"),
    ("interp_splat=1", "i"), ("interp_splat=1,pixel_vectors=1", "i"),
    ("upscale_temporal=0.3", "ie", ["--render-scale", "0.5"]), ("render_scale=0.5", "ie", ["--render-scale", "0.5"]),
    ("upscale_filter=bicubic,sharpness=0.5", "ie", ["--render-scale", "0.5"]),
    # Everything at once, per mode.
    ("zero_bias_contrast=0.1,temporal_prediction=1,subpixel_lk=2,flow_temporal=1,pixel_vectors=1,synth_bicubic=1,"
     "occlusion_strength=1,static_guard=1,chroma_weight=1,hud_mask=4,scene_cut_local=1,frame_timing=1,dedup=1", "i"),
    ("zero_bias_contrast=0.1,temporal_prediction=1,subpixel=1,subpixel_lk=2,flow_temporal=1,synth_bicubic=1,"
     "chroma_weight=1,hud_mask=4,reveal_fill=1,extrapolate_splat=1,extrapolate_accel=1,dedup=1", "e"),
]
# Options that do not change the generated frames of an offline run: output
# path / presentation / logging / platform settings, or covered through
# another key above ("fullres" is flow_scale=1; "upscale" is render_scale).
NOT_FRAMES = {"enabled", "multiplier", "low_latency", "max_frame_latency", "latency_limiter", "sync_present", "hide_present_ext", "ignore_anticheat", "pacing", "require_app",
              "present_feedback", "async_compute", "present_queue", "log", "paused", "gpu_budget", "stats", "profile",
              "shared_queue", "stats_interval", "dump_frames", "preset", "fullres", "flow_scale", "mode", "adaptive",
              "upscale", "overlay", "capture_hdr", "log_file", "target_fps", "fps_limit", "dump",
              "present_mode", "debug", "interp_hold", "synth_priority", "skip_fast", "crash_log", "latency_log", "vrr_hold", "precise_wake", "thread_priority", "pipeline_cache",
              "gpu", "method", "hotkey", "battery_saver", "present_timing", "refresh_fill", "pace_plan", "virtual_grid"}


def frames_hash(out):
    h = {}
    for f in sorted(out.glob("gen_*.ppm")):
        h[f.name] = hashlib.sha1(f.read_bytes()).hexdigest()
    return h


def main():
    build = Path(sys.argv[1])
    binary = build / "tools" / "offline" / "bdex_offline"
    keys = subprocess.run([str(binary), "--list-options", "--canonical"], check=True, capture_output=True,
                          text=True).stdout.split()
    covered = {kv.split("=")[0] for s in SETTINGS for kv in s[0].split(",")}
    covered |= {"search_radius"}  # "search" is its canonical name
    missing = sorted(set(keys) - covered - NOT_FRAMES)
    if missing:
        print(f"options not covered by the determinism test: {', '.join(missing)} "
              "(add a setting, or list them in NOT_FRAMES)")
        return 1
    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        subprocess.run([sys.executable, str(ROOT / "tools" / "make_corpus.py"), str(tmp / "corpus"), "--frames", "13"],
                       check=True, stdout=subprocess.DEVNULL)
        seq = tmp / "corpus" / "occlude"
        for setting in SETTINGS:
            spec, modes = setting[0], setting[1]
            extra = setting[2] if len(setting) > 2 else []
            env = dict(os.environ, BDEX_FG_CONFIG="/dev/null", BDEX_FG_PRESET="balanced", BDEX_FG_CACHE_DIR="")
            for kv in spec.split(","):
                k, v = kv.split("=")
                env["BDEX_FG_" + k.upper()] = v
            for mode in modes:
                env["BDEX_FG_MODE"] = {"i": "interpolate", "e": "extrapolate", "h": "hybrid"}[mode]
                hashes = []
                for run in (0, 1):
                    out = tmp / f"run{run}"
                    subprocess.run(["rm", "-rf", str(out)])
                    r = subprocess.run([str(binary), str(seq), str(out), *extra], env=env, capture_output=True, text=True)
                    if r.returncode == 77 or "no Vulkan device" in r.stderr:
                        print("no Vulkan device, skipping")
                        return 77
                    if r.returncode != 0:
                        failures.append(f"{spec} [{env['BDEX_FG_MODE']}]: bdex_offline failed: {r.stderr.strip()[-200:]}")
                        break
                    hashes.append(frames_hash(out))
                if len(hashes) == 2:
                    diff = [n for n in hashes[0] if hashes[0][n] != hashes[1].get(n)]
                    status = "ok" if not diff and hashes[0] else f"DIFFERS ({len(diff)} of {len(hashes[0])} frames)"
                    if status != "ok":
                        failures.append(f"{spec} [{env['BDEX_FG_MODE']}]: {status}")
                    print(f"{env['BDEX_FG_MODE']:<12} {spec[:70]:<70} {status}")
    if failures:
        print("not deterministic:\n  " + "\n  ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
