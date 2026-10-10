#!/usr/bin/env python3
"""Analyze a DISPLAY_CADENCE_PROBE serial capture.

Reads CAD f=... frame records, CAD tk=... tick lines and the per-second
'N fps' lines; answers the item-2 question: is the 7 fps average uniform
flush cost or spike-cadence, do spikes correlate with pushed px, and do
they phase-lock to the 125 ms GIF ladder (DMA queue collision at frame
arrival)?

Usage:
    cadence.py capture.log [--csv LABEL]

--csv prints one machine-readable record line for the bench results table
(point, frames, ticks, mean fps, mean flush ms, gap p50/p90/p99, hook, px,
bands, te, dma) instead of the full report. The log-prefix timestamps are
ms since boot; the CAD record fields are us. Do not mix them.
"""
import re, sys
from statistics import mean, median

src = open(sys.argv[1], encoding="utf-8", errors="replace").read()

# ESP log: 'I ( 12345) tag: message'
fr = []
for m in re.finditer(r"\(\s*(\d+)\)\s+\S+:\s+CAD f=(\d+) gap=(\d+) dur=(\d+) idle=(\d+) "
                     r"te=(\d+) dma=(\d+) sub=(\d+) mir=(\d+) hook=(\d+) px=(\d+) "
                     r"ch=(\d+) bands=(\d+)", src):
    g = list(map(int, m.groups()))
    fr.append(dict(ts=g[0], f=g[1], gap=g[2], dur=g[3], idle=g[4], te=g[5],
                   dma=g[6], sub=g[7], mir=g[8], hook=g[9], px=g[10],
                   ch=g[11], bands=g[12]))
tk = []
for m in re.finditer(r"\(\s*(\d+)\)\s+\S+:\s+CAD tk now=(\d+) due=(\d+) gif_due=(\d+) shown=(-?\d+)", src):
    g = list(map(int, m.groups()))
    tk.append(dict(ts=g[0], now=g[1], due=g[2], gif_due=g[3], shown=g[4]))
fps = [int(x) for x in re.findall(r"(\d+) fps \| flush frames", src)]
# Per-second flush cost as booked by display's own frame timer (ms).
fl = [int(x) for x in re.findall(r"flush frames \d+ avg (\d+) ms", src)]

print(f"frames {len(fr)}  ticks {len(tk)}  secs {len(fps)}  "
      f"fps line histogram {sorted(set(fps))} x{[fps.count(s) for s in sorted(set(fps))]}")
if len(fr) < 30:
    sys.exit("too few frame records")

def q(vals, p):
    v = sorted(vals); return v[min(len(v) - 1, int(p * len(v)))]

# The log-prefix stamp is ms since boot (esp_log), not us.
span_ms = fr[-1]["ts"] - fr[0]["ts"]
print(f"real fps {len(fr) / (span_ms / 1000):.2f} over {span_ms / 1000:.1f} s")

# Boot records carry the first-frame artefacts (the 530 ms gap and the
# over-long opening commit). Steady-state numbers drop the first 5 s.
steady = [r for r in fr if r["ts"] - fr[0]["ts"] >= 5000]
if len(steady) < 20:
    steady = fr
print(f"steady window: {len(steady)} of {len(fr)} frames (t+5 s on)")

if "--csv" in sys.argv:
    lbl = sys.argv[sys.argv.index("--csv") + 1]
    sg = [r["gap"] for r in steady]
    cols = [lbl, len(steady), len(tk),
            f"{len(steady) / ((steady[-1]['ts'] - steady[0]['ts']) / 1000):.2f}",
            mean(fl) if fl else 0,
            mean(sg) / 1000, q(sg, .5) / 1000, q(sg, .9) / 1000, q(sg, .99) / 1000,
            mean([r["hook"] for r in steady]) / 1000,
            mean([r["idle"] for r in steady]) / 1000,
            mean([r["te"] for r in steady]) / 1000,
            mean([r["dma"] for r in steady]) / 1000,
            mean([r["px"] for r in steady]),
            mean([r["ch"] for r in steady]),
            mean([r["bands"] for r in steady])]
    print("CSV point,frames,ticks,fps,flush_avg_ms,gap_mean_ms,gap_p50,gap_p90,gap_p99,"
          "hook_ms,idle_ms,te_ms,dma_ms,px,ch,bands")
    print("CSV " + ",".join(f"{c:.1f}" if isinstance(c, float) else str(c) for c in cols))
    sys.exit(0)


