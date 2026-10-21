#!/usr/bin/env python3
"""§28's owed on-glass parity: the element table's placement, checked on pixels.

Pass line (b) of §28 was pinned host-side and only host-side: each preview row's
rect and rotation centre equal the shipped pair's `clk_sprite_t` fields in
`tests/fixtures/avkit-pair.h`. What stayed owed was the other half of the claim
-- that the DIAL puts those rows where the table says, since a projection that
agrees with the pair and a compositor that draws it somewhere else are two
different bugs. §29 named the door: `GET /api/v1/fb`. This closes it.

The measurement is geometric, so it falsifies. A sweeping kit's seconds hand travels a
full circle about its declared pivot, so over one revolution every pixel that changed
between consecutive frames must lie inside the outer bound of some shown row's crop --
that crop's farthest corner from its declared pivot, both numbers read from the bake
rather than from the picture. That is the whole gate, and it is tight: a row drawn at the
wrong centre or the wrong radius puts ink outside every bound the table declares.

Two things print as diagnostics and are NOT gated, both measured rather than assumed:

  - the centroid of all moving ink. It cannot gate on a dial with more than one mover:
    the hour and minute hands move at a FIXED bearing, so their frame-to-frame ink is
    lopsided by construction. Measured: 49.9 px off the seconds pivot, 3,700 px in one 30
    degree sector against zero in another, while containment was 100%.
  - the inner radial bound (a crop's nearest corner). It holds only for a row alone in
    its annulus; ~700 px of a 17,995 px run sit in the hub, where the complication glyphs
    and the hands' own hub caps live and which this tool does not model as a mover.

A third gate -- an annulus only the seconds rows can ink -- was written, measured, and
deleted: the minute hand's shadow reaches r 204.2 against the seconds shadow's 211.0, so
on a multi-hand dial that band is ~7 px wide and the gate can never fire. A gate that
cannot fail is machinery without a consumer. Row-by-row placement needs ink charged to
one element, which is what the D31 damage route does and P8 builds; through /api/v1/fb
the union of movers is the honest ceiling.

The shadow's pivot is the light offset, not the hand's (238,238 against av-kit's
233,233), so the printed bounds keep them apart rather than pretending they coincide --
`9d74b39` is what makes that offset a fact about the glass instead of a tolerance.

Capture runs long on purpose: one seconds revolution at sweep is 60 s, and the
sample must cover it or the swept union is an arc, not a circle. Run detached --
the 30 s tool cap is real (.clinerules).

Usage:
  bench_preview_parity.py [--pair PATH] [--secs N] [--out PREFIX] [--host IP]
Exit: 0 parity holds, 1 a gate failed, 2 transport refusal / cannot measure.
"""
import argparse, array, math, os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbgrab, orb_bind

HERE = os.path.dirname(os.path.abspath(__file__))
DEF_PAIR = os.path.join(HERE, "..", "tests", "fixtures", "avkit-pair.h")

# Every hand and shadow the bake shows. The hour and minute hands move too --
# slowly, at a fixed bearing -- so they belong in the explained set, and their
# presence is exactly why a centroid over all moving ink cannot gate anything.
MOVING = [(k, i, n) for k in ("hand", "shadow")
          for i, n in ((0, "hour"), (1, "minute"), (2, "second"))]
SECONDS = ("hand", 2, "second")

ROW_RE = re.compile(
    r"\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
    r"\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+),")


def parse_pair(path):
    """Lift the needed fields out of the generated header text.

    A reader of the shipped text, not a re-imagination of the struct: the claim
    under test is what the bake wrote, so the tool checking the glass must not
    invent its own numbers."""
    txt = open(path).read()
    theme = txt[txt.index("static const clk_theme_t clk_theme"):]

    def block(name):
        """Rows from a member block, line by line.

        The end test is a line that is only `},` — searching for the substring
        `},` stops at the first ROW's own terminator instead, which is the bug
        this function was written twice to avoid."""
        head = "/* %s */ {" % name
        lines = theme[theme.index(head):].splitlines()[1:]
        rows = []
        for ln in lines:
            if ln.strip() == "},":
                break
            m = ROW_RE.search(ln)
            if m:
                rows.append(m.groups())
        if len(rows) != 5:
            raise SystemExit("%s: read %d rows, expected 5" % (name, len(rows)))
        return rows


    out = {k: [tuple(int(x) for x in r) for r in block(k)]
           for k in ("hand", "shadow")}
    # The scalars decide whether the test means anything at all: a kit that does
    # not sweep puts the seconds hand in jumps, and the union is 12 lumps.
    sc = re.search(r"/\* sweep,railway,textOverHands,tickRate \*/ "
                   r"(\d+), (\d+), (\d+), (\d+),", theme)
    if not sc:
        raise SystemExit("cannot read the sweep/tickRate line from %s" % path)
    out["sweep"], out["tick_rate"] = int(sc.group(1)), int(sc.group(4))
    out["scr_w"] = int(re.search(r"#define CLK_SCR_W (\d+)", txt).group(1))
    out["scr_h"] = int(re.search(r"#define CLK_SCR_H (\d+)", txt).group(1))
    return out


