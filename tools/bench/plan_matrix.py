#!/usr/bin/env python3
"""plan_matrix.py — hold estimate4's plans against the glass, per template (plan §49.15).

For each baked template, estimate4 (tools/bench/plans4.js) judges every plan: gears at G
on a hand grid of H fps. This stores the template on the bound orb and runs, on the bench
compositor (ink spans, nearest shadows, the paced 60 Hz grid, pre-paint where the plan
says so):

  frontier   every plan the calculator says fits and nothing beats — each must hold
  next up    the cheapest plans it refuses for TIME (a frame or a gear frame that does not
             fit, a pre-paint the slack cannot build) — each must miss, or the frontier is
             not the maximum the orb achieves

A plan holds when at least 95% of frames stay up exactly 60/H refreshes and the gears
deliver at least 90% of G. Refusals for a reason the glass cannot test (a beat or the
gears not landing on the grid, the slot, the device's bank caps) are not run.

Usage: [BENCH_MS=60000] [ARMS="pid:G/H ..."] tools/bench/plan_matrix.py BAKES OUT.json [preset ...]
"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import theme_matrix as tm   # noqa: E402  (store/wait helpers, the same device)

HERE = os.path.dirname(os.path.abspath(__file__))
NODE = os.environ.get("NODE") or os.path.expanduser("~/.local/share/nodeenv-gorb/bin/node")
SCAN = 16.94
TIME_WHY = ("does not fit", "leave", "landing")
# BENCH_MS=60000 holds each plan a whole minute, so the seconds hand crosses the whole dial;
# ARMS="station:0/20 perpetual:0/20" runs only those
MS = int(os.environ.get("BENCH_MS", "8000"))
ONLY = set(os.environ.get("ARMS", "").split())


def plans_of(path):
    return json.loads(subprocess.run([NODE, os.path.join(HERE, "plans4.js"), path],
                                     capture_output=True, text=True, check=True).stdout)


def arms_of(p):
    front = [dict(x, arm="frontier") for x in p["plans"]]
    timed = [x for x in p["judged"] if not x["ok"] and any(w in (x["why"] or "") for w in TIME_WHY)]
    nxt = [x for x in timed
           if not any(y is not x and y["fps"] <= x["fps"] and y["gears"] <= x["gears"] for y in timed)
           and any(x["fps"] >= f["fps"] and x["gears"] >= f["gears"] for f in front)]
    # a beat-only dial (a tick, no gears) commits on its beat whatever the grid: nothing to hold
    nxt = [x for x in nxt if x["gears"] or p["mode"] == "sweep"]
    return front + [dict(x, arm="next up") for x in nxt]


def tempo_of(g):
    return 8 if abs(g - 7.5) < 0.01 else int(round(g))


def run_arm(x, mode, carriage):
    H, G = x["fps"], x["gears"]
    scans = round(60 / H)
    every = round(H / G) if G else 0
    prep = 1 if (x["prepaint"] or (x["arm"] == "next up" and every >= 2 and not carriage)) else 0
    q = ("reset=1&spans=1&shadow=nearest&sweep=%d&pace=%d&gif_every=%d&prep=%d&settle=2500&ms=%d"
         % (1 if mode == "sweep" else 0, scans * 10, every, prep, MS))
    r = tm.get("/api/v1/debug/dial?" + q, timeout=40 + MS / 1000)
    h = r["hold"]
    n = sum(h) or 1
    want_fps = 1000.0 / (scans * SCAN)
    want_g = G * want_fps / H if G else 0
    if scans <= len(h):
        at = h[scans - 1]
    else:                       # past the log's last bin (12 scans): the rate is the witness
        at = n if r["commit_fps"] >= 0.95 * want_fps else 0
    holds = at / n >= 0.95 and r["commit_fps"] >= 0.95 * want_fps and (not G or r["gif_fps"] >= 0.9 * want_g)
    return {"key": x["key"], "arm": x["arm"], "prep": prep, "calc_ok": x["ok"], "glass_ok": holds,
            "agree": holds == x["ok"], "commit_fps": r["commit_fps"], "gif_fps": r["gif_fps"],
            "want_fps": round(want_fps, 2), "want_gears": round(want_g, 2), "at_nominal": round(at / n, 3),
            "holds": " ".join("%dx%d" % (v, i + 1) for i, v in enumerate(h) if v),
            "calc_frame_ms": x["frame_ms"], "calc_gear_ms": x["gear_ms"], "period_ms": x["period_ms"],
            "why": x["why"], "raw": r}


_stored = [None]


def store(path):
    """Put this bake on the orb's slot (once per file), clear the clock's strikes, show it."""
    if _stored[0] == path:
        return True
    name = tm.theme_name(path)
    tm.post("/api/v1/sd/theme?overwrite=1", open(path, "rb").read())
    if not tm.wait_up(name):
        return False
    try:
        tm.post("/api/v1/apps/clear?id=clock", timeout=10)
    except Exception:
        pass
    time.sleep(12)
    tm.get("/api/v1/screen?to=clock", timeout=10)
    time.sleep(5)
    _stored[0] = path
    return True


def main():
    bakes, out = sys.argv[1], sys.argv[2]
    ids = sys.argv[3:] or sorted(f[:-9] for f in os.listdir(bakes) if f.endswith(".orbtheme") and "@" not in f)
    res = json.load(open(out)) if os.path.exists(out) else {}
    for pid in ids:
        path = os.path.join(bakes, pid + ".orbtheme")
        p = plans_of(path)
        arms = arms_of(p)
        rec = {"preset": pid, "mode": p["mode"], "carriage": p["carriage"], "rect_kpx": p["rect_kpx"],
               "plans": [x["key"] for x in p["plans"]], "picks": p["picks"], "arms": []}
        try:
            for x in arms:
                if ONLY and "%s:%s" % (pid, x["key"]) not in ONLY:
                    continue
                # a carriage is baked at the gears' tempo, so its plan runs on a bake at that tempo
                # (bake_presets.py with BAKE_QUERY=tempo=N, filed as <preset>@N.orbtheme)
                bake = path
                pin = os.path.join(bakes, "%s@%d.orbtheme" % (pid, tempo_of(x["gears"]))) if x["gears"] else None
                if p["carriage"] and pin and os.path.exists(pin):
                    bake = pin
                elif p["carriage"] and x["gears"] and tempo_of(x["gears"]) != tempo_of(p["bank_fps"] or 0):
                    print("%-10s %-8s no bake at its carriage tempo %d: not run" % (pid, x["key"], tempo_of(x["gears"])), flush=True)
                    continue
                if not store(bake):
                    print("%-10s never came up on its slot" % pid, flush=True)
                    break
                a = run_arm(x, p["mode"], p["carriage"])
                a["bake"] = os.path.basename(bake)
                rec["arms"].append(a)
                print("%-10s %-8s %-9s calc %-4s glass %-4s %s  %5.2f/%5.2f fps  gears %5.2f/%5.2f  nominal %5.1f%%  %s"
                      % (pid, a["key"], a["arm"], "fits" if a["calc_ok"] else "no", "held" if a["glass_ok"] else "miss",
                         "  " if a["agree"] else "!!", a["commit_fps"], a["want_fps"], a["gif_fps"], a["want_gears"],
                         100 * a["at_nominal"], a["holds"]), flush=True)
        finally:
            tm.get("/api/v1/debug/dial?reset=1&settle=0&ms=0")
        res[pid] = rec
        json.dump(res, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
