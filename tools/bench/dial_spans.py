#!/usr/bin/env python3
"""dial_spans.py — the ink-span bench matrix (docs/app-platform-plan.md §49).

Drives GET /api/v1/debug/dial on the bound orb through every arm of the
question: box marks vs ink spans, on the theme's live animated background and
on the same background frozen (a static plate on the same compositor), with
the shipped beat and with a forced 30 fps sweep of the minute hand and its
shadow, and with the glass job dropped (what folding the glass into the
background frames at bake time would save). The clock must be on the glass.

Usage: tools/bench/dial_spans.py [--ip <orb>] [--ms 6000] [--json out.json]
                                [--matrix spans|shadow|multirate|speed]
Leaves every knob reset when it exits, whatever happens.
"""
import argparse
import json
import os
import sys
import urllib.request

ARMS = [
    # label,                         freeze spin rate glass
    ("gears, shipped beat",            0,    0,   0,   1),
    ("gears, 30 fps sweep",            0,    30,  30,  1),
    ("gears, 30 fps sweep, no glass",  0,    30,  30,  0),
    ("frozen, 30 fps sweep",           1,    30,  30,  1),
    ("frozen, 30 fps sweep, no glass", 1,    30,  30,  0),
]


# --matrix shadow: the shadows' bilinear sample against nearest, spans on.
SHADOW_ARMS = [
    # label,                     freeze spin rate shadow
    ("gears, 30 fps sweep",        0,    30,  30, "baked"),
    ("gears, 30 fps sweep",        0,    30,  30, "nearest"),
    ("frozen, 30 fps sweep",       1,    30,  30, "baked"),
    ("frozen, 30 fps sweep",       1,    30,  30, "nearest"),
]

# --matrix multirate: the gears keep their own 4 fps while the seconds row sweeps
# truly at the forced beat; spans on. The question is whether hand-only ticks are
# cheap enough to run between gear frames, and what a gear frame does to the beat.
MULTIRATE_ARMS = [
    # label,                      rate sweep shadow
    ("gears only, beat 8",          8,   0,  "baked"),
    ("seconds sweep, beat 8",       8,   1,  "baked"),
    ("seconds sweep, beat 15",      15,  1,  "baked"),
    ("seconds sweep, beat 30",      30,  1,  "baked"),
    ("seconds sweep, beat 15",      15,  1,  "nearest"),
    ("seconds sweep, beat 30",      30,  1,  "nearest"),
]


# --matrix speed: can the gears, an escapement, or the hands run faster than the
# gear bank's own 4 fps? Spans on; every arm in both shadow samplings. gif = the
# bank played N times faster; sweep = the seconds row sweeps truly; osc = the hour
# row rocks +-N degrees at 2.5 Hz, an escapement-sized proxy.
SPEED_ARMS = [
    # label,                                rate gif sweep osc
    ("gears 4 fps (shipped)",                0,   1,   0,   0),
    ("gears 8 fps",                          8,   2,   0,   0),
    ("gears 12 fps",                         12,  3,   0,   0),
    ("gears 16 fps",                         16,  4,   0,   0),
    ("gears 4 + seconds sweep @8",           8,   1,   1,   0),
    ("gears 4 + seconds sweep @15",          15,  1,   1,   0),
    ("gears 4 + seconds sweep @30",          30,  1,   1,   0),
    ("gears 4 + escapement proxy @15",       15,  1,   0,   10),
    ("gears 4 + escapement proxy @30",       30,  1,   0,   10),
    ("gears 8 + sweep + escapement @16",     16,  2,   1,   10),
]


