#!/usr/bin/env python3
"""bench_rung_table.py — P8.1's rung fixture: the av-kit dial with its still plate
replaced by a full-panel keyframe bank of N planes, at one requested bg_fps.

This is bench_d31_bundle.py's four-frame bank made parametric. The bench question
§31 left open and §8's P8 row demands is the DELIVERED rate across bank sizes: the
plate is full-panel, so every advance invalidates all 466 rows, and the flush floor
is the panel's worst case, not the requested bg_fps. The fixture must be one honest
animation at N planes so the boot line names the bank and the glass moves; the
medallion turns 360/N degrees per frame over the plate's own warm field.

The ask is computed against D29's art-region cap BEFORE a fixture is written, so an
unreachable rung is a recorded refusal (§32's idiom), not a fake bundle: a bank of N
full-panel planes plus the base's own non-plate art. N=1 is a still (no bank); N
beyond CLK_BG_MAX never seats at the projection.

Usage: bench_rung_table.py --planes N --fps F [--base PATH] --out OUT [--name NAME]
Exit 0 with the file written; non-zero refusing an un-buildable rung.
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbbundle as ob          # noqa: E402
import theme_schema as ts       # noqa: E402
import orbtheme as ot           # noqa: E402
import bake_clock as bc         # noqa: E402  (rgb565_le)

DEF_BASE = os.environ.get("D32_BASE", "/tmp/b3/avkit.rebake.orbtheme")
PLANE_BYTES = 466 * 466 * 2      # full-panel RGB565 colour-only plane
# D29's art-region cap and the base's non-plate art (measured on the card):
# av-kit art 1,173,563 B with its plate plane (434,312 B) removed.
BASE_ART_NONPLATE = 1_173_563 - PLANE_BYTES
BUNDLE_CAP = 4_128_768           # D29 §23's 0x3F0000 art-region gate, both doors
CLK_BG_MAX = 16                  # clk_types.h: the projection's own plate cap


def keyframe(step, n):
    """One full-panel plane: the plate's warm field, and the medallion's spoke
    cross turned step*360/n degrees. The field shifts +6 luma per step so a
    fbgrab pair a bank stride apart tells one frame from the next by more than
    the spokes — the same honesty bench_d31_bundle.py demanded of its four frames."""
    im = Image.new("RGBA", (466, 466))
    d = ImageDraw.Draw(im)
    for y in range(466):
        v = 24 + y * 26 // 466 + 6 * step
        d.line([(0, y), (465, y)], fill=(v, v * 9 // 10, v * 7 // 10))
    c = 233
    r = 150
    d.ellipse([c - r, c - r, c + r, c + r], outline=(217, 179, 92), width=14)
    a0 = math.radians(360.0 / n * step)
    for k in range(4):
        a = a0 + k * math.pi / 2.0
        d.line([c - int(r * math.sin(a)), c - int(r * math.cos(a)),
                c + int(r * math.sin(a)), c + int(r * math.cos(a))],
               fill=(217, 179, 92), width=10)
    d.ellipse([c - 30, c - 30, c + 30, c + 30], fill=(70, 110, 200))
    d.ellipse([c - 12, c - 12, c + 12, c + 12], fill=(235, 225, 200))
    return im


def compute_ask(planes):
    """Bytes the bank asks for: N full-panel planes plus the base's own art.
    The refusal is decided on this number before a fixture is ever written."""
    return planes * PLANE_BYTES + BASE_ART_NONPLATE


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--planes", type=int, required=True)
    ap.add_argument("--fps", type=int, required=True)
    ap.add_argument("--base", default=DEF_BASE)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="")
    a = ap.parse_args()
    n = a.planes
    name = a.name or "rung-%02d" % n

    ask = compute_ask(n)
    print("rung N=%d @ %d fps: ask %d B (cap %d, %s)"
          % (n, a.fps, ask, BUNDLE_CAP,
             "OVER CAP" if ask > BUNDLE_CAP else "under cap"))

    # CLK_BG_MAX is the projection's bound; a bank past it never seats. Record it
    # as a refusal, do not fake a fixture (§32: an unreachable rung is a finding).
    if n < 2 or n > CLK_BG_MAX:
        print("REFUSE: N=%d outside the projection's 2..%d keyframe bound"
              % (n, CLK_BG_MAX))
        return 3
    if ask > BUNDLE_CAP:
        print("REFUSE: N=%d asks %d B, past D29's %d B art-region cap by %d B"
              % (n, ask, BUNDLE_CAP, ask - BUNDLE_CAP))
        return 4

    if not os.path.exists(a.base):
        sys.exit("no product bundle at %s — run rebake_bundle.py first" % a.base)
    hdr, secs = ob.read_bundle(a.base)
    clksec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    clk = clksec["blob"]
    els = [dict(e) for e in clk["elements"]]
    sc = dict(clk["scalars"])

    # Bank at the catalogue head (plate planes ride rows 0..N-1, frame 0 of the
    # loop is the plate); the base's plate plane is dropped, every other row's
    # frame0 pushed past the inserted keyframes. Identical to the D31 recipe.
    planes = [bc.rgb565_le(keyframe(i, n)) for i in range(n)]
    bank = b"".join(planes)
    spr0 = dict(clk["sprites"][0])
    art0 = clksec["art"][spr0["size"]:]
    art = bank + art0
    shift = n - 1
    for e in els:
        e["frame0"] += shift
    plate = [e for e in els if e["class"] == ts.CLASS_PLATE and e["slot"] == 0][0]
    plate.update(frame0=0, n_frames=n)
    sc.update(bg_frames=n - 1, bg_fps=a.fps, bg_loop=1, bg_every=1,
              bg_mode=0, gif_frames=0)

    spr = [dict(s) for s in clk["sprites"][1:]]
    w = h = 466
    sz = w * h * 2
    bank_len = len(bank)
    at = bank_len
    new = []
    for i in range(n):
        new.append({"off": i * sz, "size": sz, "w": w, "h": h})
    for s in spr:
        s["off"] += bank_len - spr0["size"]
        new.append(s)
        at += s["size"]
    assert at == len(art), "the catalogue must account for the whole region"

    out_secs = [{"slug": "clock", "elements": els, "scalars": sc,
                 "sprites": new, "art": art,
                 "scr_w": clksec["rec"]["scr_w"],
                 "scr_h": clksec["rec"]["scr_h"]}]
    ob.write_bundle(a.out, name, out_secs)

    h2, s2 = ob.read_bundle(a.out)
    c2 = [s for s in s2 if s["blob"]["slug"] == "clock"][0]["blob"]
    p2 = [e for e in c2["elements"] if e["class"] == ts.CLASS_PLATE and e["slot"] == 0][0]
    sc2 = c2["scalars"]
    assert p2["n_frames"] == n and p2["frame0"] == 0, p2
    assert sc2["bg_frames"] == n - 1 and sc2["gif_frames"] == 0, sc2
    assert c2["sprites"][0]["off"] == 0 and c2["sprites"][0]["size"] == sz
    print("  wrote %s: name %r id %s, bank %d keyframes @ %d fps loop, art %d B"
          % (a.out, h2["name"], ot.card_stem("clock", h2["name"]), n, a.fps, h2["art_size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())

