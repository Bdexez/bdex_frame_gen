#!/usr/bin/env python3
"""Unpack corpus archives (tools/corpus_pack.py) into sequence directories.

  tools/corpus_fetch.py OUT_DIR ARCHIVE.tar.gz...

Each archive becomes OUT_DIR/<name>/f0000.ppm..., the layout of
tools/eval_sequence.py and of tests/test_corpus.py's BDEX_CORPUS_DIR
(and OUT_DIR/<name>/depth/f0000.f32... when the recording kept the depth).
"""
import io
import sys
import tarfile
from pathlib import Path

from PIL import Image


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    for archive in sys.argv[2:]:
        with tarfile.open(archive) as tar:
            names = set()
            for m in tar.getmembers():
                if not m.isfile():
                    continue
                parts = Path(m.name).parts
                if len(parts) != 2 or ".." in parts:  # <name>/<file>, nothing else
                    continue
                seq, name = parts
                names.add(seq)
                dest = out / seq
                dest.mkdir(parents=True, exist_ok=True)
                data = tar.extractfile(m).read()
                if name.endswith(".png"):
                    Image.open(io.BytesIO(data)).convert("RGB").save(dest / (Path(name).stem + ".ppm"))
                elif name in ("meta.json", "timestamps.csv"):
                    (dest / name).write_bytes(data)
                elif name.startswith("depth_f") and name.endswith(".f32"):
                    (dest / "depth").mkdir(exist_ok=True)
                    (dest / "depth" / name[len("depth_"):]).write_bytes(data)
        print(f"{archive}: {', '.join(sorted(names))}")


if __name__ == "__main__":
    main()
