#!/usr/bin/env python3
"""Report a recording's source frame times, sampled on the game's thread.

    python3 tools/recording_report.py ~/.cache/bdex-framegen/recordings/SEQUENCE

timestamps.csv describes source presents, not GPU completion or display.
Older recordings have no per-frame clock; file modification times cannot
recover it because the recorder writes asynchronously.
"""
import argparse
import csv
import math
import statistics
from pathlib import Path


def read_timestamps(path):
    rows = []
    names = set()
    with Path(path).open(newline="") as f:
        reader = csv.DictReader(f)
        if reader.fieldnames != ["frame", "present_ms", "interval_ms"]:
            raise ValueError(f"{path}: invalid timestamps header")
        for row in reader:
            try:
                name = row["frame"]
                present, interval = float(row["present_ms"]), float(row["interval_ms"])
                if not name or Path(name).name != name or name in names:
                    raise ValueError("invalid or repeated frame name")
                if not math.isfinite(present) or not math.isfinite(interval):
                    raise ValueError("non-finite clock")
                if rows:
                    delta = present - rows[-1][1]
                    if delta <= 0 or abs(delta - interval) > 0.0021:
                        raise ValueError("non-monotonic or inconsistent clock")
                elif interval != 0:
                    raise ValueError("first interval must be zero")
            except (TypeError, ValueError, KeyError) as e:
                raise ValueError(f"{path}: invalid timestamp at row {reader.line_num}: {e}") from e
            names.add(name)
            rows.append((name, present, interval))
    if not rows:
        raise ValueError(f"{path}: no timestamps")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sequence", type=Path)
    a = ap.parse_args()
    file = a.sequence / "timestamps.csv"
    if not file.exists():
        ap.exit(1, "No per-frame timestamps in this recording; record again with the updated layer.\n")
    try:
        rows = read_timestamps(file)
    except (OSError, ValueError) as e:
        ap.exit(1, str(e) + "\n")
    intervals = [r[2] for r in rows[1:]]
    if not intervals:
        ap.exit(1, "Too few frames to measure source cadence.\n")
    median = statistics.median(intervals)
    ordered = sorted(intervals)
    def percentile(q):
        return ordered[math.ceil(q * (len(ordered) - 1))]
    print(f"{len(rows)} source frames; {(len(rows)-1)*1000/(rows[-1][1]-rows[0][1]):.2f} fps")
    print(f"frame time min/median/p95/p99/max: {min(intervals):.3f} / {median:.3f} / "
          f"{percentile(.95):.3f} / {percentile(.99):.3f} / {max(intervals):.3f} ms")
    print(f"frame time sd: {statistics.pstdev(intervals):.3f} ms; "
          f"frames over 1.5 x median: {sum(t > 1.5 * median for t in intervals)}")
    print("Source present times only; use cadence_report.py with a latency log for displayed frames.")


if __name__ == "__main__":
    main()
