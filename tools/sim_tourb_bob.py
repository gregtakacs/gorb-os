#!/usr/bin/env python3
"""Host-side verifier for the tourbillon hub-jewel bob (no device needed).

The carriage's staff jewel is its physical rotation axis. The firmware pins an
INTEGER pivot on the layer centre and inverse-maps every screen texel through
it, so if that stored pivot sits a distance d from the jewel's real alpha
centroid, the jewel must orbit a circle of radius d -- 2d px peak-to-peak, every
tick, and the frame's dirty rect jitters along with it. An integer pivot can
never be correct when the axis falls on a texel corner (a .5/.5 centroid), which
is exactly the defect that made kit2's hub bob 1.70 px.

This tool reads the BAKED pair (assets/clock_assets.{bin,h}), i.e. the bytes the
device actually gets, and asks whether the shipped geometry can bob at all:

  * the stored pivot-to-axis distance, which should be 0.0000 px, and
  * the peak-to-peak row travel of the alpha centroid of the ink the painter
    really samples, over a full revolution.

Run it after tools/bake_clock.py, alongside tools/sim_tourb_mark.py:

    python3 tools/sim_tourb_bob.py

Exit 0 = the hub holds one screen row all cycle. Exit 1 = the carriage art or
its baked pivot is off-axis: fix the ART (see docs/tourbillon-axis-spec.md),
not the firmware. No runtime filter can cancel an orbit geometry demands.

The maths and the red-jewel isolation are IMPORTED, not copied: rot_sin_cos /
cdiv / parse_sprites come from sim_tourb_mark.py (which mirrors paint_layer's
nearest map) and the jewel selector and red thresholds from kit_check.py (the
art-side gate), so a pass here and a pass there cannot quietly disagree.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit_check             # noqa: E402  red_components + the red thresholds
import sim_tourb_mark as mk  # noqa: E402  the firmware integer maths

HUB_WINDOW = 16        # texels about the declared centre to sample the hub in
PAINT_ALPHA_MIN = 13   # clock_view.c paint_row_ctx's own alpha cutoff
PIVOT_AXIS_TOL = 0.25  # px; the same tolerance kit_check.py holds the art to
ROW_TRAVEL_TOL = 0.35  # px peak-to-peak; sub-texel is the requirement
STEP_10 = 2            # 0.02 deg: fine enough to catch any single-texel jump


def color_plane(f):
    """-> (rgb565 bytes, w, h), following emit_sprite's packing."""
    return mk.BLOB[f["coff"]:f["coff"] + f["w"] * f["h"] * 2], f["w"], f["h"]


def alpha_plane(f):
    """-> the frame's 8-bit alpha plane, which follows the colour plane."""
    w, h = f["w"], f["h"]
    return mk.BLOB[f["coff"] + w * h * 2:f["coff"] + w * h * 3]


def is_red(rgb, a, x, y):
    """kit_check's staff-jewel gate applied to one baked texel."""
    if a < kit_check.ALPHA_MIN:
        return False
    r, g, b = rgb(x, y)
    return r > kit_check.RED_MIN and r - max(g, b) > kit_check.RED_DOM


def hub_ink(f):
    """-> ({idx: alpha}, bbox, centroid) for this frame's staff jewel, read from
    the baked RGB565 + alpha planes through kit_check's own red gate."""
    col, w, h = color_plane(f)
    alpha = alpha_plane(f)

    def rgb(x, y):
        v = col[(y * w + x) * 2] | (col[(y * w + x) * 2 + 1] << 8)
        return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63,
                (v & 31) * 255 // 31)

    comps = kit_check.red_components(w, h, rgb, lambda x, y: alpha[y * w + x])
    if not comps:
        raise SystemExit("frame %d has no red ink: the axis cannot be verified"
                         % f["n"])
    # the staff is the red blob nearest the stored pivot
    bbox, centroid, npix = min(comps, key=lambda c: (c[1][0] - f["pvx"]) ** 2
                                            + (c[1][1] - f["pvy"]) ** 2)
    x1, x2, y1, y2 = bbox
    ink = {y * w + x: float(alpha[y * w + x])
           for y in range(y1, y2 + 1) for x in range(x1, x2 + 1)
           if is_red(rgb, alpha[y * w + x], x, y)}
    if len(ink) != npix:
        raise SystemExit("frame %d: %d red px inside the staff bbox but the gate "
                         "found %d -- another blob shares the bbox"
                         % (f["n"], len(ink), npix))
    return ink, bbox, centroid


