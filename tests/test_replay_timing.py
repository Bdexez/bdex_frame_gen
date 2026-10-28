#!/usr/bin/env python3
"""The real Vulkan demo replays an uneven clock, including a source slice."""
import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        print("no display available, skipping")
        return 77
    demo = Path(sys.argv[1]).resolve()
    env = {k: v for k, v in os.environ.items() if not k.startswith("BDEX_FG")}
    env.update(BDEX_FG_CONFIG="/dev/null", BDEX_FG_DISABLE="1")
    with tempfile.TemporaryDirectory(prefix="bdex-replay-timing-") as tmp:
        root = Path(tmp)
        source = [0, 40, 130, 155, 220, 250, 335]
        for i in range(len(source)):
            (root / f"f{i:04d}.ppm").write_bytes(b"P6\n32 24\n255\n" + bytes([i * 30, 50, 80]) * (32 * 24))
        times = root / "timestamps.csv"
        with times.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["frame", "present_ms", "interval_ms"])
            for i, t in enumerate(source):
                writer.writerow([f"f{i:04d}.ppm", 1000000 + t, t - source[i - 1] if i else 0])
        original = times.read_bytes()
        command = [str(demo), "--replay", str(root), "--replay-timing", "--fps", "1",
                   "--size", "64x48", "--mode", "immediate", "--ignore-resize"]

        def run(args, succeeds=True):
            result = subprocess.run(command + args, env=env, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=15)
            assert (result.returncode == 0) == succeeds, result.stdout
            return result.stdout

        for start, step in [(0, 1), (1, 2)]:
            log = root / f"replay-{start}-{step}.csv"
            run(["--replay-start", str(start), "--replay-step", str(step), "--replay-log", str(log)])
            with log.open(newline="") as f:
                rows = list(csv.DictReader(f))
            selected = list(range(start, len(source), step))
            assert [r["frame"] for r in rows] == [f"f{i:04d}.ppm" for i in selected], rows
            assert [float(r["source_ms"]) for r in rows] == [source[i] - source[start] for i in selected]
            for prev, row in zip(rows, rows[1:]):
                # Requested spacing follows the recording, shifted only for a logged overload.
                expected = float(row["source_ms"]) - float(prev["source_ms"]) + float(prev["shift_ms"])
                actual = float(row["target_ms"]) - float(prev["target_ms"])
                assert abs(actual - expected) < .004, (actual, expected, rows)
                assert float(row["present_ms"]) >= float(row["target_ms"]) - .1, rows
            # --fps 1 must not add a second between presents.
            assert float(rows[-1]["present_ms"]) - float(rows[0]["present_ms"]) < 2000, rows
        run(["--replay-log", str(times)], succeeds=False)
        assert times.read_bytes() == original
        run(["--replay-start", "7"], succeeds=False)
        times.unlink()
        assert "timestamps.csv missing" in run([], succeeds=False)
    print("replay timing: uneven cadence, selected sources, end, missing clock and log preservation OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
