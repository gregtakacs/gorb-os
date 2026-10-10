#!/usr/bin/env python3
"""Host-side verifier for the tourbillon orbit marks (no device needed).

Ports, operation-for-operation, the two integer pipelines that must agree on
the firmware:
  - tourb_stamp_pose(): forward-rotates each source row's ink chord into
    per-device-row mark spans (the ROTATED-INK invalidation), and
  - paint_layer()'s nearest-neighbour inverse map: the set of device pixels
    the commit actually BLENDs.

For a tick (shown frame g at angle prev -> frame f at angle new) correctness
demands every pixel either pose can paint lands inside the marked x-span of
its row, else it is neither restored from the canvas nor repainted -> ghost
(vacated pose) or missing ink (incoming pose). Reports the worst violations.

Run:  python3 tools/sim_tourb_mark.py
"""
import math
import re
import sys
import os

ROOT = os.path.join(os.path.dirname(__file__), "..")
HDR = os.path.join(ROOT, "assets", "clock_assets.h")
BIN = os.path.join(ROOT, "assets", "clock_assets.bin")

W = H = 466
SWEPT_PAD = 3

def parse_sprites():
    txt = open(HDR).read()
    m = re.search(r"/\* tourbillon \*/ \{(.*?)\n\};", txt, re.S)
    rows = re.findall(r"\{\s*([-\d ,]+)\},\s*/\* tourb(\d+) \*/", m.group(1))
    frames = []
    for vals, idx in rows:
        v = [int(x) for x in vals.replace(",", " ").split()]
        w, h, offx, offy, pvx, pvy, cx, cy, blend, show, coff, dx, dy, dw, dh = v
        if w == 0:
            continue
        frames.append(dict(w=w, h=h, pvx=pvx, pvy=pvy, cx=cx, cy=cy,
                           coff=coff, dx=dx, dy=dy, dw=dw, dh=dh, n=int(idx)))
    return frames

BLOB = open(BIN, "rb").read()

def alpha_plane(f):
    off = f["coff"] + f["w"] * f["h"] * 2
    w, h = f["w"], f["h"]
    return BLOB[off:off + w * h], w, h

SIN0_90 = [
    0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
    5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580,  10126, 10668,
    11207, 11743, 12275, 12803, 13328, 13848, 14365, 14876, 15384, 15886,
    16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174, 20622,
    21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730,
    25102, 25466, 25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088,
    28378, 28660, 28932, 29197, 29452, 29698, 29935, 30163, 30382, 30592,
    30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928, 32052, 32166,
    32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763,
    32768,
]

def chords(f):
    alp, w, h = alpha_plane(f)
    out = []
    for y in range(h):
        row = alp[y * w:(y + 1) * w]
        lo, hi = -1, -1
        for x in range(w):
            if row[x]: lo = x; break
        for x in range(w - 1, -1, -1):
            if row[x]: hi = x; break
        out.append((1, 0) if lo < 0 else (lo, hi))
    return out

def empty_spans():
    return [(32767, -32768) for _ in range(H)]

_STAMP_CACHE = {}
def stamp(f, chord, ang_10, spans, rpad_over=None):
    """Exact port of tourb_stamp_pose."""
    if rpad_over is not None:
        return _stamp_into(f, chord, ang_10, spans, rpad_over)
    key = (f["n"], ang_10)
    res = _STAMP_CACHE.get(key)
    if res is None:
        res = empty_spans()
        _stamp_into(f, chord, ang_10, res, None)
        _STAMP_CACHE[key] = res
    for y, (a, b) in enumerate(res):
        if b >= a:
            spans[y] = (min(spans[y][0], a), max(spans[y][1], b))