def hub_row(ink, f, ang_10, px_off=0, py_off=0):
    """Screen row of the hub's alpha centroid after painting frame f at ang_10.

    Replicates paint_ctx_init + paint_row_ctx's nearest map on the integer path
    the firmware ships. px_off/py_off perturb the STORED pivot only, which is
    how the controls below prove the travel is geometric and not sampling noise.
    """
    w, h = f["w"], f["h"]
    cx, cy = f["cx"], f["cy"]
    pvx, pvy = f["pvx"] + px_off, f["pvy"] + py_off
    sma, cma = mk.rot_sin_cos(ang_10)
    sxx, sxy = cma * 64, -(sma * 64)
    syx, syy = sma * 64, cma * 64
    x0 = mk.cdiv((cma * -pvx - sma * -pvy) * 64, 256) + pvx * 256 + 0x80
    y0 = mk.cdiv((sma * -pvx + cma * -pvy) * 64, 256) + pvy * 256 + 0x80
    posx, posy = cx - pvx, cy - pvy
    ax = ay = aw = 0.0
    for y in range(cy - HUB_WINDOW, cy + HUB_WINDOW + 1):
        xs_r = x0 + ((sxy * (y - posy)) >> 8)
        ys_r = y0 + ((syy * (y - posy)) >> 8)
        for x in range(cx - HUB_WINDOW, cx + HUB_WINDOW + 1):
            xa = x - posx
            xs_int = (xs_r + ((sxx * xa) >> 8)) >> 8
            yi = (ys_r + ((syx * xa) >> 8)) >> 8
            if xs_int < 0 or xs_int >= w or yi < 0 or yi >= h:
                continue
            a = ink.get(yi * w + xs_int, 0.0)
            if a > PAINT_ALPHA_MIN:
                ax += x * a
                ay += y * a
                aw += a
    return (ay / aw) if aw > 0 else None


def travel(ink, f, px_off=0, py_off=0):
    """-> (peak-to-peak px, sorted set of integer rows) over a full revolution."""
    rows, seen = [], set()
    for a in range(0, 3600, STEP_10):
        r = hub_row(ink, f, a, px_off, py_off)
        if r is not None:
            rows.append(r)
            seen.add(int(round(r)))
    return (max(rows) - min(rows) if rows else 0.0), sorted(seen)


def main():
    frames = mk.parse_sprites()
    if not frames:
        raise SystemExit("no tourbillon frames in assets/clock_assets.h -- bake "
                         "one with tools/bake_clock.py --tourbillon first")
    print("baked tourbillon: %d frames" % len(frames))
    fails = []
    worst = 0.0
    for f in sorted(frames, key=lambda r: r["n"]):
        ink, bbox, centroid = hub_ink(f)
        d = ((centroid[0] - f["pvx"]) ** 2 + (centroid[1] - f["pvy"]) ** 2) ** 0.5
        p2p, seen = travel(ink, f)
        worst = max(worst, p2p)
        print("  tourb%02d  staff bbox x%d..%d y%d..%d (%dx%d)  centroid "
              "(%.4f, %.4f)" % (f["n"], bbox[0], bbox[1], bbox[2], bbox[3],
                                bbox[1] - bbox[0] + 1, bbox[3] - bbox[2] + 1,
                                centroid[0], centroid[1]))
        print("          pivot (%d,%d)  off-axis %.4f px   rows %s  p2p %.2f px"
              % (f["pvx"], f["pvy"], d, seen, p2p))
        if d > PIVOT_AXIS_TOL:
            fails.append("tourb%02d: stored pivot sits %.4f px from the staff "
                         "axis (> %.2f), so the hub must orbit %.2f px p2p"
                         % (f["n"], d, PIVOT_AXIS_TOL, 2 * d))
        if p2p > ROW_TRAVEL_TOL:
            fails.append("tourb%02d: hub travels %.2f px p2p across %d screen "
                         "rows" % (f["n"], p2p, len(seen)))

    f = frames[0]
    ink, _, _ = hub_ink(f)
    print("\ncontrols, same art with the stored pivot perturbed:")
    for dx, dy, tag in ((0, 0, "as baked"),
                        (1, 1, "pivot +1,+1 texel (kit2's error direction)"),
                        (1, 0, "pivot +1 in x")):
        p2p, _ = travel(ink, f, dx, dy)
        print("  %-42s p2p %5.2f px" % (tag, p2p))
    print("\nworst-case hub travel over the bank: %.2f px p2p" % worst)
    if fails:
        print("\nFAIL:")
        for m in fails:
            print("  - %s" % m)
        print("\nThe bob is geometric: the art's staff axis is not on the baked "
              "pivot. Re-render\nthe carriage per docs/tourbillon-axis-spec.md and "
              "re-run tools/kit_check.py\nbefore baking. Do not patch this in the "
              "firmware.")
        return 1
    print("\nCLEAN: every baked pivot sits on the staff axis, and the hub holds a "
          "single\nscreen row through the whole revolution.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
