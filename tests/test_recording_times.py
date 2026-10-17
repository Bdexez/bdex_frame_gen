#!/usr/bin/env python3
"""Recording clocks survive selecting, packing and unpacking a corpus.
Reject invalid metadata before the panel can remove its source directory.
"""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from recording_report import read_timestamps


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        seq = tmp / "src"
        seq.mkdir()
        times = [1000.0, 1030.0, 1060.0, 1110.0, 1140.0]
        for i in range(len(times)):
            Image.new("RGB", (16, 16), (i * 40, 0, 0)).save(seq / f"f{i:04d}.ppm")
        with (seq / "timestamps.csv").open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["frame", "present_ms", "interval_ms"])
            for i, ms in enumerate(times):
                writer.writerow([f"f{i:04d}.ppm", ms, ms - times[i-1] if i else 0])
        archive = tmp / "test.tar.gz"
        pack = [sys.executable, str(ROOT / "tools/corpus_pack.py"), str(seq), str(archive),
                "--name", "timed", "--start", "1", "--count", "3", "--width", "16"]
        subprocess.run(pack, check=True)
        out = tmp / "out"
        subprocess.run([sys.executable, str(ROOT / "tools/corpus_fetch.py"), str(out), str(archive)], check=True)
        rows = read_timestamps(out / "timed/timestamps.csv")
        assert rows == [("f0000.ppm", 1030.0, 0.0), ("f0001.ppm", 1060.0, 30.0), ("f0002.ppm", 1110.0, 50.0)], rows
        meta = json.loads((out / "timed/meta.json").read_text())
        assert meta["duration_ms"] == 80.0 and meta["game_fps"] == 25.0, meta
        subprocess.run([sys.executable, str(ROOT / "tools/recording_report.py"), str(out / "timed")], check=True)
        # A present timestamp going backwards must make packing fail.
        clock = seq / "timestamps.csv"
        clock.write_text(clock.read_text().replace("1110.0", "1050.0"))
        result = subprocess.run(pack, capture_output=True, text=True)
        assert result.returncode != 0 and "cannot preserve" in result.stderr, result.stderr
        # Old recordings, with no timestamps, still pack.
        clock.unlink()
        subprocess.run(pack, check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
