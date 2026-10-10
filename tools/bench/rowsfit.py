#!/usr/bin/env python3
"""Turn the rows-capacity sweep CSV into the fps-vs-animated-rows decision table.

Labels: {A,B,C,D}_r<R> (animated full-width band of R rows), _static (no bg
animation), _real (the kit's own GIF), _top<R>. Configs: A = no carriage, no
second hand; B = no carriage + simple needle second hand (sweeping); C =
carriage r<=150; D = shipped carriage. All ladder points run at the 15-fps
tick; achieved fps reads off the cadence, so the frontier needs no extrapolation.

Prints, per config: measured (rows, flush, fps) per point, a linear fit of
flush on the CAD row count, and the row capacity at each fps gate. The gate is
flush <= 1000/f - guard_ms, guard covering the out-of-window idle/TE/submit the
P1 session measured at ~12-17 ms (docs/fps-budget-findings-2026-10-10.md); pass
--guard to retune. Row capacity is converted back to the largest ANIMATED BAND
using that config's rows-per-band slope (from the r-points themselves), which
is what the art decision actually needs.
"""
import sys

import numpy as np

TARGETS = (4, 8, 12, 15)
DEFAULT_GUARD = 17.0


def read(path):
    rows = [l.rstrip('\n').split(',') for l in open(path) if l.strip()]
    hdr = rows[0]
    out = []
    for r in rows[1:]:
        if not r[0].startswith('CSV '):
            continue
        d = dict(zip(hdr, r))
        d['point'] = d['point'][4:]
        for k in ('fps', 'flush_avg_ms', 'rows', 'blend_px_tick', 'idle_ms'):
            d[k] = float(d[k])
        out.append(d)
    return out


def band_of(point):
    if point.endswith('_static'):
        return 0
    if point.endswith('_real'):
        return None            # the kit's own art: 208-row union, sparse delta
    import re
    m = re.search(r'_r(\d+)$|_top(\d+)$', point)
    return int(m.group(1) or m.group(2)) if m else None


def main(csv_path, guard=DEFAULT_GUARD):
    pts = read(csv_path)
    by_cfg = {}
    for p in pts:
        by_cfg.setdefault(p['point'].split('_')[0], []).append(p)
    print(f'guard={guard} ms  (period - guard = flush budget; measured idle+TE+submit '
          f'was 12-17 ms at 6 fps)')
    for cfg in sorted(by_cfg):
        ps = sorted(by_cfg[cfg], key=lambda p: (band_of(p) or -1) if p['point'].endswith('_real') is False else -2)
        print(f'\n== config {cfg} ==')
        print('  point        band   fps   flush   rows   blends/tick')
        for p in ps:
            print(f"  {p['point']:<12} {str(band_of(p)):>5}  {p['fps']:5.2f} "
                  f"{p['flush_avg_ms']:6.1f}  {p['rows']:5.0f}  {p['blend_px_tick']:8.0f}")
        xy = [(p['rows'], p['flush_avg_ms']) for p in ps if p['rows'] >= 8]
        if len(xy) >= 3:
            (x, y) = zip(*xy)
            b, a = np.polyfit(x, y, 1)
            print(f'  fit: flush = {a:.1f} + {b*1000:.0f} us/row  (n={len(xy)}, '
                  f'rows {min(x):.0f}..{max(x):.0f})')
            for f in TARGETS:
                budget = 1000.0 / f - guard
                cap = (budget - a) / b if b > 0 else float('nan')
                ok = budget >= a
                print(f'  {f:2d} fps: flush<={budget:5.1f} ms -> max DMA rows '
                      f'{cap:5.0f}{" (fixed term already over budget)" if not ok else ""}')
    return 0


if __name__ == '__main__':
    args = [a for a in sys.argv[1:]]
    guard = float(args[args.index('--guard') + 1]) if '--guard' in args else DEFAULT_GUARD
    raise SystemExit(main(args[0] if args and not args[0].startswith('--') else
                          '/tmp/bench2/results.csv', guard))
