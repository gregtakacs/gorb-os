#!/usr/bin/env python3
"""rebake_bundle.py - the reverse path's first half: FORMAT 2 -> FORMAT 3 (P6.5).

Reads a shipped FORMAT 2 .orbtheme and re-bakes it as a FORMAT 3 bundle whose clock
section carries a LEGIBLE element table (D32) over the same art bytes — so the device
reader P6.5 builds has a real product bundle to prove itself against, and av-kit's
pair can be reproduced from a bundle and not only from its source kit (the clause
0.2 still owes; the bundle -> pair half is the next commit, per 22.5).

The mapping is the ruling this tool records, and it is lossless BY CONSTRUCTION for
the clock dialect:

  plate        the 26-theme-field scalars ride verbatim (D31: the app's own dialect,
               opaque to every other app). plate plane = scr_w*scr_h*2 at plate_off.
  hands 0..4   class hand, slot = the FORMAT 2 array index (the offer's own vocabulary),
               show=0 rows carry EF_HIDDEN (the element is declared and bound, just
               off), x,y = the drawn top-left, pivot = (centerx-offx, centery-offy):
               the rotation center relative to the draw point. FORMAT 2's separate
               crop-pivot and screen-center collapse to ONE pair because every shipped
               sprite draws at center - crop_pivot == off: the .h's pivot == element
               pivot, and its center == (x+pivot_x, y+pivot_y). No column invented,
               nothing lost for the kits that exist.
  shadows      class sprite, slot = index, same placement rule. A shadow's own crop
               pivot (the light offset the source baked) is the .h's own field and
               rebuilds as center - off once center does.
  overlay      class plate, slot 1 (the crystal is a second plate), order past the
               hands regardless of the .h's order array (FORMAT 2's clock_view draws
               it last; the table says what the renderer already does).
  tourbillon   class hand, slots 5..20 (16 records, the FORMAT 2 array's shape), and
               a non-empty one is a REFUSAL at this step: av-kit carries none, and a
               kit whose fan-out this mapping has not proven must not be guessed at
               (D26's direction — say cannot-draw-yet, never draw wrong).
  gif/bank     a gif bank (n_rects > 0 with real rects) is refused for the same
               reason: the plate's extra frames ride the rect table's delta planes,
               whose per-rect byte size this first cut cannot prove contig.

Every refusal exits with the sentence, not a partial table. Verification is the
round trip: write_bundle, read_bundle, art bytes identical to the source's, and
sum(plane sizes) == art_size or the tool refuses (a bank this catalogue does not
account for byte-for-byte is a bank this mapping does not understand).

  python3 tools/rebake_bundle.py IN.orbtheme OUT.orbtheme [--name NAME] [--serial N]
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbtheme as ot
import orbbundle as ob
import theme_schema as ts


def refuse(what):
    sys.exit("refused: %s" % what)


def planes_of(g, scr_w, scr_h):
    """Every bank plane the FORMAT 2 geometry names, in BYTE order: (kind, idx, off,
    size, w, h). The plate is colour-only (no alpha plane); every sprite carries
    colour + alpha. Raises with the refusal sentence for any plane shape this
    mapping cannot prove."""
    sc = g["scalars"]
    if any(x["show"] and (x["w"] or x["h"]) for x in g["tourbillon"]):
        refuse("a tourbillon: the fan-out mapping is not yet ruled; this tool refuses "
               "to guess what it would draw")
    rects = g["rects"]
    nz = [(g["rects"][i * 5:i * 5 + 5]) for i in range(len(rects) // 5)]
    nz = [r for r in nz if r[4] and (r[2] or r[3])]
    # The scalars naming gif frames is a source fact, not a bank fact: a --no-bg
    # bake keeps the kit's scalars and carries none of the pixels. The bank itself
    # is the guard: real delta rects (a nonzero off) are the shape this cut has not
    # proven, and the byte-exact contig sum below is what an absent bank proves.
    if nz:
        refuse("a gif bank: the plate's extra frames ride delta rects whose byte sizes "
               "this first cut cannot prove contiguous; the mapping rules them in when "
               "a kit that needs them is on the bench")
    if g["comps"] or g["aux"] or g["bank"]:
        refuse("complication/aux/bank tables: the text and bank class rows ride the "
               "complication mapping, which lands with the kit that carries them")
    if sc["plate_off"] != 0:
        refuse("plate_off %d: the plate's plane must sit at bank offset 0 for this "
               "mapping's contig check (a FORMAT 2 bake never moves it; a FORMAT 3 "
               "reader never needs to)" % sc["plate_off"])
    planes = [("plate", 0, sc["plate_off"], scr_w * scr_h * 2, scr_w, scr_h)]
    for i, s in enumerate(g["hands"]):
        if s["show"] and s["w"] and s["h"]:
            px, py = s["centerx"] - s["offx"], s["centery"] - s["offy"]
            if not (0 <= px < s["w"] and 0 <= py < s["h"]):
                refuse("hand %d rotates about (%d,%d) of its own %dx%d plate; the "
                       "mapping cannot name a pivot off the glass" % (i, px, py, s["w"], s["h"]))
            planes.append(("hand", i, s["color_off"], s["w"] * s["h"] * 3, s["w"], s["h"]))
    for i, s in enumerate(g["shadows"]):
        if s["show"] and s["w"] and s["h"]:
            planes.append(("shadow", i, s["color_off"], s["w"] * s["h"] * 3, s["w"], s["h"]))
    ov = g["overlay"]
    if ov["show"] and ov["w"] and ov["h"]:
        planes.append(("overlay", 0, ov["color_off"], ov["w"] * ov["h"] * 3, ov["w"], ov["h"]))
    planes.sort(key=lambda p: p[2])
    return planes


def elements_of(planes, order):
    """One row per plane, in bank order; (class, slot) is the FORMAT 2 array address."""
    recs = []
    n = 0
    rank = {0: order[0], 1: order[1], 2: order[2], 3: order[3], 4: order[4]}
    for kind, idx, off, size, w, h in planes:
        e = ts.blank_element()
        e["n"] = n
        n += 1
        e["slot"] = idx
        e["frame0"] = len(recs)      # FORMAT 3's frame window indexes the bank's rect list
        e["n_frames"] = 1
        e["w"], e["h"] = w, h
        if kind == "plate":
            e["class"], e["order"] = ts.CLASS_PLATE, 0
        elif kind == "overlay":
            e["class"], e["slot"] = ts.CLASS_PLATE, 1
            e["order"] = 63            # below only ELEM_ORDER_N's ceiling; the .h's own
                                        # draw order puts the crystal over everything
        elif kind == "hand":
            e["class"], e["order"] = ts.CLASS_HAND, 2 * rank.get(idx, idx) + 1
            e["pivot_x"], e["pivot_y"] = 0, 0   # filled from the source below
        else:                           # shadow
            e["class"], e["order"] = ts.CLASS_SPRITE, 2 * rank.get(idx, idx)
        recs.append(e)
    return recs


def rebake(src, dst, name=None, serial=0):
    h, geom, art = ot.read_container(src)          # both checksums, or it raises
    if h["fmt"] != 2:
        refuse("input is FORMAT %d; this tool maps FORMAT 2 only" % h["fmt"])
    g = ot.unpack_geom(geom)
    scr_w, scr_h = h["scr_w"], h["scr_h"]
    planes = planes_of(g, scr_w, scr_h)
    total = sum(p[3] for p in planes)
    if total != h["art_size"]:
        refuse("the plane catalogue accounts for %d B of an art region of %d B; this "
               "mapping refuses a bank it cannot account for byte-for-byte"
               % (total, h["art_size"]))
    at = 0
    for kind, idx, off, size, w, hh in planes:
        if off != at:
            refuse("bank is not contiguous at %r[%d]: plane starts at %d, the previous "
                   "one ends at %d" % (kind, idx, off, at))
        at += size
    order = [g["scalars"]["order%d" % i] for i in range(5)]
    recs = elements_of(planes, order)
    # The hand pivot is placement truth: x,y are the drawn top-left, and FORMAT 2
    # draws a hand at (center - pivot): x = offx, pivot = center - offx (== pivotx for
    # every shipped hand; a shadow's crop pivot rebuilds from center - off).
    # The placement truth, from §25.2's identity: draw point = center - crop pivot,
    # so x,y = offx,offy and the element pivot = center - off (the .h's pivot for a
    # hand; the .h's center rebuilds as (x + pivot_x, y + pivot_y) for every row).
    for r, (kind, idx, off, size, w, hh) in zip(recs, planes):
        if kind == "hand":
            s = g["hands"][idx]
            r["x"], r["y"] = s["offx"], s["offy"]
            r["pivot_x"], r["pivot_y"] = s["centerx"] - s["offx"], s["centery"] - s["offy"]
        elif kind == "shadow":
            s = g["shadows"][idx]
            r["x"], r["y"] = s["offx"], s["offy"]
            r["pivot_x"], r["pivot_y"] = s["centerx"] - s["offx"], s["centery"] - s["offy"]
        elif kind == "overlay":
            r["x"], r["y"] = g["overlay"]["offx"], g["overlay"]["offy"]
    # A hand the source names but shows is the offer's slot the theme leaves
    # silent: the app's own default covers it (D31), and no row is more honest a
    # declaration than nothing. A row with EF_HIDDEN would claim a bank range the
    # art does not hold, which check_element would rightly refuse.
    scal = {n: v for n, v in g["scalars"].items()}
    # §22.2: the screen scalars MOVE into the section; the source header's pair
    # is the truth a FORMAT 3 reader must find in the record, not the file head.
    secs = [{"slug": "clock", "elements": recs, "scalars": scal,
             "scr_w": scr_w, "scr_h": scr_h,
             "sprites": [{"off": o, "size": sz, "w": w, "h": hh}
                         for _k, _i, o, sz, w, hh in planes],
             "art": art}]
    nm = name or h["name"]
    out = ob.write_bundle(dst, nm, secs, serial=serial,
                          baked_ms=int(__import__("time").time() * 1000) & 0xFFFFFFFF)
    hdr, got = ob.read_bundle(dst)
    nfail = verify(got[0], g, planes, art, scr_w, scr_h, hdr, name or h["name"])
    print("wrote %s: %d B, format %d, %d elements, art %d B; verify %d failure(s)"
          % (dst, out, hdr["fmt"], len(recs), len(art), nfail))
    # The identity the device keys on: the name hashed to the card stem.
    print("  name %-20r -> card file %s.orbtheme" % (nm, ot.theme_id(nm)))
    if nfail:
        refuse("the re-bake failed its own round trip: %d field(s) disagree with "
               "the source geometry above" % nfail)
    return hdr, got


def verify(sec, g, planes, art, scr_w, scr_h, hdr, want_name):
    """The round trip, run on EVERY re-bake: re-read what write_bundle emitted and
    assert every mapped field against the source geometry, the sprite catalogue
    against the byte walk, and the bank byte-identical. A tool that only writes
    cannot tell you the mapping lost something; this one prints each disagreement
    and exits nonzero. Returns the failure count."""
    fails = [0]

    def ck(cond, msg):
        if not cond:
            fails[0] += 1
            print("FAIL:", msg)

    blob = sec["blob"]
    E = {(e["class"], e["slot"]): e for e in blob["elements"]}
    r = sec["rec"]
    ck(r["scr_w"] == scr_w and r["scr_h"] == scr_h,
       "record screen %dx%d, source %dx%d" % (r["scr_w"], r["scr_h"], scr_w, scr_h))
    ck(sec["art"] == art, "art bytes differ from the source's")
    ck(hdr["name"] == want_name, "name identity: header %r, wanted %r"
       % (hdr["name"], want_name))
    # frame0 is the row's seat in the byte-ordered rect list — the mapping's
    # claim that a FORMAT 3 reader can find every plane by walking the table.
    f0 = {(k, i): n for n, (k, i, _o, _s, _w, _h) in enumerate(planes)}
    byidx = {}
    for sp in blob["sprites"]:
        byidx.setdefault((sp["off"], sp["size"], sp["w"], sp["h"]),
                        blob["sprites"].index(sp))
    at = 0
    for kind, idx, off, size, w, hh in planes:
        sp = byidx.get((off, size, w, hh))
        ck(sp is not None, "catalogue lost the %r[%d] plane %dx%d at %d"
           % (kind, idx, w, hh, off))
        ck(off == at, "catalogue not in byte order at %r[%d]" % (kind, idx))
        at += size
    ck(at == len(art), "catalogue accounts for %d of %d bank bytes" % (at, len(art)))
    for e in blob["elements"]:
        cls = e["class"]
        if cls == ts.CLASS_PLATE and e["slot"] == 0:
            ck((e["x"], e["y"], e["w"], e["h"]) == (0, 0, scr_w, scr_h), "plate extent")
            ck(e["order"] == 0, "plate not first")
            ck(e["frame0"] == f0[("plate", 0)], "plate frame0")
        elif cls == ts.CLASS_PLATE:
            o = g["overlay"]
            ck((e["x"], e["y"]) == (o["offx"], o["offy"]), "overlay top-left")
            ck(e["order"] == 63, "overlay not last")
            ck(e["frame0"] == f0[("overlay", 0)], "overlay frame0")
        elif cls == ts.CLASS_HAND:
            hd = g["hands"][e["slot"]]
            ck((e["x"], e["y"]) == (hd["offx"], hd["offy"]), "hand %d top-left" % e["slot"])
            ck((e["pivot_x"], e["pivot_y"]) == (hd["centerx"] - hd["offx"],
                                                hd["centery"] - hd["offy"]),
               "hand %d pivot" % e["slot"])
            ck(e["order"] == 2 * g["scalars"]["order%d" % e["slot"]] + 1,
               "hand %d order" % e["slot"])
            ck(e["frame0"] == f0[("hand", e["slot"])], "hand %d frame0" % e["slot"])
        elif cls == ts.CLASS_SPRITE:
            sh = g["shadows"][e["slot"]]
            ck((e["x"], e["y"]) == (sh["offx"], sh["offy"]), "shadow %d top-left" % e["slot"])
            ck((e["pivot_x"], e["pivot_y"]) == (sh["centerx"] - sh["offx"],
                                                sh["centery"] - sh["offy"]),
               "shadow %d center-as-pivot" % e["slot"])
            ck(e["order"] == 2 * g["scalars"]["order%d" % e["slot"]],
               "shadow %d order" % e["slot"])
            ck(e["frame0"] == f0[("shadow", e["slot"])], "shadow %d frame0" % e["slot"])
        if cls == ts.CLASS_HAND:
            # The shipped identity the whole placement rule rests on: for a hand,
            # the .h's crop pivot EQUALS center - off. A source that broke it
            # breaks the mapping's claim to rebuild both, and verify says so.
            hd = g["hands"][e["slot"]]
            ck(hd["pivotx"] == hd["centerx"] - hd["offx"]
               and hd["pivoty"] == hd["centery"] - hd["offy"],
               "hand %d: crop pivot != center - off; the mapping's identity does "
               "not hold for this source" % e["slot"])
    for k, v in g["scalars"].items():
        ck(blob["scalars"][k] == v, "scalar %s: %r != %r" % (k, blob["scalars"][k], v))
    return fails[0]


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--name", default=None)
    ap.add_argument("--serial", type=int, default=0)
    a = ap.parse_args()
    rebake(a.src, a.dst, a.name, a.serial)
