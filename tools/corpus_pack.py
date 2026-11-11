#!/usr/bin/env python3
"""Pack a recorded game sequence for the evaluation corpus.

Takes the frames a game was recorded with (`bdex-framegen --record DIR`, or
any directory of consecutive .ppm/.png frames), keeps a window of them,
downscales them (the CI runs on a software Vulkan driver: the synthetic
corpus is 480x272) and writes a compact archive of PNGs plus meta.json,
which tools/corpus_fetch.py unpacks into the layout the corpus test reads.
A recording made with the `depth` option keeps each frame's depth
(depth/fNNNN.f32): packed too, at the output size (nearest sample).

  tools/corpus_pack.py ~/rec/kenshi kenshi-town.tar.gz --name kenshi-town --start 60 --count 121

Archives attached to the repository's GitHub release named "corpus" are
fetched and scored by the Linux CI on every push (tests/test_corpus.py with
BDEX_CORPUS_DIR); add their references with `test_corpus.py build --update`.
"""
import argparse
import csv
import io
import json
import tarfile
from pathlib import Path

from PIL import Image
from recording_report import read_timestamps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("frames", help="directory of consecutive .ppm / .png frames")
    ap.add_argument("out", help="archive to write (.tar.gz)")
    ap.add_argument("--name", required=True, help="sequence name (a directory name: letters, digits, -_)")
    ap.add_argument("--start", type=int, default=0, help="first frame kept")
    ap.add_argument("--count", type=int, default=121, help="frames kept (odd: x2 needs a last input)")
    ap.add_argument("--width", type=int, default=480, help="output width (height keeps the aspect, even)")
    ap.add_argument("--note", default="", help="what the sequence shows (kept in meta.json)")
    a = ap.parse_args()
    if not a.name.replace("-", "").replace("_", "").isalnum():
        ap.error("--name: letters, digits, '-' and '_' only")
    files = sorted(p for p in Path(a.frames).iterdir() if p.suffix.lower() in (".ppm", ".png"))
    files = files[a.start:a.start + a.count]
    if len(files) < 3:
        ap.error(f"only {len(files)} frame(s) in the window")
    src = Image.open(files[0])
    h = max(2, round(src.height * a.width / src.width / 2) * 2)
    meta = {"name": a.name, "frames": len(files), "source_size": [src.width, src.height], "size": [a.width, h],
            "source": str(Path(a.frames).name), "start": a.start, "note": a.note}
    timing = None
    clock = Path(a.frames) / "timestamps.csv"
    if clock.exists():
        # The panel may delete the source directory after a successful pack:
        # never silently discard an invalid or incomplete recording clock.
        try:
            times = {Path(name).stem: ms for name, ms, _ in read_timestamps(clock)}
            selected = [times[f.stem] for f in files]
            if any(b <= a for a, b in zip(selected, selected[1:])):
                raise ValueError("frame order differs from timestamp order")
        except (ValueError, KeyError, OSError) as e:
            ap.error(f"cannot preserve recording timestamps: {e}")
        buf = io.StringIO(newline="")
        writer = csv.writer(buf)
        writer.writerow(["frame", "present_ms", "interval_ms"])
        for i, ms in enumerate(selected):
            writer.writerow([f"f{i:04d}.ppm", f"{ms:.3f}", f"{ms - selected[i-1] if i else 0:.3f}"])
        timing = buf.getvalue().encode()
        duration = selected[-1] - selected[0]
        meta.update(timing_clock="steady_present_ms", duration_ms=duration,
                    game_fps=(len(selected) - 1) * 1000.0 / duration)
    with tarfile.open(a.out, "w:gz") as tar:
        def add(name, data):
            info = tarfile.TarInfo(f"{a.name}/{name}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        add("meta.json", json.dumps(meta, indent=1).encode())
        if timing is not None:
            add("timestamps.csv", timing)
        depths = 0
        for i, f in enumerate(files):
            buf = io.BytesIO()
            Image.open(f).convert("RGB").resize((a.width, h), Image.LANCZOS).save(buf, "PNG", optimize=True)
            add(f"f{i:04d}.png", buf.getvalue())
            # The frame's depth (little-endian floats, the frame's size): the
            # nearest sample — a depth is not averaged across an edge.
            d = Path(a.frames) / "depth" / (f.stem + ".f32")
            if d.exists() and d.stat().st_size == src.width * src.height * 4:
                depth = Image.frombuffer("F", (src.width, src.height), d.read_bytes(), "raw", "F;32F", 0, 1)
                add(f"depth_f{i:04d}.f32", depth.resize((a.width, h), Image.NEAREST).tobytes("raw", "F;32F"))
                depths += 1
    print(f"{a.out}: {len(files)} frames {a.width}x{h} ({meta['source_size'][0]}x{meta['source_size'][1]} source)"
          + (f", {depths} with depth" if depths else ""))


if __name__ == "__main__":
    main()
