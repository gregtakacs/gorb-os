"""Measure what an x-clipped commit would save, from real panel-mirror frames.

Current shipped cost: every dirty row goes out full width (466 px).
Candidate: same row bands, but each band's column window is the union of its
rows' actual dirty extents, snapped outward to even (CO5300 2px/clock).
Refinement: clip per TILE_ROWS(20)-row tile instead of per band.
"""
import glob
import os
import re
import sys

W = H = 466
PAGE = W * H
TILE = 20
BANDS = 5


def load(p):
    b = open(p, 'rb').read()
    if len(b) != W * H * 2:
        return None
    return memoryview(b).cast('H')


def rows_dirty(a, bb):
    """rows whose u16 pixels differ, plus per-row dirty [x0,x1]."""
    dirty = []
    ext = {}
    for y in range(H):
        base = y * W
        x0 = -1
        x1 = -1
        for x in range(W):
            if a[base + x] != bb[base + x]:
                if x0 < 0:
                    x0 = x
                x1 = x
        if x0 >= 0:
            dirty.append(y)
            ext[y] = (x0, x1)
    return dirty, ext


def merge_runs(rows):
    runs = []
    for y in rows:
        if runs and y == runs[-1][1] + 1:
            runs[-1][1] = y
        else:
            runs.append([y, y])
    return runs


def cap_bands(runs, maxn):
    while len(runs) > maxn:
        best, gap = 0, 10 ** 9
        for i in range(len(runs) - 1):
            g = runs[i + 1][0] - runs[i][1] - 1
            if g < gap:
                gap, best = g, i
        runs[best][1] = runs[best + 1][1]
        del runs[best + 1]
    return runs


def snap_even(r0, r1):
    r0 &= ~1
    if (r1 - r0 + 1) & 1:
        r1 += 1
    return r0, min(r1, H - 1)


def cost_fullwidth(runs):
    return sum(snap_even(a, b)[1] - snap_even(a, b)[0] + 1 for a, b in runs) * W


def cost_clipped(runs, ext, per_tile):
    tot = 0
    for a, b in runs:
        if per_tile:
            for t0 in range(a, b + 1, TILE):
                t1 = min(b, t0 + TILE - 1)
                xs = [ext[y] for y in range(t0, t1 + 1) if y in ext]
                if not xs:
                    continue
                lo = min(x[0] for x in xs)
                hi = max(x[1] for x in xs)
                lo &= ~1
                if (hi - lo + 1) & 1:
                    hi += 1
                tot += (t1 - t0 + 1) * (min(hi, W - 1) - lo + 1)
        else:
            xs = [ext[y] for y in range(a, b + 1) if y in ext]
            if not xs:
                continue
            lo = min(x[0] for x in xs)
            hi = max(x[1] for x in xs)
            lo &= ~1
            if (hi - lo + 1) & 1:
                hi += 1
            tot += (b - a + 1) * (min(hi, W - 1) - lo + 1)
    return tot


def main(paths):
    tot = {'rows': 0, 'full': 0, 'band': 0, 'tile': 0, 'true': 0}
    frames = 0
    for p in paths:
        a, b = load(p[0]), load(p[1])
        if a is None or b is None:
            print(f"  skip {p[0]}: not a {W*H*2} B mirror")
            continue
        dirty, ext = rows_dirty(a, b)
        if not dirty:
            continue
        runs = cap_bands(merge_runs(dirty), BANDS)
        tot['rows'] += len(dirty)
        tot['full'] += cost_fullwidth(runs)
        tot['band'] += cost_clipped(runs, ext, False)
        tot['tile'] += cost_clipped(runs, ext, True)
        tot['true'] += sum(x1 - x0 + 1 for x0, x1 in ext.values())
        frames += 1
    if not frames:
        print("no comparable frame pairs")
        return
    print(f"\n{frames} consecutive frame pairs\n")
    print(f"  dirty rows/frame        {tot['rows']/frames:7.1f}")
    print(f"  SHIPPED full-width      {tot['full']/frames:11,.0f} px  = {100*tot['full']/frames/PAGE:5.1f}% of panel")
    print(f"  clipped per band        {tot['band']/frames:11,.0f} px  = {100*tot['band']/frames/PAGE:5.1f}%  ({100*(1-tot['band']/tot['full']):.0f}% less)")
    print(f"  clipped per 20-row tile {tot['tile']/frames:11,.0f} px  = {100*tot['tile']/frames/PAGE:5.1f}%  ({100*(1-tot['tile']/tot['full']):.0f}% less)")
    print(f"  true dirty ink (floor)  {tot['true']/frames:11,.0f} px  = {100*tot['true']/frames/PAGE:5.1f}%")


if __name__ == '__main__':
    pairs = []
    args = sys.argv[1:]
    for i in range(0, len(args) - 1, 2):
        pairs.append((args[i], args[i + 1]))
    main(pairs)