for key in ("gap", "dur", "idle", "te", "dma", "hook", "mir", "sub", "px", "ch", "bands"):
    v = [r[key] for r in fr]
    print(f"{key:>5}: min {min(v):7} p50 {q(v,.5):7} p90 {q(v,.9):7} "
          f"p99 {q(v,.99):7} max {max(v):7} mean {mean(v):9.0f}")

gaps = [r["gap"] for r in fr]
lo = sum(1 for x in gaps if x < 135_000); mid = sum(1 for x in gaps if 135_000 <= x < 160_000)
hi = sum(1 for x in gaps if x >= 160_000)
print(f"\ngap modes: <135ms {lo}  135-160ms {mid}  >=160ms {hi}"
      f"   (uniform would need no 160+ tail while 125ms is the target)")

print("\nhistogram gap (5 ms buckets):")
h = {}
for x in gaps: h[x // 5000] = h.get(x // 5000, 0) + 1
for b in sorted(h): print(f"  {b*5:4}-{b*5+5:4} ms: {'#' * h[b]} ({h[b]})")

print("\ntop-10 gap frames:")
for r in sorted(fr, key=lambda r: -r["gap"])[:10]:
    print(f"  ts {r['ts']:9} gap {r['gap']//1000:4} dur {r['dur']//1000:4} "
          f"idle {r['idle']//1000:4} te {r['te']//1000:3} dma {r['dma']//1000:4} "
          f"hook {r['hook']//1000:3} sub {r['sub']//1000:4} px {r['px']:7} ch {r['ch']} bands {r['bands']}")

def corr(a, b):
    ma, mb = mean(a), mean(b)
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    den = (sum((x - ma) ** 2 for x in a) * sum((y - mb) ** 2 for y in b)) ** 0.5
    return num / den if den else 0.0

print("\ncorrelations with gap:")
for key in ("px", "te", "dma", "hook", "mir", "ch", "bands"):
    print(f"  gap ~ {key:>5}: r {corr(gaps, [r[key] for r in fr]):+.2f}")
# dur decomposition: what share of the average gap is each measured phase
mg = mean(gaps); parts = {k: mean([r[k] for r in fr]) for k in
                          ("idle", "te", "dma", "hook", "mir")}
print(f"  mean gap {mg:,.0f} us; idle {parts['idle']:,.0f} | hook {parts['hook']:,.0f} "
      f"te {parts['te']:,.0f} dma-submit {parts['dma']:,.0f} mir {parts['mir']:,.0f} "
      f"| unbooked {mg - sum(parts.values()):,.0f} (LVGL re-render between chunks + ISR lag)")

# GIF ladder phase: rebuild the 125 ms deadline grid from tick lines
if len(tk) >= 8:
    step = round((tk[-1]["due"] - tk[0]["due"]) / (len(tk) - 1)) if len(tk) > 1 else 125
    g0 = tk[0]["gif_due"] - tk[0]["now"]      # grid phase origin, ms clock
    print(f"\ntick ladder: step {step} ms; inter-now deltas p50 {q([b['now']-a['now'] for a,b in zip(tk,tk[1:])],.5)} ms")
    late = [b["now"] - a["due"] for a, b in zip(tk, tk[1:])]
    print(f"tick lateness (now - prev due): p50 {q(late,.5)} p90 {q(late,.9)} max {max(late)} ms")
    ph = {}
    for r in fr:
        p = ((r["ts"] - g0) % 125) * 8 // 10  # 12.5 ms buckets, 0..9
        ph.setdefault(p, []).append(r["gap"])
    print("gap by done-phase within GIF 125 ms grid (12.5 ms buckets):")
    for p in sorted(ph):
        print(f"  {p*12.5:5.1f}-{p*12.5+12.5:5.1f} ms: n {len(ph[p]):3} "
              f"mean gap {mean(ph[p]):8.0f} us")
    # gif frame index of the advance driving each frame = which shown was live
    adv = [(b["shown"], b["now"]) for b in tk]
    by_shown = {}
    for r in fr:
        cand = [min(((s, t0) for s, t0 in adv),
                   key=lambda st: abs(st[1] - (r["ts"] - r["gap"])))]
        if cand: by_shown.setdefault(cand[0], []).append(r["gap"])
    for s in sorted(by_shown):
        v = by_shown[s]
        print(f"  gif frame {s}: n {len(v):3} mean gap {mean(v):8.0f} p90 {q(v,.9):8} us")