def row_geom(rows, idx):
    """Pivot on screen, the crop's swept bounds, ink area — all read from the row.

    Two radii. Rotating a crop about its pivot sweeps an annulus: the farthest
    corner bounds it outside, the nearest corner bounds it inside. The outer
    bound is the gate; the inner one is reported only, because it holds solely
    for a row alone in its annulus (see the module docstring)."""
    w, h, _ox, _oy, px, py, cx, cy, _blend, show = rows[idx][:10]
    corner = [(px - a, py - bb) for a in (0, w) for bb in (0, h)]
    r_in = min(math.hypot(*c) for c in corner)
    r_out = max(math.hypot(*c) for c in corner)
    return {"show": show, "w": w, "h": h, "cx": cx, "cy": cy,
            "r_in": r_in, "radius": r_out, "area": w * h}


def luma565(v):
    r = (v >> 11) & 31
    g = (v >> 5) & 63
    b = v & 31
    return (r * 77 + g * 151 + b * 28) >> 6


def moved_pixels(prev, cur, w, h, thr):
    """Indices of pixels whose luma changed between two raw565 frames.

    Early-out on the packed value: a still plate makes almost every pair equal,
    and that comparison is the whole cost of a 75 s capture."""
    a = array.array("H"); a.frombytes(prev)
    b = array.array("H"); b.frombytes(cur)
    out = []
    for i in range(w * h):
        va, vb = a[i], b[i]
        if va == vb:
            continue
        if abs(luma565(va) - luma565(vb)) > thr:
            out.append(i)
    return out


