#!/usr/bin/env python3
"""How the frames of a game really reached the screen, from the layer's
display log.

    BDEX_FG_LATENCY_LOG=/tmp/fg.log %command%      (Steam launch options; or in
                                                    the config: latency_log = ...)
    tools/cadence_report.py /tmp/fg.log [--hz 143.912] [--skip 3]

The log (latency_log) has a line per displayed frame: "<game's present ms>
<display ms> <real 0/1> <planned ms>" (steady clock, from the present
feedback; the planned refresh is pace_plan's, 0 without one). Under Xwayland
(x11_clock), a line per present once the X server reports it executed — the
display time is then the refresh it went to the compositor at — followed by
"<msc> <content ms> <ust ms> <mode> <sent ms>": the content time the frame
shows on the game's present clock gives the motion line below. Reported:

  shown/s         frames that reached the screen per second
  held/s          a frame on screen for more than one refresh (a repeat):
                  display intervals of 1.5 refreshes or more
  same refresh/s  two frames shown within a third of a refresh: one replaced
  off plan        frames shown more than half a refresh from the refresh
                  their content was made for (their motion off by that much)
  motion jumps/s  (x11_clock) content steps over 1.6 times the usual one in
                  a refresh: the picture moving on twice as far at once,
                  usually right after a refresh held
  motion step rms (x11_clock) every refresh's content step against the
                  refresh period (a held refresh is a step of 0): 0 for
                  perfectly even motion; and the sd of the content's delay
                  behind the screen
  content delay   (x11_clock) mean of each refresh's handover time minus the
                  present time of the content it shows: the motion's latency
                  (the real frame's alone tells little once generated frames
                  carry the motion)
  refresh grid    how well the display times sit on a grid of the refresh
                  period: the share within 1 ms of it (feedback precise
                  enough for the layer to measure the grid: most above 90 %)

The refresh rate is taken from --hz, else guessed from the display
intervals. The first --skip seconds (start-up) are left out.
"""
import argparse
import math
import statistics
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--hz", type=float, default=0.0)
    ap.add_argument("--skip", type=float, default=3.0, help="seconds left out at the start")
    a = ap.parse_args()
    rows = []
    with open(a.log) as f:
        for line in f:
            p = line.split()
            if len(p) >= 3:
                try:
                    rows.append((float(p[0]), float(p[1]), p[2] == "1", float(p[3]) if len(p) >= 4 else 0.0,
                                 float(p[5]) if len(p) >= 6 else None))
                except ValueError:
                    pass
    if len(rows) < 20:
        sys.exit("too few displayed frames in the log")
    rows.sort(key=lambda r: r[1])
    start = rows[0][1] + a.skip * 1000.0
    rows = [r for r in rows if r[1] >= start]
    if len(rows) < 20:
        sys.exit("too few frames after --skip")
    shown = [r[1] for r in rows]
    iv = [b - x for x, b in zip(shown, shown[1:])]
    if a.hz > 0:
        period = 1000.0 / a.hz
    else:
        # the most common interval among those over 2 ms
        cand = sorted(x for x in iv if x > 2.0)
        period = statistics.median(cand[: max(1, len(cand) // 2)]) if cand else 16.67
    secs = (shown[-1] - shown[0]) / 1000.0
    same = sum(1 for x in iv if x < period / 3)
    held = sum(1 for x in iv if x >= 1.5 * period)
    games = sorted({r[0] for r in rows if r[2]})
    gi = [b - x for x, b in zip(games, games[1:])]
    k = 2.0 * math.pi / period
    c = sum(math.cos(t * k) for t in shown) / len(shown)
    s = sum(math.sin(t * k) for t in shown) / len(shown)
    phase = math.atan2(s, c) / k
    on_grid = sum(1 for t in shown if abs(((t - phase) / period) - round((t - phase) / period)) * period < 1.0) / len(shown)
    planned = [(r[1] - r[3]) for r in rows if r[3] > 0.0]
    print(f"refresh period    {period:.3f} ms ({1000.0 / period:.2f} Hz){'' if a.hz else ' (guessed)'}")
    if gi:
        print(f"game              {1000.0 / statistics.mean(gi):.1f} fps, frame time sd {statistics.pstdev(gi):.2f} ms")
    print(f"shown/s           {len(shown) / secs:.1f}")
    print(f"held/s            {held / secs:.1f}")
    print(f"same refresh/s    {same / secs:.1f}")
    if planned:
        off = sum(1 for d in planned if abs(d) > period / 2) / len(planned)
        print(f"off plan          {100 * off:.1f} % (display - plan: mean {statistics.mean(planned):+.2f} ms)")
    else:
        print("off plan          (no plan: no refresh grid, or extrapolation)")
    if all(r[4] is not None for r in rows):
        # the frame each refresh showed: the last presented for it
        last = {}
        for r in rows:
            last[round(r[1] / period)] = r[4]
        ticks = sorted(last)
        steps = [(b - a, last[b] - last[a]) for a, b in zip(ticks, ticks[1:]) if 0 < b - a <= 8]
        per = sorted(dc / n for n, dc in steps if dc > 0)
        if per:
            usual = per[len(per) // 2]
            jumps = sum(1 for n, dc in steps if n == 1 and dc > 1.6 * usual)
            print(f"motion jumps/s    {jumps / secs:.2f} (usual step {usual:.2f} ms of content a refresh)")
        # Every refresh in turn, a refresh without a new frame showing the
        # previous one: how far each refresh's content step is from the
        # period (rms), and how much the content's delay behind the screen
        # wanders (sd). Even motion is 0 for both; a frame held a refresh
        # counts a step of 0 then one of two periods.
        if len(ticks) > 2:
            filled, at = [], 0
            for tick in range(ticks[0], ticks[-1] + 1):
                if at + 1 < len(ticks) and ticks[at + 1] <= tick:
                    at += 1
                filled.append((tick * period, last[ticks[at]]))
            deltas = [b[1] - a[1] for a, b in zip(filled, filled[1:])]
            rms = math.sqrt(sum((d - period) ** 2 for d in deltas) / len(deltas))
            lag_sd = statistics.pstdev([t - c for t, c in filled])
            print(f"motion step rms   {rms:.2f} ms (content step a refresh vs the period), delay sd {lag_sd:.2f} ms")
            # The refresh the X server handed each frame over at, behind the
            # game's present clock (the screen shows it a refresh later at the
            # soonest): the motion's latency, whichever frames carry it.
            handover = {}
            for r in rows:
                handover[round(r[1] / period)] = r[1] - r[4]
            print(f"content delay     {statistics.mean(handover.values()):.1f} ms mean (refresh handed over - its content's present time)")
    print(f"refresh grid      {100 * on_grid:.0f} % of display times within 1 ms of it")
    real = [r[1] - r[0] for r in rows if r[2]]
    if real:
        print(f"real frame        present -> screen median {statistics.median(real):.1f} ms")


if __name__ == "__main__":
    main()