def call(ip, **q):
    qs = "&".join("%s=%s" % kv for kv in q.items())
    with urllib.request.urlopen("http://%s/api/v1/debug/dial?%s" % (ip, qs), timeout=40) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="the orb to bench (default: this clone's bound orb, .orb-target)")
    ap.add_argument("--ms", type=int, default=6000)
    ap.add_argument("--json")
    ap.add_argument("--matrix", choices=("spans", "shadow", "multirate", "speed"), default="spans")
    a = ap.parse_args()
    if not a.ip:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        import orb_bind
        a.ip = orb_bind.gate_ip(orb_bind.load())
    rows = []
    if a.matrix == "speed":
        try:
            for label, rate, gif, sweep, osc in SPEED_ARMS:
                for shadow in ("baked", "nearest"):
                    r = call(a.ip, reset=1, spans=1, rate=rate, gif=gif, sweep=sweep, osc=osc,
                             shadow=shadow, glass=1, settle=2500, ms=a.ms)
                    if not r.get("l4"):
                        sys.exit("L4 is not live: put the clock on the glass first")
                    r["label"] = label
                    rows.append(r)
                    p, h, g = r["per_commit"], r["commit_ms_hist"], r["tick_gap_x_period"]
                    print("%-34s %-7s gears %5.2f fps  frames %5.2f fps  hook avg %5.1f max %5.1f  "
                          "dma %4.1f  commits<10/20/40/60/90/more %s  late ticks 1.5-2/2-3/>3x %d/%d/%d of %d"
                          % (label, shadow, r["gif_fps"], r["commit_fps"], p["hook_us"] / 1e3,
                             r["max_commit_us"] / 1e3, p["dma_us"] / 1e3,
                             "/".join(str(h[k]) for k in ("lt10", "lt20", "lt40", "lt60", "lt90", "ge90")),
                             g["le200"], g["le300"], g["gt300"],
                             sum(g[k] for k in ("le125", "le150", "le200", "le300", "gt300"))),
                          flush=True)
        finally:
            call(a.ip, reset=1, settle=0, ms=0)
        if a.json:
            with open(a.json, "w") as f:
                json.dump(rows, f, indent=1)
        return
    if a.matrix != "spans":
        try:
            arms = SHADOW_ARMS if a.matrix == "shadow" else MULTIRATE_ARMS
            for arm in arms:
                if a.matrix == "shadow":
                    label, freeze, spin, rate, shadow = arm
                    q = dict(spin=spin, freeze=freeze, sweep=0)
                else:
                    label, rate, sweep, shadow = arm
                    q = dict(spin=0, freeze=0, sweep=sweep)
                r = call(a.ip, reset=1, spans=1, rate=rate, shadow=shadow, glass=1,
                         settle=2000, ms=a.ms, **q)
                if not r.get("l4"):
                    sys.exit("L4 is not live: put the clock on the glass first")
                r["label"] = label
                rows.append(r)
                p, h, g = r["per_commit"], r["commit_ms_hist"], r["tick_gap_x_period"]
                print("%-24s %-7s %5.2f fps  hook avg %5.1f max %5.1f ms  shadow %5.1f ms  "
                      "commits<10/20/40/60/90/more %s  ticks<=1.25/1.5/2/3/more x period %s"
                      % (label, shadow, r["commit_fps"], p["hook_us"] / 1e3,
                         r["max_commit_us"] / 1e3, r["roles"]["shadow_still"][1] / 1e3,
                         "/".join(str(h[k]) for k in ("lt10", "lt20", "lt40", "lt60", "lt90", "ge90")),
                         "/".join(str(g[k]) for k in ("le125", "le150", "le200", "le300", "gt300"))),
                      flush=True)
        finally:
            call(a.ip, reset=1, settle=0, ms=0)
        if a.json:
            with open(a.json, "w") as f:
                json.dump(rows, f, indent=1)
        return
    try:
        for label, freeze, spin, rate, glass in ARMS:
            for spans in (0, 1):
                r = call(a.ip, reset=1, spans=spans, spin=spin, rate=rate, freeze=freeze,
                         glass=glass, settle=2000, ms=a.ms)
                if not r.get("l4"):
                    sys.exit("L4 is not live: put the clock on the glass first")
                r["label"] = label
                rows.append(r)
                p = r["per_commit"]
                print("%-32s %-5s %5.2f fps  hook %6.1f ms  paint %6.1f  other %6.1f  "
                      "restored %6.0f px  rot walk %6.0f px  spans %4.1f ms  esc %d"
                      % (label, "spans" if spans else "box", r["commit_fps"],
                         p["hook_us"] / 1e3, p["paint_us"] / 1e3, p["render_us"] / 1e3,
                         p["restored_px"], p["rot_iter_span_px"] if spans else p["rot_iter_box_px"],
                         p["span_us"] / 1e3, p["escapes"]), flush=True)
    finally:
        call(a.ip, reset=1, settle=0, ms=0)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
