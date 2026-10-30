#!/usr/bin/env python3
"""Render-arm analyzer: turn orb serial captures into the §3 table of
docs/render-cost-findings-2026-10-30.md, and refuse to report an arm it cannot
prove.

Why this exists: Station's ~7 fps was attributed to DMA bytes for as long as nobody
could say what the 90 ms between flushes was. The dial now prints the answer
(`render ms/s … = N us/kpx`), and the two ablation arms that price the fix print a
boot line naming themselves. An arm whose boot line is absent is an arm that was
never on the glass -- a capture after a refused flash leaves the previous image
running -- so this exits non-zero on it rather than averaging two arms into one
row.

Usage:
    render_arms.py [--window LO:HI] label=path [--arm=LINE] ...

  label=path      a capture, e.g. shipped=/tmp/cap-off-1.txt
  --window LO:HI  device-time seconds to average, default 8:20
  --arm=LINE      extra substring that MUST appear in the capture's boot lines
                  (repeatable). `commit: cols FULL` proves the x-clip OFF arm;
                  `ABLATE AA` proves the nearest-everywhere arm; `ABLATE INK`
                  proves the hands-hidden arm.

The window matters more than it looks. The dial's tempo decays with device time
(one run read 12 fps at t=11 s and 6 fps at t=74 s), so arms measured over
different windows compare the decay. Every capture resets the board when it opens
the port, so each starts at t=0 and the same window is always available. Report
lines do not exist before SNTP lands (~10 s cold): a capture shorter than ~20 s
proves nothing and is reported as n=0.
"""
import re
import statistics
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# The clock line and the render line arrive ~4 ms apart, so they are paired in
# stream order rather than by timestamp equality.
CLK = re.compile(r"I \((\d+)\) clock: clock (\d+) fps \| flush frames (\d+)")
REN = re.compile(r"render ms/s (\d+) at (\d+) px/s = (\d+) us/kpx")
PHA = re.compile(r"phases ms/s: mirror (\d+) te (\d+) dma (\d+) \| hook (\d+)")
COMMIT = re.compile(r"commit: rows coalesced, cols ([A-Z ]*?) ")


def parse(path):
    """Return (rows, boot_text): rows are (t, fps, flush_frames, render_tuple)."""
    raw = ANSI.sub("", open(path, "rb").read().decode("utf8", "replace"))
    boot = []
    rows = []
    pend = None
    for ln in raw.splitlines():
        boot.append(ln)
        m = CLK.search(ln)
        if m:
            pend = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            continue
        r = REN.search(ln)
        if r and pend:
            rows.append((pend[0], pend[1], pend[2],
                         (int(r.group(1)), int(r.group(2)), int(r.group(3)))))
            pend = None
    return rows, "\n".join(boot)


def med(xs):
    return statistics.median(xs) if xs else 0


def main(argv):
    window = (8, 20)
    arms = []
    targets = []
    for a in argv[1:]:
        if a.startswith("--window="):
            lo, hi = a.split("=", 1)[1].split(":")
            window = (int(lo), int(hi))
        elif a.startswith("--arm="):
            arms.append(a.split("=", 1)[1])
        elif "=" in a:
            targets.append(tuple(a.split("=", 1)))
        else:
            targets.append(("?", a))
    if not targets:
        print(__doc__.strip().splitlines()[0])
        print("usage: render_arms.py label=path ...")
        return 2

    print(f"{'arm':12s} {'capture':22s} {'n':>3s} {'fps':>5s} {'flush':>6s} "
          f"{'ms/s':>5s} {'kpx/s':>6s} {'us/kpx':>7s}  commit cols")
    bad = 0
    for label, path in targets:
        try:
            rows, boot = parse(path)
        except OSError as e:
            print(f"{label:12s} {path:22s} unreadable: {e}")
            bad = 1
            continue
        sel = [r for r in rows if window[0] <= r[0] / 1000.0 <= window[1]]
        cols = COMMIT.search(boot)
        missing = [a for a in arms if a not in boot]
        if missing:
            print(f"{label:12s} {path:22s} ARM NOT PROVEN -- boot lacks: "
                  f"{', '.join(repr(m) for m in missing)}")
            bad = 1
            continue
        if not sel:
            print(f"{label:12s} {path:22s} {'0':>3s}   -- no samples in "
                  f"{window[0]}-{window[1]}s: capture too short or dial not foreground "
                  f"(GET /api/v1/screen?to=clock)")
            bad = 1
            continue
        fps = med([r[1] for r in sel])
        fl = med([r[2] for r in sel])
        ms = med([r[3][0] for r in sel])
        px = med([r[3][1] for r in sel])
        us = med([r[3][2] for r in sel])
        print(f"{label:12s} {path.split('/')[-1]:22s} {len(sel):3d} {fps:5.1f} "
              f"{fl:6.1f} {ms:5.0f} {px/1000:6.0f} {us:7.0f}  "
              f"{cols.group(1).strip() if cols else '(no commit line)'}")
    if arms:
        print(f"\narm proof required in every capture: {', '.join(repr(a) for a in arms)}")
    print("window: "
          f"{window[0]}-{window[1]}s of device time, medians; every capture is its own boot.")
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv))