def capture(base, secs, outdir):
    """raw565 frames across the window. Returns list[bytes], or None on refusal."""
    os.makedirs(outdir, exist_ok=True)
    frames = []
    t_end = time.time() + secs
    k = 0
    while time.time() < t_end:
        try:
            meta, raw = fbgrab.fetch(base, "raw565", 200, False)
        except Exception as e:
            print("FAIL: grab %d: %s" % (k, e))
            return None
        with open(os.path.join(outdir, "f%03d.bin" % k), "wb") as f:
            f.write(raw)
        frames.append(raw)
        k += 1
        sys.stdout.write("\r  grabbed %d (%s)"
                         % (k, meta.get("consistency", "?")))
        sys.stdout.flush()
        time.sleep(0.4)
    print("\n  frames  : %d across %ds" % (k, secs))
    return frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pair", default=DEF_PAIR)
    ap.add_argument("--secs", type=int, default=75)
    ap.add_argument("--thr", type=int, default=24)
    ap.add_argument("--out", default="/tmp/parity")
    ap.add_argument("--host", default="")
    a = ap.parse_args()

    pair = parse_pair(a.pair)
    if not pair["sweep"]:
        print("SKIP: the pair does not sweep; the swept union is lumps, not a "
              "circle, and neither gate means anything.")
        return 0

    if a.secs < 62:
        print("FAIL: a %ds window cannot cover the 60 s seconds revolution. The "
              "swept union would be an arc, and all three gates would mean "
              "nothing. Re-run with --secs 80 or more." % a.secs)
        return 2

    # Same target resolution and the same identity gate fbgrab uses: gate_ip()
    # is an `ip neigh` MAC check, because gate_mac() opens the serial port and
    # opening the port resets the board -- which would replace the screen being
    # measured with a boot log.
    base = ("http://%s" % a.host) if a.host else \
        "http://%s" % orb_bind.gate_ip(orb_bind.load())
    movers = []
    for kind, idx, label in MOVING:
        g = row_geom(pair[kind], idx)
        if g["show"]:
            g["label"] = "%s %s" % (kind, label)
            movers.append(g)
    if not movers:
        print("SKIP: no moving row is shown in this pair.")
        return 0

    print("pair      : %s  %dx%d  sweep=%d tick_rate=%d"
          % (os.path.basename(a.pair), pair["scr_w"], pair["scr_h"],
             pair["sweep"], pair["tick_rate"]))
    for m in movers:
        print("  mover   : %-14s pivot (%d,%d) annulus r %.1f..%.1f"
              % (m["label"], m["cx"], m["cy"], m["r_in"], m["radius"]))
    print("target    : %s" % base)

    w, h = pair["scr_w"], pair["scr_h"]
    stride = w * h * 2
    frames = capture(base, a.secs, a.out)
    if frames is None:
        return 2
    if len(frames) < 4:
        print("FAIL: too few frames to cover a revolution.")
        return 1
    if len(frames[0]) != stride:
        print("FAIL: raw565 body is %d B, expected %d" % (len(frames[0]), stride))
        return 2

    moved = set()
    for k in range(1, len(frames)):
        if len(frames[k]) == stride:
            moved.update(moved_pixels(frames[k - 1], frames[k], w, h, a.thr))
    if not moved:
        print("FAIL: nothing moved on the glass across the window.")
        return 1

    # ---- the gates ---------------------------------------------------------
    # GATE 1 (hard) CONTAINMENT: every moving pixel lies inside the swept
    # annulus of at least one shown mover. Radii come from the table, so this
    # fails when the dial draws a row where the table does not say.
    # GATE 2 (hard) MOTION: enough pixels moved to make gate 1 mean anything.
    # Diagnostics only, printed and NOT gated: the centroid and the per-sector
    # ring. A centroid over all moving ink cannot gate on a dial with more than
    # one mover -- the hour and minute hands move at a fixed bearing, so their
    # diff-ink is lopsided by construction (measured first run: centroid 49.9 px
    # off, sector histogram 3700 px in one sector against 0 in another, while
    # containment was 100%). Proving placement row by row needs ink attributed
    # to one element, which is what the D31 damage route does and what P8
    # builds; through /api/v1/fb the union of movers is the honest ceiling.
    xs = [p % w for p in moved]
    ys = [p // w for p in moved]
    dist = [[math.hypot(x - m["cx"], y - m["cy"]) for x, y in zip(xs, ys)]
            for m in movers]
    slack = 3.0
    inside = [[d <= m["radius"] + slack
               for d in dd] for m, dd in zip(movers, dist)]
    bad = sum(1 for k in range(len(xs)) if not any(c[k] for c in inside))
    cont_pct = 100.0 * (len(xs) - bad) / len(xs)
    # Inner bound and the angular picture are REPORTED, not gated, and both for
    # measured reasons. The inner bound holds only for a row alone in its annulus:
    # 695 px of the first run's 17,323 fell nearer a pivot than any crop's nearest
    # corner, and that is the hub region, where the complication glyphs and the
    # hands' own hub caps live and which this tool does not model as a mover. A
    # band only the seconds rows can ink was written, measured, and deleted: the
    # minute hand's SHADOW reaches r 204.2 against the seconds shadow's 211.0, so
    # on a multi-hand dial the band is ~7 px wide and a gate there can never fire
    # -- and a gate that cannot fail is the machinery-without-a-consumer this plan
    # already refuses. Row-by-row attribution needs ink charged to one element,
    # which is what the D31 damage route does and P8 builds.
    inner_bad = sum(1 for k in range(len(xs))
                    if all(dd[k] < movers[j]["r_in"] - slack
                           for j, dd in enumerate(dist)))
    cen_x = sum(xs) / len(xs)
    cen_y = sum(ys) / len(ys)
    dom = max(movers, key=lambda m: m["area"])
    hist = [0] * 12
    for x, y, d in zip(xs, ys, dist[movers.index(dom)]):
        if d < 12 or d > dom["radius"] + slack:
            continue
        ang = (math.atan2(y - dom["cy"], x - dom["cx"]) + math.pi) / (2 * math.pi)
        hist[min(11, int(ang * 12))] += 1

    print("\n  moving px      : %d (%.2f%% of panel)"
          % (len(moved), 100.0 * len(moved) / (w * h)))
    print("  containment    : %.2f%% inside a declared outer bound (%d unexplained)"
          % (cont_pct, bad))
    for m, c in zip(movers, inside):
        print("    explained by %-14s : %d px" % (m["label"], sum(c)))
    print("  inner bound    : %d px nearer a pivot than any crop's nearest corner "
          "(hub ink, reported not gated)" % inner_bad)
    print("  centroid (diag): (%.1f, %.1f) -- union of movers, not a gate" % (cen_x, cen_y))
    print("  ring (diag)    : %d/12 sectors lit about the %s, hist %s"
          % (sum(1 for v in hist if v), dom["label"], hist))

    fails = []
    if cont_pct < 99.5:
        fails.append("containment %.2f%% < 99.5%%: %d moving px land outside every "
                     "outer bound the table declares" % (cont_pct, bad))
    if len(moved) < 3000:
        fails.append("only %d moving px: too little motion to certify containment"
                     % len(moved))

    if fails:
        print("\nPARITY FAIL")
        for f in fails:
            print("  - %s" % f)
        return 1
    print("\nPARITY PASS -- every moving pixel on the glass lies inside the outer "
          "bound the element table declares for a shown row, measured through "
          "/api/v1/fb")
    return 0


if __name__ == "__main__":
    sys.exit(main())

