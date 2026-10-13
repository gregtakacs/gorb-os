#!/usr/bin/env python3
"""Acceptance gate for an orbital kit's tourbillon carriage geometry.

Run it on the zip BEFORE baking it:

    python3 tools/kit_check.py <kit.zip>

Exit 0 = safe to bake. Exit 1 = the carriage staff does not sit on the
rotation centre the manifest declares, and the device will render a ~1.7 px
vertical bob on the hub jewel at 8 fps.

The gate measures the red staff jewel in every tourbillon frame and demands
that its alpha-weighted centroid land exactly on the manifest's declared
`centre`. It also demands an odd x odd ink bbox: an even-width blob on an
even-dimension canvas cannot have an integer centroid at all.

This gate needs a kit WITH a tourbillon: a kit without one fails at the
manifest check, which is correct but not a geometry verdict. Its historical
must-fail fixture, the off-axis kit2 (sha256 c13fe2f7d178aa31..., 3493631 B),
left the repo on 2026-10-12 with the rest of the orbital art bank. Re-create
the negative case by shifting a carriage's declared centre by (0.5, 0.5) px;
docs/tourbillon-axis-spec.md keeps the numbers it was rejected on.
"""
import json
import os
import sys
import tempfile
import zipfile

# red staff-jewel gate, matched to how the shipped art paints it
ALPHA_MIN = 60
RED_MIN = 100
RED_DOM = 40

CENTROID_TOL = 0.25      # hard reject beyond this
SPREAD_TOL = 0.01        # frames must be registered to within this


def load_png_bytes(data):
    """-> (W, H, rgb_fn, alpha_fn) from in-memory PNG bytes. Prefers Pillow,
    falls back to the sibling stdlib decoder so the gate runs bare."""
    tmp = os.path.join(tempfile.mkdtemp(prefix="kit_check_"), "f.png")
    with open(tmp, "wb") as f:
        f.write(data)
    try:
        return load_png(tmp)
    finally:
        os.remove(tmp)
        os.rmdir(os.path.dirname(tmp))


def load_png(path):
    """-> (W, H, rgb_fn, alpha_fn). Prefers Pillow, falls back to a stdlib PNG
    decoder so the gate runs in a bare environment too."""
    try:
        from PIL import Image
        im = Image.open(path).convert("RGBA")
        px = im.load()
        W, H = im.size

        def rgb(x, y):
            p = px[x, y]
            return p[0], p[1], p[2]

        def al(x, y):
            return px[x, y][3]
        return W, H, rgb, al
    except ImportError:
        # sibling stdlib decoder in tools/ -- no third-party deps needed
        libdir = os.path.dirname(os.path.abspath(__file__))
        if libdir not in sys.path:
            sys.path.insert(0, libdir)
        import pngdec
        im = pngdec.decode(path)
        W, H, ct, px = im["W"], im["H"], im["ct"], im["px"]
        a = pngdec.alpha8(im)

        def rgb(x, y):
            i = y * W + x
            if ct == 6:
                return px[i * 4], px[i * 4 + 1], px[i * 4 + 2]
            if ct == 2:
                return px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
            return 0, 0, 0
        return W, H, rgb, lambda x, y: a[y * W + x]



def red_components(w, h, rgb, al):
    """Every red-ink blob in the frame as (bbox, centroid, npix), 8-connected.
    The carriage's staff jewel shares the layer with the seconds hand's own red
    jewels, so the caller must pick the blob at the declared centre."""
    seen = set()
    pts = set()
    for y in range(h):
        for x in range(w):
            a = al(x, y)
            if a < ALPHA_MIN:
                continue
            r, g, b = rgb(x, y)
            if r > RED_MIN and r - max(g, b) > RED_DOM:
                pts.add((x, y))
    out = []
    for seed in sorted(pts):
        if seed in seen:
            continue
        stack = [seed]
        comp = []
        seen.add(seed)
        while stack:
            x, y = stack.pop()
            comp.append((x, y, al(x, y)))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in pts and q not in seen:
                        seen.add(q)
                        stack.append(q)
        xs = [c[0] for c in comp]
        ys = [c[1] for c in comp]
        sw = sum(c[2] for c in comp)
        cx = sum(c[0] * c[2] for c in comp) / sw
        cy = sum(c[1] * c[2] for c in comp) / sw
        out.append(((min(xs), max(xs), min(ys), max(ys)), (cx, cy), len(comp)))
    return out


def measure_hub(z, name, W, H, centre):
    """-> (bbox, alpha-weighted centroid, px count) of the staff jewel, i.e.
    the red blob whose centroid is nearest the declared rotation centre."""
    w, h, rgb, al = load_png_bytes(z.read(name))
    if (w, h) != (W, H):
        sys.exit("FAIL %s is %dx%d, expected %dx%d (layers must be full-canvas)"
                 % (name, w, h, W, H))
    comps = red_components(w, h, rgb, al)
    if not comps:
        sys.exit("FAIL %s has no red ink (alpha>=%d, R>%d, R-max(G,B)>%d): the "
                 "axis cannot be verified" % (name, ALPHA_MIN, RED_MIN, RED_DOM))
    best = min(comps, key=lambda c: (c[1][0] - centre[0]) ** 2
                                + (c[1][1] - centre[1]) ** 2)
    return best, comps


