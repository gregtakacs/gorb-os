"""Decompose the flush span: is the dial transfer-bound or composite-bound?

The 'avg N ms' figure spans first-chunk-entry to last-band-DMA-done, so it
contains LVGL's render time between chunks, not just the QSPI work.
"""
import re
import statistics
import sys

PAT = re.compile(
    r'clock (\d+) fps \| flush frames \d+ avg (\d+) ms \| px/s (\d+).*chunks/s (\d+)'
    r' te/s (\d+) ms submit/s (\d+) ms')
PH = re.compile(
    r'phases ms/s: mirror (\d+) te (\d+) dma (\d+)')

for path in sys.argv[1:]:
    raw = re.sub(r'\x1b\[[0-9;]*m', '', open(path, 'rb').read().decode('utf8', 'replace'))
    lines = raw.splitlines()
    fps, avg, px, ch, te, sub, mir, ph_t, ph_d = [], [], [], [], [], [], [], [], []
    pend = None
    for ln in lines:
        m = PAT.search(ln)
        if m:
            v = [int(x) for x in m.groups()]
            if v[0] >= 6:
                pend = v
                fps.append(v[0]); avg.append(v[1]); px.append(v[2])
                ch.append(v[3]); te.append(v[4]); sub.append(v[5])
            continue
        p = PH.search(ln)
        if p and pend is not None:
            mir.append(int(p.group(1))); ph_t.append(int(p.group(2)))
            ph_d.append(int(p.group(3))); pend = None
    if not fps:
        print(f"{path}: no samples")
        continue
    n = len(fps)
    f = statistics.median(fps)
    a = statistics.median(avg)
    # per-frame: ms/s totals are per second, so divide by the fps that earned them
    per = lambda xs: statistics.median(xs) / f
    print(f"\n== {path}  (n={n})")
    print(f"  med fps {f} | flush span {a:.0f} ms/frame | {statistics.median(px):,.0f} px/s")
    m_ms, t_ms, d_ms, s_ms = per(mir), per(ph_t), per(ph_d), per(sub)
    qspi = m_ms + t_ms + d_ms
    print(f"  QSPI+mirror work : mirror {m_ms:5.1f} + te {t_ms:5.1f} + dma {d_ms:5.1f} = {qspi:5.1f} ms/frame ({100*qspi/a:4.1f}%)")
    print(f"  submit time      : {s_ms:5.1f} ms/frame  ({100*s_ms/a:4.1f}%)")
    print(f"  RESIDUAL (LVGL composite + gaps): {a-qspi:5.1f} ms/frame ({100*(a-qspi)/a:4.1f}%)")
