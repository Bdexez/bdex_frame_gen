#!/usr/bin/env python3
"""Measure fixed xN at arbitrary source rates in an isolated headless gamescope.

python3 tools/framerate_bench.py --output /tmp/fg-rates --fps 30 59.94 90 variable uncapped

No desktop windows, no user configuration. Keep logs and summary.json; the
reported output rate counts submissions, the X server's rate counts handovers.
This is a pacing benchmark, not an image-quality or real-display measurement.
"""
import argparse
import csv
import json
import math
import os
from pathlib import Path
import re
import signal
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent


def gpu_snapshot():
    """Optional context for detecting a busy NVIDIA GPU before a case."""
    if not shutil.which("nvidia-smi"):
        return None
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,utilization.gpu,memory.used,clocks.gr", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=3)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def variable_sources(directory, seconds):
    """A moving pattern sampled at changing rates, with real content times."""
    directory.mkdir()
    now = 0.0
    previous = 0.0
    with (directory / "timestamps.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["frame", "present_ms", "interval_ms"])
        index = 0
        while now <= seconds * 1000:
            # Slow/fast transitions, ramps and alternating source intervals.
            phase = now / (seconds * 1000)
            fps = (30 if phase < .2 else 90 if phase < .4 else
                   24 + 980 * (phase - .4) if phase < .6 else
                   (45 if index % 2 else 200) if phase < .8 else 60)
            dt = 0.0 if index == 0 else now - previous
            name = f"f{index:05d}.ppm"
            pixels = bytearray()
            offset = int(now * .09)
            for y in range(48):
                for x in range(96):
                    v = 210 if ((x - offset) // 9 + y // 8) % 2 else 45
                    pixels.extend((v, 60 + (y * 3) % 150, 255 - v))
            (directory / name).write_bytes(b"P6\n96 48\n255\n" + pixels)
            writer.writerow([name, f"{now:.6f}", f"{dt:.6f}"])
            previous = now
            now += 1000 / fps
            index += 1


def trace_sources(directory, seconds, csv_path):
    """A real game's recorded frame times (timestamps.csv), looped to fill
    the run, on the same moving pattern: the source's own jitter and rate
    changes, which a capped --fps source does not have."""
    intervals = []
    with open(csv_path, newline="") as stream:
        for row in csv.DictReader(stream):
            value = float(row["interval_ms"])
            if 0.5 < value < 500.0:
                intervals.append(value)
    if not intervals:
        raise SystemExit(f"{csv_path}: no frame intervals")
    directory.mkdir()
    now = 0.0
    with (directory / "timestamps.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["frame", "present_ms", "interval_ms"])
        index = 0
        while now <= seconds * 1000:
            dt = 0.0 if index == 0 else intervals[(index - 1) % len(intervals)]
            now += dt
            name = f"f{index:05d}.ppm"
            pixels = bytearray()
            offset = int(now * .09)
            for y in range(48):
                for x in range(96):
                    v = 210 if ((x - offset) // 9 + y // 8) % 2 else 45
                    pixels.extend((v, 60 + (y * 3) % 150, 255 - v))
            (directory / name).write_bytes(b"P6\n96 48\n255\n" + pixels)
            writer.writerow([name, f"{now:.6f}", f"{dt:.6f}"])
            index += 1


def summarize(log):
    rows = []
    for line in log.splitlines():
        match = re.search(r"game ([\d.]+) fps -> output ([\d.]+) fps \(x([\d.]+)\)", line)
        if not match:
            continue
        row = dict(zip(("source_fps", "output_fps", "ratio"), map(float, match.groups())))
        for key, pattern in {
            "real_delay_ms": r"real frame delayed ([\d.]+) ms",
            "present_to_screen_ms": r"present-to-screen ([\d.]+) ms",
            "handover_fps": r"displayed ([\d.]+) fps",
            "catchups": r"(\d+) frame\(s\) without generation to catch up",
            "omitted": r"(\d+) intermediate frame\(s\) omitted to catch up",
        }.items():
            value = re.search(pattern, line)
            if value:
                row[key] = float(value[1])
        row["x11_grid"] = "Hz (X server)" in line
        rows.append(row)
    steady = rows[1:] if len(rows) > 2 else rows
    result = {"samples": rows}
    for key in ("source_fps", "output_fps", "ratio", "real_delay_ms", "present_to_screen_ms", "handover_fps"):
        values = [row[key] for row in steady if key in row]
        if values:
            result[key] = statistics.median(values)
    result["catchups"] = sum(row.get("catchups", 0) for row in steady)
    result["omitted"] = sum(row.get("omitted", 0) for row in steady)
    result["x11_grid"] = any(row["x11_grid"] for row in steady)
    if steady:
        result["aggregate_ratio"] = sum(row["output_fps"] for row in steady) / sum(row["source_fps"] for row in steady)
    return result


def summarize_cadence(log, hz, seconds):
    """Use the existing display-clock analysis, including content jumps."""
    if not log.is_file():
        return {"error": "no latency log"}
    try:
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/cadence_report.py"), str(log),
             "--hz", str(hz), "--skip", str(min(2.0, seconds / 3.0))],
            capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"error": str(exc)}
    if result.returncode:
        return {"error": result.stderr.strip() or result.stdout.strip()}
    data = {"report": result.stdout}
    for key, label in {"logged_per_s": "shown/s", "held_per_s": "held/s",
                       "same_refresh_per_s": "same refresh/s", "motion_jumps_per_s": "motion jumps/s",
                       "motion_step_rms_ms": "motion step rms"}.items():
        match = re.search(r"^" + re.escape(label) + r"\s+([\d.]+)", result.stdout, re.MULTILINE)
        if match:
            data[key] = float(match[1])
    match = re.search(r"delay sd ([\d.]+) ms", result.stdout)
    if match:
        data["content_delay_sd_ms"] = float(match[1])
    match = re.search(r"^content delay\s+([\d.]+)", result.stdout, re.MULTILINE)
    if match:
        data["content_delay_ms"] = float(match[1])
    return data


def child(args):
    env = dict(os.environ)
    env.pop("WAYLAND_DISPLAY", None)  # use the isolated compositor's Xwayland
    env.pop("ENABLE_GAMESCOPE_WSI", None)  # measure actual X Present completions
    env.update(BDEX_FG="1", BDEX_FG_CONFIG="/dev/null", BDEX_FG_LOG="2",
               BDEX_FG_MULTIPLIER=str(args.child_multiplier), BDEX_FG_MODE=args.child_mode,
               BDEX_FG_PRESENT_MODE="immediate", BDEX_FG_STATS_INTERVAL="1",
               BDEX_FG_PRESET=args.preset, BDEX_FG_LATENCY_LOG=str(args.child / "latency.log"))
    # Defaults (including skip_fast) are exercised unless an override is explicit.
    for setting in args.env:
        key, value = setting.split("=", 1)
        env[key if key.startswith("BDEX_FG_") else "BDEX_FG_" + key.upper()] = value
    command = [str(args.demo), "--size", args.size, "--ignore-resize", "--mode", "immediate",
               "--duration", str(args.seconds)]
    if args.gpu_load > 0:
        command += ["--gpu-load", str(args.gpu_load)]
    if args.child_fps == "variable" or args.child_fps.startswith("trace"):
        command += ["--replay", str(args.sources if args.child_fps == "variable" else args.sources.parent / args.child_fps),
                    "--replay-timing", "--replay-log", str(args.child / "source.csv")]
    elif args.child_fps == "uncapped":
        command += ["--uncapped"]
    else:
        command += ["--fps", args.child_fps]
    with (args.child / "demo.log").open("w") as log:
        result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.seconds + 30)
    (args.child / "exit.json").write_text(json.dumps({"returncode": result.returncode}))
    return result.returncode


def run_compositor(command, env, log, done, timeout):
    """Reap the isolated process tree, even if gamescope stalls on shutdown."""
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                               start_new_session=True)
    deadline = time.monotonic() + timeout
    finished_at = None
    reason = None
    try:
        while process.poll() is None:
            now = time.monotonic()
            if done.exists() and finished_at is None:
                finished_at = now
            if finished_at is not None and now - finished_at >= 2.0:
                reason = "shutdown timeout after demo exit"
                break
            if now >= deadline:
                reason = "timeout"
                break
            try:
                process.wait(timeout=0.1)
            except subprocess.TimeoutExpired:
                pass
    finally:
        # A compositor may exit with children still alive; all processes in
        # this new session belong to this one isolated benchmark case.
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    return reason or process.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build")
    parser.add_argument("--layer-dir", type=Path, help="Optional A/B layer; demo still comes from --build")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fps", nargs="*", default=["30", "59.94", "90", "variable", "uncapped"],
                        help="source rates, variable, uncapped (none with --fps alone: only the --trace cases)")
    parser.add_argument("--multipliers", nargs="+", type=int, choices=range(2, 9), default=[4])
    parser.add_argument("--modes", nargs="+", choices=["interpolate", "hybrid", "extrapolate"], default=["interpolate"])
    parser.add_argument("--trace", action="append", default=[], type=Path, metavar="TIMESTAMPS_CSV",
                        help="also replay a real game's frame times (a recording's timestamps.csv), looped")
    parser.add_argument("--hz", type=float, default=144)
    parser.add_argument("--size", default="640x360")
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--preset", default="performance")
    parser.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--gpu-load", type=int, default=0,
                        help="bdex_demo --gpu-load N: the scene drawn N times a frame (a game bound by its own GPU work)")
    parser.add_argument("--check", action="store_true",
                        help="fail if fixed interpolation loses more than 5%% of outputs or triggers catch-up")
    parser.add_argument("--max-motion-jumps", type=float,
                        help="optional maximum content jumps per second (requires display-clock content times)")
    parser.add_argument("--child", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--child-fps", help=argparse.SUPPRESS)
    parser.add_argument("--child-multiplier", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--child-mode", help=argparse.SUPPRESS)
    parser.add_argument("--demo", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--sources", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        return child(args)
    if not args.output:
        parser.error("--output is required (new directory for logs)")
    if not math.isfinite(args.seconds) or args.seconds < 4 or not math.isfinite(args.hz) or args.hz <= 0:
        parser.error("use --seconds >= 4 and --hz > 0")
    if args.max_motion_jumps is not None and (not math.isfinite(args.max_motion_jumps) or args.max_motion_jumps < 0):
        parser.error("--max-motion-jumps must be finite and >= 0")
    for fps in args.fps:
        if fps not in ("variable", "uncapped") and not fps.startswith("trace"):
            try:
                if not math.isfinite(float(fps)) or float(fps) < 1:
                    raise ValueError()
            except ValueError:
                parser.error("--fps accepts positive rates >= 1, variable, or uncapped")
    if any("=" not in setting or not setting.split("=", 1)[0] for setting in args.env):
        parser.error("--env requires KEY=VALUE")
    if not shutil.which("gamescope"):
        parser.error("gamescope is required")
    width, height = map(int, args.size.split("x"))
    if width <= 0 or height <= 0:
        parser.error("--size must be positive WxH")
    args.build = args.build.resolve()
    layer = (args.layer_dir or args.build / "layer").resolve()
    demo = args.build / "demo/bdex_demo"
    manifests = list(layer.glob("*.json"))
    if not manifests or not demo.is_file():
        parser.error("build the layer and demo first")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest_dir = output / "data/vulkan/implicit_layer.d"
    manifest_dir.mkdir(parents=True)
    for manifest in manifests:
        shutil.copy2(manifest, manifest_dir)
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "VK_INSTANCE_LAYERS", "VK_IMPLICIT_LAYER_PATH",
                        "VK_ADD_IMPLICIT_LAYER_PATH") and not k.startswith("BDEX_FG")}
    env.update(XDG_DATA_HOME=str(output / "data"), XDG_CONFIG_HOME=str(output / "config"),
               XDG_CACHE_HOME=str(output / "cache"), BDEX_FG_CONFIG="/dev/null")
    sources = output / "sources"
    if "variable" in args.fps:
        variable_sources(sources, args.seconds)
    for index, trace in enumerate(args.trace):
        name = f"trace{index}"
        trace_sources(output / name, args.seconds, trace)
        (output / name / "origin.txt").write_text(str(trace.resolve()) + "\n")
        args.fps.append(name)
    summaries = []
    failed = False
    for mode in args.modes:
        for mult in args.multipliers:
            for fps in args.fps:
                case = output / f"{mode}-x{mult}-{fps}"
                case.mkdir()
                command = ["gamescope", "--backend", "headless", "-W", str(width), "-H", str(height),
                           "-r", str(args.hz), "--", sys.executable, str(Path(__file__).resolve()),
                           "--child", str(case), "--child-fps", fps, "--child-multiplier", str(mult),
                           "--child-mode", mode, "--demo", str(demo), "--size", args.size,
                           "--seconds", str(args.seconds), "--preset", args.preset, "--sources", str(sources),
                           "--gpu-load", str(args.gpu_load)]
                for setting in args.env:
                    command += ["--env", setting]
                done = case / "exit.json"
                gpu_before = gpu_snapshot()
                with (case / "compositor.log").open("w") as log:
                    compositor_exit = run_compositor(command, env, log, done, args.seconds + 45)
                child_exit = json.loads(done.read_text())["returncode"] if done.exists() else None
                data = summarize((case / "demo.log").read_text(errors="replace")) if (case / "demo.log").exists() else {}
                data.update(mode=mode, multiplier=mult, requested_fps=fps, hz=args.hz,
                            child_exit=child_exit, compositor_exit=compositor_exit,
                            gpu_before=gpu_before, gpu_after=gpu_snapshot(),
                            motion_jump_limit=args.max_motion_jumps)
                data["cadence"] = summarize_cadence(case / "latency.log", args.hz, args.seconds)
                problems = []
                if child_exit != 0:
                    problems.append(f"demo exit: {child_exit}")
                if not data.get("samples"):
                    problems.append("no frame statistics")
                if args.check and mode == "interpolate" and data.get("samples"):
                    if abs(data["aggregate_ratio"] - mult) > 0.05 * mult:
                        problems.append(f"expected x{mult}, measured x{data['aggregate_ratio']:.3f}")
                    if data["catchups"] or data["omitted"]:
                        problems.append("fixed multiplier dropped outputs to catch up")
                if args.max_motion_jumps is not None:
                    jumps = data["cadence"].get("motion_jumps_per_s")
                    if jumps is None:
                        problems.append("no display-clock content times to check motion jumps")
                    elif jumps > args.max_motion_jumps:
                        problems.append(f"motion jumps {jumps:.2f}/s exceed {args.max_motion_jumps:.2f}/s")
                data["problems"] = problems
                failed |= bool(problems)
                summaries.append(data)
                (output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
                print(f"{case.name}: source {data.get('source_fps', '?')}, output {data.get('output_fps', '?')}, "
                      f"x{data.get('ratio', '?')}, catch-ups {data.get('catchups', '?')}, "
                      f"real delay {data.get('real_delay_ms', '?')} ms, X grid {data.get('x11_grid', False)}, "
                      f"motion jumps {data['cadence'].get('motion_jumps_per_s', '?')}/s, "
                      f"step rms {data['cadence'].get('motion_step_rms_ms', '?')} ms, "
                      f"content delay {data['cadence'].get('content_delay_ms', '?')} ms, "
                      f"demo exit {child_exit}", flush=True)
                for problem in problems:
                    print(f"  FAIL: {problem}", flush=True)
    print(f"Logs: {output}")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
