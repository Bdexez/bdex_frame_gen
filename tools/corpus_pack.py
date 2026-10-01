#!/usr/bin/env python3
"""Pack a recorded game sequence for the evaluation corpus.

Takes the frames a game was recorded with (`bdex-framegen --record DIR`, or
any directory of consecutive .ppm/.png frames), keeps a window of them,
downscales them (the CI runs on a software Vulkan driver: the synthetic
corpus is 480x272) and writes a compact archive of PNGs plus meta.json,
which tools/corpus_fetch.py unpacks into the layout the corpus test reads.

  tools/corpus_pack.py ~/rec/kenshi kenshi-town.tar.gz --name kenshi-town --start 60 --count 121

Archives attached to the repository's GitHub release named "corpus" are
fetched and scored by the Linux CI on every push (tests/test_corpus.py with
BDEX_CORPUS_DIR); add their references with `test_corpus.py build --update`.
"""
import argparse
import io
import json
import tarfile
from pathlib import Path

from PIL import Image


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
    with tarfile.open(a.out, "w:gz") as tar:
        def add(name, data):
            info = tarfile.TarInfo(f"{a.name}/{name}")
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
        add("meta.json", json.dumps(meta, indent=1).encode())
        for i, f in enumerate(files):
            buf = io.BytesIO()
            Image.open(f).convert("RGB").resize((a.width, h), Image.LANCZOS).save(buf, "PNG", optimize=True)
            add(f"f{i:04d}.png", buf.getvalue())
    print(f"{a.out}: {len(files)} frames {a.width}x{h} ({meta['source_size'][0]}x{meta['source_size'][1]} source)")


if __name__ == "__main__":
    main()