def declared_tourbillon(m):
    """-> (centre, entry) for the manifest's tourbillon layer."""
    for d in m.get("drawOrder", []):
        if d.get("layer") == "tourbillon":
            return d.get("centre"), d
    return None, None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    kit = sys.argv[1]
    z = zipfile.ZipFile(kit)
    m = json.loads(z.read("manifest.json"))
    centre, tb = declared_tourbillon(m)
    if not centre:
        sys.exit("FAIL manifest has no tourbillon entry with a `centre`")
    cx, cy = centre
    names = ["tourbillon/frame_%02d.png" % i
             for i in range(1, int(m.get("frames", 0)) + 1)]
    missing = [f for f in names if f not in z.namelist()]
    if missing:
        sys.exit("FAIL manifest declares %s frames but these are absent: %s"
                 % (m.get("frames"), missing))
    W, H = load_png_bytes(z.read(names[0]))[0:2]

    print("kit            : %s" % os.path.basename(kit))
    print("canvas         : %dx%d  (manifest size %s)" % (W, H, m.get("size")))
    print("declared centre: [%s, %s]   frames=%s fps=%s"
          % (cx, cy, m.get("frames"), m.get("fps")))
    print()

    fails = []
    cents = []
    bboxes = []
    for i, f in enumerate(names, 1):
        (bbox, (mx, my), n), comps = measure_hub(z, f, W, H, centre)
        if i == 1:
            print("  (red blobs in layer: %d, staff = nearest to centre)"
                  % len(comps))
        bw = bbox[1] - bbox[0] + 1
        bh = bbox[3] - bbox[2] + 1
        cents.append((mx, my))
        bboxes.append(bbox)
        print("  frame_%02d  hub bbox x%d..%d y%d..%d (%dx%d)  "
              "centroid (%.4f, %.4f)  %d px"
              % (i, bbox[0], bbox[1], bbox[2], bbox[3], bw, bh, mx, my, n))
        if i == 1:
            if bw % 2 == 0 or bh % 2 == 0:
                fails.append("staff jewel bbox is %dx%d: must be ODD x ODD. An "
                             "even blob on an even canvas puts its centroid on "
                             "a texel corner, so no integer centre can be "
                             "correct" % (bw, bh))
            if abs(mx - cx) > CENTROID_TOL or abs(my - cy) > CENTROID_TOL:
                fails.append("centroid (%.4f, %.4f) is (%+.3f, %+.3f) from the "
                             "declared centre [%s, %s] = %.3f px: must be <= "
                             "%.2f px"
                             % (mx, my, mx - cx, my - cy, cx, cy,
                                ((mx - cx) ** 2 + (my - cy) ** 2) ** 0.5,
                                CENTROID_TOL))

    sdx = max(c[0] for c in cents) - min(c[0] for c in cents)
    sdy = max(c[1] for c in cents) - min(c[1] for c in cents)
    print()
    print("  frame-to-frame registration spread: (%.4f, %.4f)" % (sdx, sdy))
    if len(set(bboxes)) != 1 or sdx > SPREAD_TOL or sdy > SPREAD_TOL:
        fails.append("bank not registered: %d distinct bboxes, centroid spread "
                     "(%.4f, %.4f) > %.2f px. The baker takes ONE crop for the "
                     "whole cycle, so drift desyncs the dirty rects"
                     % (len(set(bboxes)), sdx, sdy, SPREAD_TOL))

    for fld in ("pivot", "size", "hubJewel"):
        if fld not in tb:
            fails.append("tourbillon entry has no `%s` field (hands carry "
                         "theirs; the carriage needs the same proof)" % fld)
    if type(cx) is float or type(cy) is float:
        fails.append("centre is fractional (%s, %s): prep_watch_kit.py and "
                     "bake_clock.py:547 read it with int(), which truncates "
                     "and silently loses the correction. Move the ART" % (cx, cy))
    if int(m.get("frames", 0)) != len(names):
        fails.append("manifest frames=%r != %d tourbillon PNGs"
                     % (m.get("frames"), len(names)))

    print()
    if fails:
        d = ((cents[0][0] - cx) ** 2 + (cents[0][1] - cy) ** 2) ** 0.5
        print("RESULT: FAIL")
        for f in fails:
            print("  - %s" % f)
        print()
        print("On device the integer pivot sits %.3f px from the staff, so the "
              "hub orbits %.2f px peak-to-peak every tick and the dirty rect "
              "jitters with it." % (d, 2 * d + 0.3))
        return 1
    print("RESULT: PASS  (centroid on the declared centre, odd bbox, bank "
          "registered, geometry self-declared)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
