#!/usr/bin/env python3
"""Report for tools/bench/flights_bench.sh: one row per arm.

    tools/bench/flights_report.py OUTDIR

Reads serial.txt (capf.py lines carry a host epoch prefix), arms.tsv (arm start
times) and the snap-*.json boundary snapshots. Per arm it prices:

  dial   — the per-second 'clock N fps' line: mean, p5, min; RUNG moves; and the
           scheduler's tick-gap histogram over the arm (snapshot deltas): the
           share of ticks that held more than 1.5x / 2x the beat.
  feed   — FLT lines: polls ok/fail/skip, handshakes, open/total ms, bytes, and
           the internal heap low-water each poll saw; plus the dial's gaps
           counted DURING polls, so a cost the feed causes cannot hide in an
           arm-wide average.
  radio  — disconnects, no-lease drops, esp-aes failures, aborts.
"""
import json, os, re, statistics, sys

out = sys.argv[1]
arms = []
for line in open(os.path.join(out, "arms.tsv")):
    p = line.rstrip("\n").split("\t")
    arms.append((p[0], float(p[1])))
bounds = [(arms[i][0], arms[i][1], arms[i + 1][1]) for i in range(len(arms) - 1)]

lines = []
for l in open(os.path.join(out, "serial.txt"), errors="replace"):
    m = re.match(r"(\d+\.\d+) (.*)", l)
    if m:
        lines.append((float(m.group(1)), m.group(2)))


def arm_of(t):
    for n, a, b in bounds:
        if a <= t < b:
            return n
    return None


def snap(name):
    try:
        return json.load(open(os.path.join(out, f"snap-{name}.json")))
    except Exception:
        return None


def pct(v, p):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * len(v)))]


rows = {n: dict(fps=[], rung=0, flt=[], skips=0, radio=dict(disc=0, nolease=0, aes=0, abort=0, ring=0))
        for n, _, _ in bounds}
for t, s in lines:
    n = arm_of(t)
    if not n:
        continue
    r = rows[n]
    m = re.search(r"clock: clock (\d+) fps", s)
    if m:
        r["fps"].append(int(m.group(1)))
    if "RUNG " in s:
        r["rung"] += 1
    m = re.search(r"FLT (ok|FAIL) (-?\d+) (reused|NEW) open=(\d+) ttfb=(\d+) total=(\d+) B=(\d+) "
                  r"ac=(\d+) kept=(\d+) int_min=(\d+) lg_min=(\d+) \| dial ticks=(\d+) >1.5x=(\d+) "
                  r">2x=(\d+) >3x=(\d+)", s)
    if m:
        g = m.groups()
        r["flt"].append(dict(ok=g[0] == "ok", status=int(g[1]), new=g[2] == "NEW", open=int(g[3]),
                             total=int(g[5]), bytes=int(g[6]), int_min=int(g[9]), lg_min=int(g[10]),
                             ticks=int(g[11]), j15=int(g[12]), j2=int(g[13]), j3=int(g[14])))
    if "FLT skip" in s:
        r["skips"] += 1
    if "disassociated" in s or "wifi:state: run -> init" in s:
        r["radio"]["disc"] += 1
    if "no lease" in s:
        r["radio"]["nolease"] += 1
    if "esp-aes: Failed" in s:
        r["radio"]["aes"] += 1
    if "abort()" in s or "Guru Meditation" in s:
        r["radio"]["abort"] += 1
    if "re-funded short" in s:
        r["radio"]["ring"] += 1

print(f"{'arm':8} {'min':>5} | {'fps mean':>8} {'p5':>3} {'min':>3} {'rung':>4} | "
      f"{'ticks':>6} {'>1.5x':>6} {'>2x':>6} {'>3x':>5} {'max':>5} | "
      f"{'polls':>5} {'ok':>4} {'skip':>4} {'hs':>3} {'open':>5} {'tot p50':>7} {'tot max':>7} "
      f"{'KB':>5} {'int_min':>7} {'lg_min':>6} | {'poll ticks':>10} {'>1.5x':>5} {'>2x':>4} | radio")
for n, a, b in bounds:
    r = rows[n]
    s0, s1 = snap(f"{n}-start"), snap(f"{n}-end")
    if s0 and s1:
        j0, j1 = s0["jank"], s1["jank"]
        d = {k: j1[k] - j0[k] for k in ("ticks", "le125", "le150", "le200", "le300", "gt300")}
        over15 = d["le200"] + d["le300"] + d["gt300"]
        over2 = d["le300"] + d["gt300"]
        jt = f"{d['ticks']:6d} {100*over15/max(1,d['ticks']):5.1f}% {100*over2/max(1,d['ticks']):5.2f}% " \
             f"{100*d['gt300']/max(1,d['ticks']):4.2f}% {j1['max_gap_ms']:5d}"
    else:
        jt = " " * 36
    f = r["flt"]
    ok = sum(x["ok"] for x in f)
    hs = sum(x["new"] for x in f)
    tot = [x["total"] for x in f if x["ok"]]
    opn = [x["open"] for x in f if x["new"]]
    pt = sum(x["ticks"] for x in f)
    p15 = sum(x["j15"] for x in f)
    p2 = sum(x["j2"] for x in f)
    fps = r["fps"]
    rad = " ".join(f"{k}={v}" for k, v in r["radio"].items() if v) or "clean"
    print(f"{n:8} {(b-a)/60:5.1f} | {statistics.mean(fps) if fps else float('nan'):8.2f} "
          f"{pct(fps,5):3} {min(fps) if fps else 0:3} {r['rung']:4} | {jt} | "
          f"{len(f)+r['skips']:5} {ok:4} {r['skips']:4} {hs:3} "
          f"{(statistics.median(opn) if opn else 0):5.0f} {(statistics.median(tot) if tot else 0):7.0f} "
          f"{(max(tot) if tot else 0):7} {(statistics.mean([x['bytes'] for x in f])/1024 if f else 0):5.1f} "
          f"{(min(x['int_min'] for x in f) if f else 0):7} {(min(x['lg_min'] for x in f) if f else 0):6} | "
          f"{pt:10} {100*p15/max(1,pt):4.1f}% {100*p2/max(1,pt):3.1f}% | {rad}")
print()
print("internal heap at the arm boundaries (a snapshot can land mid-poll):")
for n, a, b in bounds:
    s0, s1 = snap(f"{n}-start"), snap(f"{n}-end")
    if s0 and s1:
        h0, h1 = s0["heap"], s1["heap"]
        print(f"  {n:8} start free {h0['int_free']:6} largest {h0['int_largest']:6} | "
              f"end free {h1['int_free']:6} largest {h1['int_largest']:6} | lowwater since boot {h1['int_lowwater']}"
              + (f" | rung {h1 and s1['jank']['rung']} moves {s1['jank']['rung_moves']} handshake-seconds forgiven {s1['jank'].get('burst_ignored', '-')}"))
fails = [(n, x) for n, *_ in bounds for x in rows[n]["flt"] if not x["ok"]]
for n, x in fails[:10]:
    print(f"FAIL in {n}: status {x['status']} total {x['total']} ms bytes {x['bytes']}")
