#!/usr/bin/env python3
"""bench_d31_bundle.py — the §31 animated-plate bench theme: the same av-kit dial,
its still plate replaced by a four-frame keyframe bank of a turning spoked medallion.

The bench question §31.1 left open is the glass, not the format: FORMAT 3's install
path has never driven orb_sched's bgAnim flipper, because every FORMAT 3 theme so far
was a still. This fixture is the smallest honest way to ask it:

  plate n 0  four whole-panel keyframes (row n_frames 4, scalars bg_frames 3,
             bg_loop 1, bg_fps 8, bg_mode 0, gif_frames 0) — the projection's §31
             cross-checks are the fixture's acceptance test, so the frames must be
             visibly ONE animation: a gold medallion whose spoke cross turns 90
             degrees per frame over a warm field, the same furniture the shipped
             av-kit plate carries (rim, spokes, blued hub) so the bank reads on
             fbgrab as that dial moving, not as noise.
  all else   the av-kit's own rows and planes, frame0 shifted by the inserted rows:
             glass, hands, shadows, numerals untouched, so the only animation on
             the glass is the ruled one.

Planes encode exactly as the §31 baker writes a bank: colour-only RGB565 at
scr_w*scr_h*2 B, contiguous, the plate's plane first at offset 0 (frame 0 of the
loop is the plate). The shipped C (verify_js_bake.py) must project the file before
the bench touches a board; the boot log then repeats the same figures.

    python3 tools/bench_d31_bundle.py [out.orbtheme]
"""
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

BASE = os.environ.get("D32_BASE", "/tmp/b3/avkit.rebake.orbtheme")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/d31bench.orbtheme"
NAME = "d31-anim"
BG_N, BG_FPS = 4, 8            # keyframes total / requested loop tempo


def keyframe(step):
    """One panel: the warm field, and the medallion's spoke cross at step*90 deg."""
    im = Image.new("RGBA", (466, 466))
    d = ImageDraw.Draw(im)
    # The field: vertical gradient of the plate's own warm darks, +6 lum per step
    # so a still frame of the bank is told from the next on the grab by more than
    # the spokes.
    for y in range(466):
        v = 24 + y * 26 // 466 + 6 * step
        d.line([(0, y), (465, y)], fill=(v, v * 9 // 10, v * 7 // 10))
    c = 233
    r = 150
    d.ellipse([c - r, c - r, c + r, c + r], outline=(217, 179, 92), width=14)
    a0 = math.radians(90 * step)
    for k in range(4):
        a = a0 + k * math.pi / 2.0
        d.line([c - int(r * math.sin(a)), c - int(r * math.cos(a)),
                c + int(r * math.sin(a)), c + int(r * math.cos(a))],
               fill=(217, 179, 92), width=10)
    d.ellipse([c - 30, c - 30, c + 30, c + 30], fill=(70, 110, 200))
    d.ellipse([c - 12, c - 12, c + 12, c + 12], fill=(235, 225, 200))
    return im


def main():
    if not os.path.exists(BASE):
        sys.exit("no product bundle at %s — run rebake_bundle.py first" % BASE)
    hdr, secs = ob.read_bundle(BASE)
    clksec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    clk = clksec["blob"]
    els = [dict(e) for e in clk["elements"]]
    sc = dict(clk["scalars"])
    art0 = clksec["art"]

    # The bank first (the plate's planes ride rows 0..N-1 of the catalogue, and
    # frame 0 of the loop is the plate), then the base's art from its row 1 up:
    # the base's plate plane is REPLACED by the bank, so it is dropped, and every
    # other row's frame0 is pushed past the inserted keyframes.
    planes = [bc.rgb565_le(keyframe(i)) for i in range(BG_N)]
    bank = b"".join(planes)
    spr0 = dict(clk["sprites"][0])
    art0 = clksec["art"][spr0["size"]:]
    art = bank + art0
    shift = BG_N - 1
    for e in els:
        e["frame0"] += shift
    plate = [e for e in els if e["class"] == ts.CLASS_PLATE and e["slot"] == 0][0]
    plate.update(frame0=0, n_frames=BG_N)
    sc.update(bg_frames=BG_N - 1, bg_fps=BG_FPS, bg_loop=1, bg_every=1,
              bg_mode=0, gif_frames=0)

    # The catalogue: N plate planes colour-only at the head, then the base's rows
    # 1.. re-offset as one block. read_bundle's sprite rows carry off/size/w/h.
    spr = [dict(s) for s in clk["sprites"][1:]]
    w = h = 466
    sz = w * h * 2
    bank_len = len(bank)
    at = bank_len
    new = []
    for i in range(BG_N):
        new.append({"off": i * sz, "size": sz, "w": w, "h": h})
    for s in spr:
        s["off"] += bank_len - spr0["size"]   # base art restarts after the bank
        new.append(s)
        at += s["size"]
    assert at == len(art), "the catalogue must account for the whole region"

    out_secs = [{"slug": "clock", "elements": els, "scalars": sc,
                 "sprites": new, "art": art,
                 "scr_w": clksec["rec"]["scr_w"],
                 "scr_h": clksec["rec"]["scr_h"]}]
    ob.write_bundle(OUT, NAME, out_secs)

    # Read the file back and prove the §31 story with the door's own answers.
    h2, s2 = ob.read_bundle(OUT)
    c2 = [s for s in s2 if s["blob"]["slug"] == "clock"][0]["blob"]
    p2 = [e for e in c2["elements"] if e["class"] == ts.CLASS_PLATE and e["slot"] == 0][0]
    sc2 = c2["scalars"]
    assert p2["n_frames"] == BG_N and p2["frame0"] == 0, p2
    assert sc2["bg_frames"] == BG_N - 1 and sc2["gif_frames"] == 0, sc2
    assert c2["sprites"][0]["off"] == 0 and c2["sprites"][0]["size"] == sz
    print("%s: name %r id %s, plate bank %d keyframes @ %d fps, art %d B"
          % (OUT, h2["name"], ot.card_stem("clock", h2["name"]), BG_N, BG_FPS,
             h2["art_size"]))


if __name__ == "__main__":
    main()