def _stamp_into(f, chord, ang_10, spans, rpad_over):
    pvx, pvy, cxs, cys = f["pvx"], f["pvy"], f["cx"], f["cy"]
    sma, cma = rot_sin_cos(ang_10)
    si, co = -sma * 64, cma * 64
    rpad = (1 + (f["w"] >> 7)) if rpad_over is None else rpad_over
    for ys, (c0, c1) in enumerate(chord):
        if c1 < c0:
            continue
        dy = ys - pvy
        dxa, dxb = c0 - pvx, c1 - pvx
        ax = cxs + ((co * dxa - si * dy) >> 16)
        ay = cys + ((si * dxa + co * dy) >> 16)
        bx = cxs + ((co * dxb - si * dy) >> 16)
        by = cys + ((si * dxb + co * dy) >> 16)
        up = ay <= by
        ya, yb = (ay, by) if up else (by, ay)
        xya, xyb = (ax, bx) if up else (bx, ax)
        den = yb - ya
        y0, y1 = ya - rpad, yb + rpad
        adx = abs(xyb - xya)
        if den == 0 or adx > den:
            xl, xr = min(xya, xyb) - SWEPT_PAD, max(xya, xyb) + SWEPT_PAD
            for y in range(y0, y1 + 1):
                if 0 <= y < H:
                    spans[y] = (min(spans[y][0], xl), max(spans[y][1], xr))
            continue
        step16 = cdiv((xyb - xya) << 16, den)
        x16 = (xya << 16) - step16 * rpad
        for y in range(y0, y1 + 1):
            q0, q1 = x16 >> 16, (x16 + step16) >> 16
            x16 += step16
            if 0 <= y < H:
                xl, xr = min(q0, q1) - SWEPT_PAD, max(q0, q1) + SWEPT_PAD
                spans[y] = (min(spans[y][0], xl), max(spans[y][1], xr))

def rot_rect_box(f, ang_10, rx1, ry1, rx2, ry2):
    rad = ang_10 * (math.pi / 1800.0)
    c, s = math.cos(rad), math.sin(rad)
    x1s = y1s = 1 << 30
    x2s = y2s = -(1 << 30)
    for i in range(4):
        cxp = rx2 if (i & 1) else rx1
        cyp = ry2 if (i & 2) else ry1
        dx = cxp - f["pvx"]
        dy = cyp - f["pvy"]
        px = f["cx"] + (c * dx - s * dy)
        py = f["cy"] + (s * dx + c * dy)
        ix, iy = math.floor(px), math.floor(py)
        x1s, y1s = min(x1s, ix), min(y1s, iy)
        x2s, y2s = max(x2s, ix), max(y2s, iy)
    return (max(0, x1s - SWEPT_PAD), max(0, y1s - SWEPT_PAD),
            min(W - 1, x2s + SWEPT_PAD), min(H - 1, y2s + SWEPT_PAD))

def swept_box(f, ang_10):
    return rot_rect_box(f, ang_10, 0, 0, f["w"] - 1, f["h"] - 1)

def diff_box(f, ang_10):
    return rot_rect_box(f, ang_10, f["dx"], f["dy"],
                        f["dx"] + f["dw"] - 1, f["dy"] + f["dh"] - 1)

def trigo_sin(angle):
    angle %= 360
    if angle < 90:
        ret = SIN0_90[angle]
    elif angle < 180:
        ret = SIN0_90[180 - angle]
    elif angle < 270:
        ret = -SIN0_90[angle - 180]
    else:
        ret = -SIN0_90[360 - angle]
    if ret == 32767:
        return 32768
    if ret == -32767:
        return -32768
    return ret

def cdiv(a, b):                     # C integer division: truncate toward 0
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q

def rot_sin_cos(ang_10):            # exact port of the firmware helper
    tr = -(ang_10 % 3600)
    if tr == 0:
        return 0, 1024
    low = cdiv(tr, 10)
    rem = tr - low * 10
    s1, s2 = trigo_sin(low), trigo_sin(low + 1)
    c1, c2 = trigo_sin(low + 90), trigo_sin(low + 91)
    sinma = cdiv(s1 * (10 - rem) + s2 * rem, 10) >> 5
    cosma = cdiv(c1 * (10 - rem) + c2 * rem, 10) >> 5
    return sinma, cosma

