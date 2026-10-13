#!/usr/bin/env python3
"""Compare default generated frames with the pre-census build on one GPU.

Run under a usable graphical display with two built trees:
    python3 tests/check_matcher_invariant.py build baseline/build
The old commit is pinned in the Linux CI workflow. Real frames are checked
first so a scene or display mismatch cannot masquerade as a flow regression.
"""
import os
import shutil
import subprocess
import sys
import tempfile
from contextlib import nullcontext
from pathlib import Path

import numpy as np
from PIL import Image


def layer_env(build, work):
    """Load exactly this build's layer (see tools/layer_env.sh): old loaders
    ignore VK_IMPLICIT_LAYER_PATH but always search XDG_DATA_HOME."""
    xdg = Path(work) / "vulkan" / "implicit_layer.d"
    xdg.mkdir(parents=True, exist_ok=True)
    for m in (build / "layer").glob("*.json"):
        shutil.copy(m, xdg)
    return {"VK_IMPLICIT_LAYER_PATH": str(build / "layer"), "XDG_DATA_HOME": str(work)}


def render(build, dump):
    # Compare the historical default path, regardless of the caller's game
    # profile or experimental BDEX_FG_* settings.
    env = {k: v for k, v in os.environ.items()
           if k != "BDEX_FG" and not k.startswith("BDEX_FG_") and k != "VK_ADD_IMPLICIT_LAYER_PATH"}
    env.update({
        **layer_env(build, Path(dump).parent / f"xdg-{build.resolve().as_posix().replace('/', '_')}"),
        "BDEX_FG_CONFIG": "/dev/null",
        "BDEX_FG": "1",
        "BDEX_FG_PRESET": "balanced",
        "BDEX_FG_MULTIPLIER": "2",
        "BDEX_FG_GRAD_WEIGHT": "0",
        # The default since 0.4, absent from the baseline: off for the historical path.
        "BDEX_FG_ZERO_BIAS_CONTRAST": "0",
        # Likewise (0.6): the histogram gate of the scene-cut fallback, the
        # capped zero bias.
        "BDEX_FG_SCENE_CUT_HIST": "0",
        "BDEX_FG_ZERO_BIAS_CAP": "0",
        "BDEX_FG_HUD": "0",
        # What a discrete GPU turns on by itself with any preset but
        # performance (Config::autoConfigure): off, as on the CI's lavapipe.
        "BDEX_FG_PIXEL_VECTORS": "0", "BDEX_FG_INTERP_SPLAT": "0", "BDEX_FG_ROT_WEIGHT": "0",
        "BDEX_FG_TEMPORAL_PREDICTION": "0", "BDEX_FG_ZERO_BIAS": "0.002", "BDEX_FG_SCENE_CUT_LOCAL": "0",
        "BDEX_FG_EXTRAPOLATE_SPLAT": "0", "BDEX_FG_REVEAL_FILL": "0", "BDEX_FG_GLOBAL_MOTION": "0", "BDEX_FG_HUD_MASK": "0", "BDEX_FG_STATIC_SOFT": "0", "BDEX_FG_INTERP_ACCEL": "0",
        "BDEX_FG_LOG": "0",
        "BDEX_FG_STATS": "0",
        "BDEX_FG_DUMP": str(dump),
        "BDEX_FG_DUMP_FRAMES": "24",
    })
    return subprocess.Popen([str(build / "demo" / "bdex_demo"), "--fps", "30",
                             "--frames", "12", "--size", "800x600", "--deterministic"],
                            env=env, stdout=subprocess.DEVNULL)


def frames(path, kind):
    files = sorted(path.glob(f"*_{kind}_*.ppm"))
    if not files:
        raise RuntimeError(f"no {kind} frames in {path}")
    last = max(int(p.name.split("_")[0][2:]) for p in files)
    files = [p for p in files if int(p.name.split("_")[0][2:]) == last]
    return [np.asarray(Image.open(p)) for p in files]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    current, baseline = (Path(p).resolve() for p in sys.argv[1:])
    keep = os.environ.get("BDEX_INVARIANT_KEEP")
    context = nullcontext(keep) if keep else tempfile.TemporaryDirectory(prefix="bdex-invariant-")
    with context as tmp:
        root = Path(tmp)
        if keep:
            print(f"keeping frame dumps in {root}")
        # Local tiling compositors may require concurrent windows to give both
        # demos one extent. CI's Xvfb has no window manager and runs sequentially.
        processes = []
        try:
            for name, build in (("baseline", baseline), ("current", current)):
                (root / name).mkdir()
                process = render(build, root / name)
                processes.append(process)
                if os.environ.get("BDEX_INVARIANT_SEQUENTIAL") == "1":
                    if process.wait(timeout=120) != 0:
                        raise RuntimeError(f"{name} demo exited with status {process.returncode}")
            for process in processes:
                if process.poll() is None and process.wait(timeout=120) != 0:
                    raise RuntimeError(f"demo exited with status {process.returncode}")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
        for kind in ("real", "gen"):
            old = frames(root / "baseline", kind)
            new = frames(root / "current", kind)
            if len(old) != len(new):
                raise AssertionError(f"{kind}: frame count {len(old)} != {len(new)}")
            for i, (a, b) in enumerate(zip(old, new)):
                if a.shape != b.shape:
                    raise AssertionError(f"{kind} frame {i}: window sizes {a.shape} != {b.shape}; run under a fixed display such as Xvfb")
                if not np.array_equal(a, b):
                    difference = np.max(np.abs(a.astype(np.int16) - b.astype(np.int16)))
                    raise AssertionError(f"{kind} frame {i} differs (maximum channel delta {difference})")
            print(f"{kind}: {len(old)} frames byte-identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
