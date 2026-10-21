#!/usr/bin/env python3
"""Runs the SHIPPED compiled-clock projection over a JS-baked bundle.

The browser's bakeClock3 claims a bundle the dial will project, not merely one the door
opens. host_clock3_test.py already builds orb_clock3.c (with orb_bundle.c behind it) into
a host harness, so this drives that same binary with --dump3f over any file and prints
what the projection settled on. Exit 0 means the dial's own code accepted the bake.

    python3 tools/verify_js_bake.py /tmp/bench_bake.orbtheme
"""
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import host_clock3_test as H3
from theme_schema import SOURCES
from theme_schema import MOTION_NAMES as MOTIONS


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: verify_js_bake.py BUNDLE [EXTRA BUNDLE ...]")
    exe = H3.build()
    bad = 0
    for path in sys.argv[1:]:
        rc, out, err = H3.run(exe, "--dump3f", path)
        slug = os.path.basename(path)
        if rc != 0:
            bad += 1
            print("PROJECTION REFUSED %s: %s" % (slug, (out or err).strip()[:400]), file=sys.stderr)
            continue
        sc = {}
        spr = []
        bgrows = []
        ncomp = 0
        comps = []
        n_aux = 0
        auxs = []
        art = scr = cnt = None
        for line in out.splitlines():
            w = line.split()
            if w[0] == "scalar":
                sc[w[1]] = w[2]
            elif w[0] == "bgframe":
                bgrows.append(int(w[2]))
            elif w[0] == "art":
                art = int(w[1]), int(w[2])
            elif w[0] == "scr":
                scr = (int(w[1]), int(w[2]))
            elif w[0] == "counts":
                cnt = (int(w[1]), int(w[2]))
            elif w[0] == "n_comps":
                ncomp = int(w[1])
            elif w[0] == "n_aux":
                n_aux = int(w[1])
            elif w[0] == "aux":
                # the harness's own field order: idx tag kind src x y w h span zero
                # vmin vmax color_off show flags pivotx pivoty centerx centery
                auxs.append(tuple(int(x) for x in w[1:]))
            elif w[0] == "comp":
                # the harness's own field order: idx tag kind src x y w h count
                # first off frame_bytes value_min scale show
                comps.append((int(w[1]), int(w[2]), int(w[3]), int(w[4]), int(w[5]), int(w[6]),
                              int(w[7]), int(w[8]), int(w[9]), int(w[10]), int(w[11]),
                              int(w[12]), int(w[13]), int(w[14]), int(w[15])))
            elif w[0] in ("plate", "overlay", "hand", "shadow", "tourbillon"):
                # dump_sprite's own field order: tag i w h offx offy pivotx pivoty
                # centerx centery blend show color_off diffx diffy diffw diffh
                spr.append((w[0], int(w[1]), int(w[2]), int(w[3]), int(w[4]), int(w[5]), int(w[12]),
                            int(w[6]), int(w[7]), int(w[8]), int(w[9])))
        shown = [s for s in spr if s[6]]
        print("%s: projected — %d seated sprites, %dx%s panel, bank %s B, %d complications, "
              "%d pointers"
              % (slug, len(shown), scr[0], scr[1], art[1], ncomp, n_aux))
        for kind, idx, w, h, offx, offy, coff, px, py, cx, cy in shown[:8]:
            print("    %-10s[%d] %dx%d draws at (%d,%d) colour_off %d" % (kind, idx, w, h, offx, offy, coff))
            if kind == "shadow":
                # §25.2's whole contract in one line: the row's PIVOT COLUMN became
                # the centre, and the CROPPED pivot — recomputed from the hand, never
                # carried by the row — pins the plane's pivot pixel there. A light
                # offset that survived the wire as (centre - hand centre) is the
                # composer's shadow promise; the print is what makes the test able
                # to say so.
                hand = next((s for s in shown if s[0] == "hand" and s[1] == idx), None)
                hx = hand[4] + hand[7] if hand else 0
                hy = hand[5] + hand[8] if hand else 0
                print("    shadow[%d] centre (%d,%d) = hand centre (%d,%d) + light (%d,%d), crop pivot (%d,%d)"
                      % (idx, cx, cy, hx, hy, cx - hx, cy - hy, px, py))
        for (_, tag, kind, src, x, y, cw, ch, count, first, off, fbytes, vmin, scale, show) in comps:
            print("    comp[%d] window src=%s box %dx%d at (%d,%d) %d plates off %d stride %d value_min %d %s"
                  % (tag, SOURCES[src], cw, ch, x, y, count, off, fbytes, vmin,
                     "shown" if show else "hidden"))
        for a in auxs:
            (_, tag, kind, src, x, y, aw, ah, span, zero, vmin, vmax, coff, show,
             flags, px, py, cx, cy) = a
            # D34: the pointer, read back off the clk_aux_t the shipped projection filled.
            # The unit is the motion's own, so it is printed with the number rather than
            # beside it: 3600 on a rotate is a turn, 90 on a slide-x is ninety pixels.
            print("    aux[%d] %s src=%s box %dx%d at (%d,%d) travel %d from %d over %d..%d %s%s"
                  % (tag, MOTIONS[kind] if kind < len(MOTIONS) else "?%d" % kind,
                     SOURCES[src], aw, ah, x, y, span, zero, vmin, vmax,
                     "shown" if show else "hidden",
                     " shadow-twin" if flags & 256 else ""))
            if flags & 256:
                # §25.2's identity for the pointer's twin: the projection took the
                # row's pivot column as the centre (x + pivot), and the hand it hangs
                # under is the row the dial placed directly before it. A light offset
                # that survived the wire as (twin centre - hand centre) is the
                # composer's shadow promise; the print is what lets the test say so.
                hand = next((p for p in auxs if p[0] == tag - 1), None)
                if hand:
                    print("    aux[%d] shadow centre (%d,%d) = hand centre (%d,%d) "
                          "+ light (%d,%d), crop pivot (%d,%d)"
                          % (tag, cx, cy, hand[17], hand[18], cx - hand[17],
                             cy - hand[18], px, py))
        print("    scalars: tick_rate %s order_n %s shadow_on %s bg_frames %s"
              % (sc.get("tick_rate"), sc.get("order_n"), sc.get("shadow_on"),
                 sc.get("bg_frames")))
        if bgrows:
            print("    plate bank: %d extra keyframes at %s"
                  % (len(bgrows), ", ".join(str(o) for o in bgrows)))
    if bad:
        print("%d of %d bundles the compiled dial would refuse" % (bad, len(sys.argv) - 1), file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
