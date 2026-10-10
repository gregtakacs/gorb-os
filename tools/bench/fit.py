#!/usr/bin/env python3
"""Fit the fps-budget model flush_ms = t0 + b*rows + c*px over the bench points.

Reads the CSV that tools/bench/cadence.py --csv appends (/tmp/bench/results.csv),
repeats points with the same label stem, and least-squares fits the mean per-tick
flush against the two drivers:

  rows   full-width rows the commit DMA's   (CSV `rows`, = CAD px / 466)
  px     blended px the hook composited     (CSV `blend_px_tick` + `bench_ink_tick`)

Reports coefficients with R^2, the per-label repeat spread (the plan demands
+-5 ms agreement between repeats), the residuals, and the (rows, px) frontier
where the mean flush reaches the 125 ms / 8 fps target.

Usage: fit.py results.csv [--skip P1-base] [--target 125]
"""
import argparse
import sys
from collections import defaultdict


def read_csv(path):
    rows = []
    hdr = None
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('point,'):
            hdr = line.split(',')
            continue
        if line.startswith('CSV point,'):
            hdr = line[4:].split(',')
            continue
        if line.startswith('CSV '):
            line = line[4:]
        f = line.split(',')
        if hdr is None or len(f) != len(hdr):
            continue
        d = dict(zip(hdr, f))
        try:
            rows.append({k: (v if k in ('point', 'note') else float(v))
                         for k, v in d.items()})
        except ValueError:
            continue
    return rows


COLS = ('rows', 'blend_px_tick', 'bench_ink_tick')


def fit(points, target):
    # Four populations, because one c cannot price them together: a real layer
    # blend does a rotated/scattered PSRAM texel fetch, the synthetic ink at
    # partial alpha does a LINEAR fetch + mix, and the opaque control does
    # neither (store only). Pooling them is what put a 28 ms residual on the
    # opaque point and hid the rows term.
    names = ['real-layer blend px', 'synthetic linear-blend px']
    xs, ys = [], []
    for p in points:
        ys.append(p['flush_avg_ms'])
        xs.append((1.0, p.get('rows', 0.0),
                   p.get('blend_px_tick', 0.0) / 1000.0,
                   p.get('bench_ink_tick', 0.0) / 1000.0))
    ncol = 1 + len(COLS)
    A = [[sum(xs[k][i] * xs[k][j] for k in range(len(xs))) for j in range(ncol)]
         + [sum(xs[k][i] * ys[k] for k in range(len(xs)))] for i in range(ncol)]
    a = gauss(A, ncol)
    if a is None:
        print('singular design matrix: need one distinct point per term')
        return None
    pred = [sum(ai * xi for ai, xi in zip(a, x)) for x in xs]
    ybar = sum(ys) / len(ys)
    ss_res = sum((y - p) ** 2 for y, p in zip(ys, pred))
    ss_tot = sum((y - ybar) ** 2 for y in ys) or 1.0
    print(f'flush_ms = {a[0]:.2f} + {a[1] * 1000:.1f} us/row '
          + ' + '.join(f'{a[i + 2] * 1000:.1f} ns per {names[i]} px'
                       for i in range(len(names))))
    print(f'R^2 = {1 - ss_res / ss_tot:.4f} over {len(ys)} points; '
          f'max |residual| {max(abs(y - p) for y, p in zip(ys, pred)):.2f} ms')
    print('\npoint            rows  real_px(k) ink_px(k)  flush  predicted  err')
    for p, x, pr in zip(points, xs, pred):
        print(f'{p["point"]:<14} {x[1]:6.1f} {x[2]:9.1f} {x[3]:10.1f} '
              f'{p["flush_avg_ms"]:7.1f} {pr:8.1f} {p["flush_avg_ms"] - pr:7.2f}')
    frontier(a, target)
    layer_table(points)
    contrasts(points)
    return a, pred


CONTRASTS = (
    ('px ladder at 112 blocks (synthetic blends)', 'P2a_px0', 'P2a_px112', 'ink'),
    ('px ladder at 36 blocks', 'P2a_px0', 'P2a_px36', 'ink'),
    ('rows lifted 133, same ink', 'P2a_px36', 'P5_sp36', 'rows'),
    ('rows lifted 162, same ink (top)', 'P2a_px112', 'P5_sp112', 'rows'),
    ('art lever B: carriage beyond r=150 gone', 'P2a_px0', 'P4_vb', 'real'),
    ('art lever A+B: carriage + GIF band', 'P2a_px0', 'P4_vc', 'real'),
    ('art lever A alone (GIF band only)', 'P2a_px0', 'P4_va', 'real'),
)


