#!/usr/bin/env python3
"""Input-to-screen latency, from the demo's flashes and the layer's display log.

    bdex_demo --latency-test [auto] --latency-log demo.log      (with the layer:)
    BDEX_FG_LATENCY_LOG=layer.log ... bdex_demo ...
    tools/latency_report.py demo.log layer.log [--label NAME] [--json]

The demo writes "flash <input ms> <present ms>" for every input it answered
with a white frame (the steady clock: a key press as the window system
delivered it, or with `auto` one every half second, taken when the demo
starts the frame); the layer (latency_log) writes "<present ms> <display ms>
<real>" for every frame the screen showed, from the present feedback. The
two are joined on the present call (the layer's timestamp follows the
demo's by microseconds). Reported per flash, then as median / 95th
percentile / mean:

  input->present   the game's own part (input sampled, frame rendered)
  first photon     input to the first displayed frame carrying the flash —
                   with interpolation a generated frame, half of it white
  real frame       input to the flash's own (real) frame on screen

Needs present feedback (VK_KHR_present_wait: gamescope, most Mesa drivers,
NVIDIA); without it only input->present is known. A photodiode on the
screen measures the same flash end to end, compositor and panel included.
`--compare a.json b.json ...` prints saved runs side by side.
"""
import argparse
import bisect
import json
import statistics
import sys


def read_demo(path):
    flashes = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 3 and p[0] == "flash":
                flashes.append((float(p[1]), float(p[2])))
    return flashes


def read_layer(path):
    shown = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 3:
                shown.append((float(p[0]), float(p[1]), p[2] == "1"))
    shown.sort()
    return shown


def join(flashes, shown, tolerance_ms=2.0):
    """Per flash: (input->present, first photon, real frame) in ms (None unknown)."""
    keys = [s[0] for s in shown]
    out = []
    for inp, pres in flashes:
        i = bisect.bisect_left(keys, pres - 0.05)
        frames = []
        while i < len(shown) and shown[i][0] <= pres + tolerance_ms:
            frames.append(shown[i])
            i += 1
        # (one present: every frame with that present time)
        if frames:
            t0 = frames[0][0]
            frames = [f for f in frames if abs(f[0] - t0) < 0.01]
        first = min((f[1] for f in frames), default=None)
        real = next((f[1] for f in frames if f[2]), None)
        out.append((pres - inp, None if first is None else first - inp, None if real is None else real - inp))
    return out


def summary(values):
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    p95 = v[min(len(v) - 1, int(round(0.95 * (len(v) - 1))))]
    return {"n": len(v), "median": statistics.median(v), "p95": p95, "mean": statistics.fmean(v)}


def fmt(s):
    return "      —" if s is None else f"{s['median']:6.1f} {s['p95']:6.1f} {s['mean']:6.1f}  (n={s['n']})"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("demo_log", nargs="?")
    ap.add_argument("layer_log", nargs="?")
    ap.add_argument("--label", default="run")
    ap.add_argument("--json", help="also write the summary there (for --compare)")
    ap.add_argument("--compare", nargs="+", metavar="JSON", help="print saved summaries side by side")
    ap.add_argument("--verbose", action="store_true", help="one line per flash")
    a = ap.parse_args()
    if a.compare:
        print(f"{'run':28s} {'input->present':>22s} {'first photon':>22s} {'real frame':>22s}   (median p95 mean, ms)")
        for path in a.compare:
            with open(path) as f:
                r = json.load(f)
            print(f"{r['label']:28s} {fmt(r['input_present']):>22s} {fmt(r['first_photon']):>22s} {fmt(r['real_frame']):>22s}")
        return 0
    if not a.demo_log:
        ap.error("demo_log required")
    flashes = read_demo(a.demo_log)
    if not flashes:
        print("no flash in the demo log (--latency-test, and an input or `auto`)", file=sys.stderr)
        return 1
    rows = join(flashes, read_layer(a.layer_log)) if a.layer_log else [(p - i, None, None) for i, p in flashes]
    if a.verbose:
        for ip, fp, rf in rows:
            print(f"  input->present {ip:6.1f}  first photon {'—' if fp is None else f'{fp:6.1f}'}  "
                  f"real {'—' if rf is None else f'{rf:6.1f}'}")
    r = {"label": a.label, "input_present": summary(r_[0] for r_ in rows),
         "first_photon": summary(r_[1] for r_ in rows), "real_frame": summary(r_[2] for r_ in rows)}
    print(f"{a.label}: {len(rows)} flashes (median p95 mean, ms)")
    print(f"  input -> present   {fmt(r['input_present'])}")
    print(f"  first photon       {fmt(r['first_photon'])}")
    print(f"  real frame         {fmt(r['real_frame'])}")
    if r["first_photon"] is None and a.layer_log:
        print("  (no displayed frame matched: present feedback missing on this surface?)")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
