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
            elif w[0] in ("plate", "overlay", "hand", "shadow", "tourbillon"):
                # dump_sprite's own field order: tag i w h offx offy pivotx pivoty
                # centerx centery blend show color_off diffx diffy diffw diffh
                spr.append((w[0], int(w[1]), int(w[2]), int(w[3]), int(w[4]), int(w[5]), int(w[12])))
        shown = [s for s in spr if s[6]]
        print("%s: projected — %d seated sprites, %dx%s panel, bank %s B"
              % (slug, len(shown), scr[0], scr[1], art[1]))
        for kind, idx, w, h, offx, offy, coff in shown[:8]:
            print("    %-10s[%d] %dx%d draws at (%d,%d) colour_off %d" % (kind, idx, w, h, offx, offy, coff))
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