_PAINT_CACHE = {}
def painted_pixels(f, ang_10, filt=False):
    """Device pixels the painter blends for frame f at ang_10 (paint_row_ctx's
    rot path, clipped to its own swept box -- before the row-span clip).

    filt=True mirrors the CLK_TOURB_FILTER branch: the pair of texel rows that
    straddles the unbiased coordinate (ups - 0x80), either of which may carry the
    ink. Those pixels blend too, so the marks have to cover them; the x tap is
    unchanged, so only the row-adjacent ink can widen the set."""
    key = (f["n"], ang_10, filt)
    hit = _PAINT_CACHE.get(key)
    if hit is not None:
        return hit
    alp, w, h = alpha_plane(f)
    posx, posy = f["cx"] - f["pvx"], f["cy"] - f["pvy"]
    sma, cma = rot_sin_cos(ang_10)
    sxx, sxy = cma * 64, -(sma * 64)
    syx, syy = sma * 64, cma * 64
    xin0, yin0 = -f["pvx"], -f["pvy"]
    x0_ups = cdiv((cma * xin0 - sma * yin0) * 64, 256) + f["pvx"] * 256 + 0x80
    y0_ups = cdiv((sma * xin0 + cma * yin0) * 64, 256) + f["pvy"] * 256 + 0x80
    bx1, by1, bx2, by2 = swept_box(f, ang_10)
    pts = set()
    last = h - 1
    for y in range(by1, by2 + 1):
        y_abs = y - posy
        xs_r = x0_ups + ((sxy * y_abs) >> 8)
        ys_r = y0_ups + ((syy * y_abs) >> 8)
        for x in range(bx1, bx2 + 1):
            x_abs = x - posx
            xs_int = (xs_r + ((sxx * x_abs) >> 8)) >> 8
            ys_ups = ys_r + ((syx * x_abs) >> 8)
            ys_int = ys_ups >> 8
            if not (0 <= xs_int < w and 0 <= ys_int < h):
                continue
            if not filt:
                if alp[ys_int * w + xs_int]:
                    pts.add((x, y))
                continue
            yv = ys_ups - 0x80
            y0 = yv >> 8
            wy = yv & 0xFF
            if y0 < 0:
                y0, wy = 0, 0
            elif y0 >= last:
                y0, wy = last, 0
            s0 = y0 * w + xs_int
            if wy == 0:
                if alp[s0]:
                    pts.add((x, y))
            elif alp[s0] or alp[(y0 + (0 if y0 == last else 1)) * w + xs_int]:
                pts.add((x, y))
    _PAINT_CACHE[key] = pts
    return pts

def in_span(x, y, spans):
    a, b = spans[y]
    return a != 32767 and a <= x <= b

def tick_case(frames, g, f_i, a_prev, a_new, chord_cache, filt=False):
    spans = empty_spans()
    G, F = frames[g], frames[f_i]
    stamp(G, chord_cache[g], a_prev, spans)     # apply_angles orbit step
    stamp(G, chord_cache[g], a_new, spans)
    for fr in (G, F):                            # tourbillon_advance diff boxes
        dx1, dy1, dx2, dy2 = diff_box(fr, a_new)
        for y in range(dy1, dy2 + 1):
            spans[y] = (min(spans[y][0], dx1), max(spans[y][1], dx2))
    stamp(F, chord_cache[f_i], a_new, spans)     # incoming-frame chords

    miss_new = [(x, y) for (x, y) in painted_pixels(F, a_new, filt) if not in_span(x, y, spans)]
    miss_old = [(x, y) for (x, y) in painted_pixels(G, a_prev, filt) if not in_span(x, y, spans)]
    return miss_new, miss_old

