#!/usr/bin/env python3
"""Interleaved x-clip A/B: flash an arm, capture N short windows, swap arms.

Why not one long capture per arm: the dial's tempo DECAYS with device time. One
continuous OFF run read 12 fps at t=11 s and 6 fps at t=74 s, with the per-frame
billing growing from ~22k px to ~98k px over the same minute. Two arms captured
over different device-time windows therefore measure the decay, not the change.

The fix is in the capture, not the analysis: capf.py resets the board when it
opens the port, so every capture covers the same slice of device time. Six
windows (ON,OFF,ON,OFF,ON,OFF) are averaged per arm over one fixed window, and
the alternation cancels any slow ambient drift between the two.
"""
import re
import sys

WIN_LO, WIN_HI = 8_000, 19_000   # past the boot ring settle, inside every capture

MS   = re.compile(r"[IWE] \((\d+)\)")
# orb_sched.c:664 real format: "clock 6 fps | flush frames 6 avg 142 ms |
# px/s 590888 (272% page) chunks/s 66 te/s 71 ms submit/s 177 ms"
FPX  = re.compile(r"clock (\d+) fps \| flush frames (\d+) avg (\d+) ms")
FPS  = re.compile(r"px/s (\d+) \((\d+)% page\)")
FPHA = re.compile(r"phases ms/s: mirror (\d+) te (\d+) dma (\d+)")
FRND = re.compile(r"render ms/s (\d+) at (\d+) px/s = (\d+) us/kpx")
FCOM = re.compile(r"commit: (.*)")


def clean(raw):
    return re.sub(r"\x1b\[[0-9;]*m", "", raw.decode("utf-8", "replace"))


def rows_of(path):
    """Clock samples carry the fps/span/px billing; the phases and render lines
    are SEPARATE log records that immediately follow a clock line, so they are
    carried forward and attached to the sample they belong to."""
    out, commit = [], ""
    dma = te = -1
    rnd = -1                                  # us/kpx; -1 = probe absent
    for raw in open(path, "rb"):
        ln = clean(raw)
        m = MS.search(ln)
        if not m:
            continue
        ms = int(m.group(1))
        if not commit and (c := FCOM.search(ln)):
            commit = c.group(1).strip()
        if (p := FPHA.search(ln)):
            # The phases line reports the second just printed by the clock line,
            # so it patches the row already appended -- never the next one.
            te, dma = int(p.group(2)), int(p.group(3))
            if out:
                out[-1] = out[-1][:4] + (dma, te) + out[-1][6:]
            continue
        if (rn_ := FRND.search(ln)):
            if out:
                out[-1] = out[-1][:6] + (int(rn_.group(3)),)
            continue
        f, p = FPX.search(ln), FPS.search(ln)
        if not f:
            continue
        frames = int(f.group(2))
        pxf = int(p.group(1)) / frames if p and frames else 0.0
        out.append((ms, int(f.group(1)), int(f.group(3)), pxf,
                    max(dma, 0), max(te, 0), rnd))
    return out, commit


def agg(rows):
    m = lambda i: sum(r[i] for r in rows) / len(rows)
    return dict(n=len(rows), fps=m(1), span=m(2), px=m(3), dma=m(4), te=m(5),
                rnd=m(6), lo=min(r[1] for r in rows), hi=max(r[1] for r in rows))


def fmt(tag, a):
    rnd = f"  render {a['rnd']:4.1f} us/kpx" if a["rnd"] >= 0 else ""
    print(f"{tag}  n={a['n']:3d}  fps {a['fps']:5.2f} [{a['lo']}-{a['hi']}]  "
          f"span {a['span']:5.1f} ms  px/frame {a['px']:8.0f}  "
          f"dma {a['dma']:4.0f}  te {a['te']:4.0f}{rnd}")


def collect(paths):
    rows, commits, caps = [], set(), 0
    for p in paths:
        r, c = rows_of(p)
        r = [x for x in r if WIN_LO <= x[0] <= WIN_HI]
        if not r:
            print(f"  !! {p}: nothing in the {WIN_LO}-{WIN_HI} ms window")
            continue
        rows += r
        caps += 1
        if c:
            w = re.search(r"cols\s+(\w+)", c)
            commits.add(w.group(1).lower() if w else c)
    if not rows:
        sys.exit(f"no usable samples in {paths}")
    return agg(rows), commits, caps


OFF_PATHS = sys.argv[1:1 + 3]
ON_PATHS = sys.argv[1 + 3:]
O, oc, ocap = collect(OFF_PATHS)
N, nc, ncap = collect(ON_PATHS)
print(f"window {WIN_LO}-{WIN_HI} ms of device time; "
      f"captures OFF={ocap} ON={ncap}")
fmt("OFF", O); print(f"      arm extent word: {sorted(oc) or 'boot not captured'}")
fmt("ON ", N); print(f"      arm extent word: {sorted(nc) or 'boot not captured'}")
print()
if O["px"] and N["px"]:
    print(f"bytes   {(1 - N['px'] / O['px']) * 100:+.1f}% fewer px/frame  "
          f"({O['px']:.0f} -> {N['px']:.0f})")
for k, lab, unit in (("fps", "fps", ""), ("span", "span", " ms"),
                     ("dma", "dma ", " ms/s"), ("te", "te  ", " ms/s")):
    if O[k] and N[k]:
        print(f"{lab}    {(N[k] / O[k] - 1) * 100:+6.1f}%  "
              f"({O[k]:7.2f}{unit} -> {N[k]:7.2f}{unit})")

