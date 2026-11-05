#!/usr/bin/env python3
"""grid_matrix.py — gears and hands on the 60 Hz grid, per template (plan §49.7).

The panel refreshes at ~59 Hz (16.94 ms), so a frame rate holds steady only where it
divides the refresh: every frame then stays up the same number of refreshes. This stores
each gear or carriage template on the bound orb and runs, with ink spans, nearest shadows
and a truly sweeping seconds hand:

  today 8         the shipped next-edge present at a forced 8 fps beat (the wobble baseline)
  paced 8         pace 7.5 refreshes: 7/8 alternating, the steadiest 8 fps there is
  N on M          hands paced at M fps (60/M refreshes), a gear frame on every (M/N)th frame

and reports, from the device's presentation log, how many refreshes each frame and each
gear frame stayed up. Clean = every frame holds its nominal count.

Usage: tools/bench/grid_matrix.py BAKES OUT.json [preset ...]
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import theme_matrix as tm   # noqa: E402  (store/wait helpers, the same device)

GEAR = ["skeleton", "calendar", "modern", "regulator", "opentour", "kitetour", "orbital"]
ARMS = [
    # label,          query
    ("today 8",       "rate=8"),
    ("paced 8, g4",   "pace=75&gif_every=2"),
    ("4 on 12",       "pace=50&gif_every=3"),
    ("6 on 12",       "pace=50&gif_every=2"),
    ("3 on 15",       "pace=40&gif_every=5"),
    ("5 on 15",       "pace=40&gif_every=3"),
    ("4 on 20",       "pace=30&gif_every=5"),
    ("10 on 20",      "pace=30&gif_every=2"),
    ("3 on 6",        "pace=100&gif_every=2"),
    ("4 on 4",        "pace=150&gif_every=1"),
]


def main():
    bakes, out = sys.argv[1], sys.argv[2]
    ids = sys.argv[3:] or GEAR
    res = json.load(open(out)) if os.path.exists(out) else {}
    for pid in ids:
        path = os.path.join(bakes, pid + ".orbtheme")
        name = tm.theme_name(path)
        sd = tm.get("/api/v1/sd")
        if not (sd.get("source") == "slot" and sd.get("active") == name):
            tm.post("/api/v1/sd/theme?overwrite=1", open(path, "rb").read())
            if not tm.wait_up(name):
                print("%-10s never came up on its slot" % pid, flush=True)
                continue
            try:
                tm.post("/api/v1/apps/clear?id=clock", timeout=10)
            except Exception:
                pass
            time.sleep(12)
        tm.get("/api/v1/screen?to=clock", timeout=10)
        time.sleep(5)
        rec = res.get(pid, {"preset": pid, "arms": {}})
        try:
            for label, q in ARMS:
                r = tm.get("/api/v1/debug/dial?reset=1&spans=1&shadow=nearest&sweep=1&%s&settle=2500&ms=8000" % q)
                rec["arms"][label] = r
                h = r["hold"]
                gh = [(i + 1, v) for i, v in enumerate(r["gear_hold"]) if v]
                print("%-10s %-12s %5.2f fps  gears %5.2f  interval %6.1f +- %5.1f ms  holds %s  gear holds %s"
                      % (pid, label, r["commit_fps"], r["gif_fps"], r["interval_ms"][0], r["interval_ms"][1],
                         " ".join("%dx%d" % (v, i + 1) for i, v in enumerate(h) if v), gh), flush=True)
        finally:
            tm.get("/api/v1/debug/dial?reset=1&settle=0&ms=0")
        res[pid] = rec
        json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