def contrasts(points):
    """Single-variable deltas, which is what the sweeps were built to produce.

    The joint LS fit cannot separate the row term from the real-blend term on
    these points alone: every art point sits at rows 250-270, and the only
    points with rows far above that lift the real-blend count with them. Each
    pair below moves one driver and holds the other, so its slope is a direct
    measurement rather than a covariance artefact.
    """
    by = {p['point']: p for p in points}
    print('\nsingle-variable contrasts:')
    print('  contrast                                     d_rows  d_px(k)  d_flush'
          '      per-unit')
    for name, a_lbl, b_lbl, drv in CONTRASTS:
        a, b = by.get(a_lbl), by.get(b_lbl)
        if not a or not b:
            continue
        dr = b['rows'] - a['rows']
        dreal = (b['blend_px_tick'] - a['blend_px_tick']) / 1000.0
        dink = (b['bench_ink_tick'] - a['bench_ink_tick']) / 1000.0
        df = b['flush_avg_ms'] - a['flush_avg_ms']
        if drv == 'rows':
            per = f'{df * 1000 / dr:6.1f} us/row' if dr else 'n/a'
        else:
            d = (dreal if drv == 'real' else dink) * 1000.0
            per = f'{df * 1e6 / d:6.1f} ns/px ' if d else 'n/a      '
        print(f'  {name:<42} {dr:7.1f} {dreal + dink:8.1f} {df:8.1f}  {per}')
    o, r, base = by.get('P2o_px112'), by.get('P2a_px112'), by.get('P2a_px0')
    if o and r and base:
        ink = r['bench_ink_tick'] / 1000.0
        ro = o['flush_avg_ms'] - base['flush_avg_ms']
        rr = r['flush_avg_ms'] - base['flush_avg_ms']
        print(f'  opaque store vs alpha blend at {ink:,.0f}k px/tick: '
              f'{ro:.1f} ms ({ro * 1e6 / (ink * 1000):.0f} ns/px) vs '
              f'{rr:.1f} ms ({rr * 1e6 / (ink * 1000):.0f} ns/px)')


def frontier(a, target):
    """What the fit allows, at the shipped real-layer blend count."""
    base = 177129.0          # blends/tick the shipped art composites
    print(f'\nfrontier for mean flush <= {target:.0f} ms (8 fps), art levers only')
    print('  rows   real-blend px budget   shipped-art ms at this row count')
    for r in (208, 176, 144, 128, 96, 80, 64, 48, 32):
        fixed = a[0] + a[1] * r
        budget = (target - fixed) * 1000.0 / a[2] if a[2] > 0 else 0.0
        now = fixed + base / 1000.0 * a[2]
        print(f'  {r:4d}   {budget:15,.0f}   {now:14.1f} ms')
    print('  (negative budget = the row term alone already busts the target)')


LAYERS = (('hr', 'blend_hr', 'ms_hr'), ('min', 'blend_min', 'ms_min'),
          ('sec', 'blend_sec', 'ms_sec'), ('dial', 'blend_dial', 'ms_dial'),
          ('cw', 'blend_cw', 'ms_cw'), ('glass', 'blend_glass', 'ms_glass'))


def layer_table(points):
    """Per-layer blend price, straight off the fps line's per-layer split.

    The single fitted c is a blend of populations; this says which layer's
    pixels each art lever actually removes, and what one of ITS blends costs.
    """
    print('\nper-layer blend price (fps-line split; rows/px from the same capture):')
    print('  point            layer  blends/tick  ms/frame  ns/blend')
    for p in points:
        if 'blend_hr' not in p:
            continue
        fps = max(1.0, p['fps'])
        for name, bp, ms in LAYERS:
            b = p.get(bp, 0.0)
            if b < 500:
                continue
            ms_f = p.get(ms, 0.0) / fps
            print(f'  {p["point"]:<14} {name:<6} {b:11,.0f} {ms_f:9.2f} '
                  f'{ms_f * 1e6 / b:9.0f}')


def gauss(A, n):
    M = [r[:] for r in A]
    for i in range(n):
        p = max(range(i, n), key=lambda r: abs(M[r][i]))
        if abs(M[p][i]) < 1e-12:
            return None
        M[i], M[p] = M[p], M[i]
        for r in range(n):
            if r == i:
                continue
            f = M[r][i] / M[i][i]
            for c in range(i, n + 1):
                M[r][c] -= f * M[i][c]
    return [M[i][n] / M[i][i] for i in range(n)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--skip', action='append', default=[])
    ap.add_argument('--target', type=float, default=125.0)
    a = ap.parse_args()
    pts = [p for p in read_csv(a.csv) if p['point'] not in a.skip
           and p.get('flush_avg_ms')]
    by = defaultdict(list)
    for p in pts:
        by[p['point']].append(p)
    print('repeat spread (flush_avg_ms) — the plan requires +-5 ms:')
    for k, v in sorted(by.items()):
        if len(v) > 1:
            f = [x['flush_avg_ms'] for x in v]
            print(f'  {k:<14} n={len(f)} ' + ' '.join(f'{x:.1f}' for x in f) +
                  f'   spread {max(f) - min(f):.1f} ms'
                  + ('  OK' if max(f) - min(f) <= 5 else '  TOO WIDE'))
    reps = []
    for k, v in sorted(by.items()):
        m = dict(v[0])
        m['flush_avg_ms'] = sum(x['flush_avg_ms'] for x in v) / len(v)
        m['hook_ms'] = sum(x['hook_ms'] for x in v) / len(v)
        m['rows'] = sum(x['rows'] for x in v) / len(v)
        m['blend_px_tick'] = sum(x['blend_px_tick'] for x in v) / len(v)
        m['bench_ink_tick'] = sum(x['bench_ink_tick'] for x in v) / len(v)
        reps.append(m)
    print()
    fit(reps, a.target)


if __name__ == '__main__':
    sys.exit(main())
