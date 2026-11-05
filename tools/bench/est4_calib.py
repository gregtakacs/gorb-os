#!/usr/bin/env python3
"""est4_calib.py — measure estimate4's terms on the glass, for its refit (plan §49.15).

Runs each baked template on the bound orb as it ships (ink spans, its own smoothing flags,
the paced grid, no pre-paint) across a spread of direct plans — gears on every frame, and the
sweep alone — and records the device's mean commit (hook + DMA) per frame. tools/bench/
est4_fit.js then solves estimate4's coefficients against these.

Usage: tools/bench/est4_calib.py BAKES OUT.json [preset ...]
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import plan_matrix as pm   # noqa: E402  (store + the same device)
import theme_matrix as tm  # noqa: E402

GEAR_ARMS = ["4/4", "6/6", "7.5/7.5", "10/10", "15/15", "20/20", "4/20"]
STILL_ARMS = ["0/10", "0/20"]
MS = int(os.environ.get("BENCH_MS", "20000"))


def main():
    bakes, out = sys.argv[1], sys.argv[2]
    ids = sys.argv[3:] or sorted(f[:-9] for f in os.listdir(bakes) if f.endswith(".orbtheme") and "@" not in f)
    res = json.load(open(out)) if os.path.exists(out) else {}
    for pid in ids:
        path = os.path.join(bakes, pid + ".orbtheme")
        p = pm.plans_of(path)
        arms = GEAR_ARMS if p["rect_kpx"] else STILL_ARMS
        if not p["rect_kpx"] and p["mode"] != "sweep":
            # a ticking dial with no gears (Orbit's rings, Rink's): its seconds forced to sweep, so
            # the fit sees a turning ring frame by frame ("s": est4_fit.js prices it hands=sweep)
            arms = ["0/4s", "0/6s", "0/10s", "0/15s"]
        if p["carriage"]:          # a carriage moves at its baked tempo: only plans on that tempo
            t = pm.tempo_of(p["bank_fps"])
            g = 7.5 if t == 8 else t
            arms = ["%g/%g" % (g, g), "%g/%g" % (g, 2 * g)] if 2 * g <= 20 else ["%g/%g" % (g, g)]
        if not pm.store(path):
            print("%-10s never came up" % pid, flush=True)
            continue
        rec = res.get(pid, {})
        for key in arms:
            forced = key.endswith("s")
            g, h = (float(v) for v in key.rstrip("s").split("/"))
            scans = round(60 / h)
            every = round(h / g) if g else 0
            q = ("reset=1&spans=1&sweep=%d&pace=%d&gif_every=%d&prep=0&settle=2500&ms=%d"
                 % (1 if p["mode"] == "sweep" or forced else 0, scans * 10, every, MS))
            r = tm.get("/api/v1/debug/dial?" + q, timeout=60 + MS / 1000)
            pc = r["per_commit"]
            rec[key] = r
            print("%-10s %-8s commit %6.1f ms (hook %.1f dma %.1f)  fps %5.2f" % (pid, key, (pc["hook_us"] + pc["dma_us"]) / 1000,
                  pc["hook_us"] / 1000, pc["dma_us"] / 1000, r["commit_fps"]), flush=True)
        res[pid] = rec
        json.dump(res, open(out, "w"), indent=1)
    tm.get("/api/v1/debug/dial?reset=1&settle=0&ms=0")


if __name__ == "__main__":
    main()