def main():
    frames = parse_sprites()
    n = len(frames)
    print(f"parsed {n} tourbillon frames; w={frames[0]['w']} h={frames[0]['h']} "
          f"pivot=({frames[0]['pvx']},{frames[0]['pvy']}) "
          f"diff=({frames[0]['dx']},{frames[0]['dy']},{frames[0]['dw']},{frames[0]['dh']})")
    chord_cache = {i: chords(f) for i, f in enumerate(frames)}

    # Mark-area budget probe: typical orbit tick (frame swap + small step)
    areas = []
    rows_t = []
    for a_new in range(0, 3600, 60):
        sp = empty_spans()
        g = (a_new // 240) % n
        f_i = (g + 1) % n
        stamp(frames[f_i], chord_cache[f_i], a_new, sp)   # swap stamp only
        stamp(frames[g],  chord_cache[g],  a_new - 4, sp) # orbit prev
        stamp(frames[g],  chord_cache[g],  a_new, sp)     # orbit new
        for fr in (frames[g], frames[f_i]):
            dx1, dy1, dx2, dy2 = diff_box(fr, a_new)
            for y in range(dy1, dy2 + 1):
                sp[y] = (min(sp[y][0], dx1), max(sp[y][1], dx2))
        ar = sum(b - a + 1 for a, b in sp if b >= a)
        nr = sum(1 for a, b in sp if b >= a)
        areas.append(ar); rows_t.append(nr)
    print(f"swap-only stamp area avg {sum(areas)//len(areas)} px; "
          f"full tick union avg {sum(areas)//len(areas)} px, rows {sum(rows_t)//len(rows_t)}; "
          f"max tick {max(areas)} px")

    ref = painted_pixels(frames[0], 900)
    ys = [y for _, y in ref]
    print(f"painted set tourb00 @90.0deg: {len(ref)} px, rows {min(ys)}..{max(ys)}")
    ink = []
    ink_f = []
    for a_new in range(0, 3600, 150):
        g = (a_new // 240) % n
        ink.append(len(painted_pixels(frames[g], a_new)))
        ink_f.append(len(painted_pixels(frames[g], a_new, True)))
    print(f"painted-set avg {sum(ink)//len(ink)} px, max {max(ink)} px")
    print(f"painted-set with the vertical 2-tap: avg {sum(ink_f)//len(ink_f)} px "
          f"(+{(sum(ink_f)-sum(ink))//max(1,len(ink))}), max {max(ink_f)} px")

    worst = []
    ANGS = list(range(0, 3600, 37))
    ANGS += [44, 45, 46, 896, 904, 1346, 1354, 2246, 2254, 3146, 3154]
    for card in (0, 900, 1800, 2700):          # dense tenth-degrees at cardinals
        ANGS += [card + d for d in range(-6, 7)]
        ANGS += [card - 1 + d for d in range(-6, 7)]
    for filt in (False, True):        # nearest, then the carriage's 2-tap rows
        for a_new in ANGS:
            for step in (1, 2, 4, 7, 10):
                a_prev = a_new - step
                g = (a_new // 240) % n
                f_i = (g + (2 if step >= 7 else 1)) % n
                mn, mo = tick_case(frames, g, f_i, a_prev, a_new, chord_cache, filt)
                if mn or mo:
                    worst.append((filt, a_new, step, g, f_i, len(mn), len(mo),
                                  sorted(mn)[:6], sorted(mo)[:6]))
    if not worst:
        print("CLEAN: marked spans cover every pixel either pose paints, "
              "nearest and 2-tap, all cases")
        return 0
    print(f"{len(worst)} FAILING cases; worst first:")
    for filt, a_new, step, g, f_i, ln, lo, mn, mo in \
            sorted(worst, key=lambda t: -(t[5] + t[6]))[:20]:
        print(f" {'2-tap' if filt else 'nearest'} ang {a_new/10:6.1f} step {step:2d} "
              f"frames {g:2d}->{f_i:2d}: "
              f"missing-new={ln:5d} missing-old={lo:5d}  new@{mn[:4]} old@{mo[:4]}")
    return 1

if __name__ == "__main__":
    sys.exit(main())

