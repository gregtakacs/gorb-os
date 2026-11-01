#!/usr/bin/env python3
"""Runs the SHIPPED compiled-clock projection over a JS-baked bundle.

The browser's bakeClock3 claims a bundle the dial will project, not merely one the door
opens. host_clock3_test.py already builds orb_clock3.c (with orb_bundle.c behind it) into
a host harness, so this drives that same binary with --dump3f over any file and prints
what the projection settled on: every moving row (D42), the carriage, the complication
windows and the plate bank. Exit 0 means the dial's own code accepted the bake.

    python3 tools/verify_js_bake.py /tmp/bench_bake.orbtheme
"""
import os
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)
import host_clock3_test as H3
from theme_schema import SOURCES, SHAPES, ENDS, CADENCES


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
        p = H3.parsed(out)
        sc = {k: v[0] for k, v in p["sc"].items()}
        rows = p["rows"]
        print("%s: projected — %d rows, %dx%d panel, bank %d B, %d complications, beat %s %d/s"
              % (slug, len(rows), p["scr"][0], p["scr"][1], p["art"][1], len(p["comps"]),
                 "sweep" if sc.get("second_sweep") else "step", sc.get("tick_rate", 0)))
        for r in rows:
            s = r["spr"]
            twin = r["flags"] & 256
            print("    row[%d] ptr%d %s src=%s box %dx%d at (%d,%d) colour_off %d turn %d from %d "
                  "travel (%d,%d) over %d..%d end %s cad %s%s%s"
                  % (r["i"], r["tag"], SHAPES[r["shape"]] if r["shape"] < len(SHAPES) else "?",
                     SOURCES[r["src"]], s["w"], s["h"], s["offx"], s["offy"], s["color_off"],
                     r["span"], r["zero"], r["dx"], r["dy"], r["vmin"], r["vmax"],
                     ENDS[r["end"]] if r["end"] < len(ENDS) else "?",
                     CADENCES[r["cadence"]] if r["cadence"] < len(CADENCES) else "?",
                     "" if s["show"] else " hidden", " shadow-twin" if twin else ""))
            if twin and r["i"] > 0:
                # §25.2's identity for a twin: its centre is its row's centre plus the
                # light, and its arbour in its own art is the crop pivot. A light offset
                # that survived the wire as (twin centre - row centre) is the composer's
                # shadow promise; the print is what lets a test say so.
                h = rows[r["i"] - 1]["spr"]
                print("    row[%d] shadow centre (%d,%d) = row centre (%d,%d) + light (%d,%d), "
                      "crop pivot (%d,%d)"
                      % (r["i"], s["centerx"], s["centery"], h["centerx"], h["centery"],
                         s["centerx"] - h["centerx"], s["centery"] - h["centery"],
                         s["pivotx"], s["pivoty"]))
        if sc.get("tourbillon_n"):
            tb = p["spr"]["tourbillon"]
            for f in range(sc["tourbillon_n"]):
                t = tb[f]
                print("    carriage[%d] %dx%d draws at (%d,%d) colour_off %d"
                      % (f, t[0], t[1], t[2], t[3], t[10]))
            print("    carriage: row %d, %d frames @ %d fps"
                  % (sc["car_row"], sc["tourbillon_n"], sc["tourbillon_fps"]))
        for c in p["comps"]:
            (_, tag, kind, src, x, y, cw, ch, count, first, off, fbytes, vmin, scale, show,
             prow, dms) = c
            print("    comp[%d] window src=%s box %dx%d at (%d,%d) %d plates off %d stride %d "
                  "value_min %d depth %d %s"
                  % (tag, SOURCES[src], cw, ch, x, y, count, off, fbytes, vmin, first,
                     "shown" if show else "hidden"))
        if p["bgl"]:
            print("    plate bank: %d extra keyframes at %s"
                  % (len(p["bgl"]), ", ".join(str(p["bgl"][k]) for k in sorted(p["bgl"]))))
    if bad:
        print("%d of %d bundles the compiled dial would refuse" % (bad, len(sys.argv) - 1),
              file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
