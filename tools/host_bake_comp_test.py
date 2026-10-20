#!/usr/bin/env python3
"""Bench check for the complication producer in tools/bake_clock.py.

    python3 tools/host_bake_comp_test.py

bake_clock.bake_complications() is the ONE place a kit's complications/ folder and
manifest layout become theme_schema records; pack_geom then lays them down and the
firmware's orb_clock3_parse() reads them back. This test drives that whole
Python span — bake, pack, unpack — against a synthetic kit it builds itself, so it
needs neither the microSD harness nor a vendor kit on disk. It pins the parts a
mistake would silently get wrong:

  * a window packs RAW with a planar alpha (w*h*3), never under-dial (w*h*2): a
    tile given the wrong stride makes the reader step into the next frame, which
    reads as scrambled art, not an error;
  * weekday and month tiles are indexed Monday-first / January-first no matter how
    the kit's own directory lists them, because the value the device derives from
    the clock is positional, and a theme and device that disagree about which frame
    "Saturday" is produce a dial one day wrong;
  * tiles pack contiguously with no gap and no overlap, and every frame stays
    inside the art payload the container will carry.

The classic-kit path (no manifest, no complications/) is checked too: it must yield
empty tables so pack_geom still emits the old four-count blob and a non-calendar
theme is byte-for-byte the theme it always was.
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image                                    # noqa: E402
import bake_clock as bc                                  # noqa: E402
import orbtheme as ot                                    # noqa: E402
import theme_schema as ts                                # noqa: E402

FAILS = []


def ok(cond, label):
    if cond:
        print("ok    " + label)
    else:
        FAILS.append(label)
        print("FAIL  " + label)


def _tile(w, h, colour):
    """A solid RGBA tile, fully opaque so bbox/crop and the stride maths see a
    real box."""
    return Image.new("RGBA", (w, h), colour)


def _write_tiles(root, names, w, h, colour):
    os.makedirs(root, exist_ok=True)
    for n in names:
        _tile(w, h, colour).save(os.path.join(root, n + ".png"))


def build_kit(base):
    """A tiny calendar kit: a weekday window (c2), a month window (c3), a moon
    window (c5) hosted by the (skipped) subdial c4. The tile files are written in
    an arbitrary order so the bake's Monday-first / January-first indexing is the
    thing under test, not the directory listing."""
    theme = os.path.join(base, "theme")
    os.makedirs(theme)
    manifest = {
        "size": 466,
        "layout": {"comps": [
            {"id": "c2", "type": "window", "host": "dial", "angle": 270, "dist": 91,
             "shows": "day", "size": 1},
            {"id": "c3", "type": "window", "host": "dial", "angle": 90, "dist": 91,
             "shows": "month", "size": 1},
            {"id": "c4", "type": "subdial", "host": "dial", "angle": 180, "dist": 102,
             "size": 62, "scale": "date"},
            # underLayer is what prep_watch_kit writes from the kit's drawOrder;
            # the two-host case is the whole point — the moon belongs under the
            # c4 plate (bucket 4), the apertures on the dial face do not.
            {"id": "c5", "type": "moon", "host": "c4", "angle": 0, "dist": 2,
             "size": 29, "underLayer": 4},
        ]},
    }
    json.dump(manifest, open(os.path.join(theme, "manifest.json"), "w"))
    _write_tiles(os.path.join(theme, "complications", "c2_day"),
                 ["SUN", "SAT", "FRI", "THU", "WED", "TUE", "MON"], 60, 30,
                 (10, 20, 30, 255))
    _write_tiles(os.path.join(theme, "complications", "c3_month"),
                 list(reversed(bc._MONTH_TILES)), 60, 30, (30, 20, 10, 255))
    _write_tiles(os.path.join(theme, "complications", "c5_moon"),
                 ["%02d" % i for i in range(30)], 56, 28, (5, 5, 5, 255))
    # The sub-dial pointer prep emits: a hand and its blur/offset twin, each on
    # a full canvas with its pivot already on the sub-dial's centre (233,335),
    # which is what bake_aux's crop-to-ink then moves into the sprite's offx/offy.
    _aux_pointer(os.path.join(theme, "clock_aux_0.png"), (200, 190, 40, 255))
    _aux_pointer(os.path.join(theme, "clock_aux_0_shadow.png"), (0, 0, 0, 128))
    return theme


def _aux_pointer(path, colour):
    im = Image.new("RGBA", (bc.SCREEN_W, bc.SCREEN_H), (0, 0, 0, 0))
    px = im.load()
    for y in range(335 - 56, 335 + 14):                 # 56 above the pivot, 14 under
        for x in range(233 - 3, 233 + 4):
            px[x, y] = colour
    im.save(path)


# The style entry that pairs those two sprites with how to drive them, exactly
# as prep_watch_kit writes it into clock_style.json's `aux` list.
AUX_STYLE = {"aux": [{"file": "clock_aux_0.png", "tag": 4, "src": "date", "kind": 0,
                     "host": 4, "zero": 0, "span": 3600, "valueMin": 1,
                     "valueMax": 31, "jump": True, "show": True,
                     "pivotX": 233, "pivotY": 335, "centerX": 233, "centerY": 335,
                     "blend": 0, "shadowFile": "clock_aux_0_shadow.png",
                     "shadowPivotX": 17, "shadowPivotY": 63,
                     "shadowDx": 2, "shadowDy": 2}]}


def roundtrip(comps, aux, bank):
    """Pack records through the shipped writer and read them back through the
    shipped reader, exactly as the container round-trips them."""
    scalars = {n: 0 for n, _ in ot.THEME_FIELDS}
    geom = ot.pack_geom(scalars, [ot.blank_spr()] * 5, [ot.blank_spr()] * 5,
                        ot.blank_spr(), [], [], [], [], [],
                        comps=comps, aux=aux, bank=bank)
    return ot.unpack_geom(geom)



def main():
    base = tempfile.mkdtemp(prefix="orb_comp_kit_")
    try:
        theme = build_kit(base)

        # ---- classic kit: no manifest, no complications -> four-count blob -----
        empty_dir = os.path.join(base, "empty")
        os.makedirs(empty_dir)
        eblob = bc.Blob()
        oc, oa, ob = bc.bake_complications(empty_dir, eblob, verbose=False)
        ok(oc == [] and oa == [] and ob == [], "classic kit yields empty comp tables")
        ok(len(eblob.buf) == 0, "classic kit appends no pixels")

        # ---- calendar kit: three windows, correct sources -------------------
        blob = bc.Blob()
        comps, aux, bank = bc.bake_complications(theme, blob, verbose=False)
        ok(len(comps) == 3, "three window comps baked (c4 subdial + opening skipped)")
        ok(aux == [] and bank == [], "windows need no aux/bank (they address the blob)")

        srcs = sorted(ts.SOURCES[c["src"]] for c in comps)
        ok(srcs == ["month", "moon_age", "weekday"], "sources weekday/month/moon_age: %s" % srcs)
        by = {ts.SOURCES[c["src"]]: c for c in comps}

        # ---- raw planar-alpha stride, never under-dial ----------------------
        for name, c in by.items():
            ok((c["flags"] & ts.FLAG_UNDER_DIAL) == 0,
               "%s window is NOT under-dial (runtime composite)" % name)
            ok(c["frame_bytes"] == c["w"] * c["h"] * 3, "%s stride is w*h*3" % name)
            ok(c["kind"] == ts.KINDS.index("window"), "%s kind is window" % name)

        # ---- Monday-first / January-first, independent of directory order ----
        ok(by["weekday"]["count"] == 7, "weekday window has 7 tiles")
        ok(by["month"]["count"] == 12, "month window has 12 tiles")
        ok(by["moon_age"]["count"] == 30, "moon window has 30 tiles")
        ok(by["month"]["value_min"] == 0 and by["weekday"]["value_min"] == 0,
           "weekday/month frame 0 stands for value 0")

        # ---- geometry: box centred on the polar placement, contiguous packing --
        day = by["weekday"]
        cx = 466 // 2
        ok(abs(day["x"] + day["w"] // 2 - (cx - 91)) <= 1,
           "270deg window sits left of centre: x=%d" % day["x"])
        # ---- host-relative placement: the moon rides its SUBDIAL's centre ------
        # c4 sits at angle 180 dist 102 (six o'clock); the c5 moon at angle 0
        # dist 2 on host c4 is 2 px ABOVE that centre — 335 - 2 = 333 — and that
        # point is the middle of the tile's BOTTOM edge, not its centre. The fan
        # arc then rises from the arch baseline it is authored to sit on.
        # Before the bake computed centres for skipped kinds the host fell back to
        # the dial centre and the moon drew over the pinion.
        moon = by["moon_age"]
        ok(abs(moon["y"] + moon["w"] // 2 - 333) <= 1,
           "hosted fan moon is bottom-edge anchored on the subdial: y=%d w=%d"
           % (moon["y"], moon["w"]))
        ok(abs(moon["x"] + moon["w"] // 2 - 233) <= 1,
           "hosted moon stays on the subdial's vertical axis: x=%d" % moon["x"])
        ok(min(c["off"] for c in comps) == 0, "first tile at blob offset 0")
        ends = [c["off"] + c["frame_bytes"] * c["count"] for c in comps]
        ok(max(ends) == len(blob.buf), "comp tiles fill the payload with no trailing gap")
        for c in comps:
            ok(c["off"] + c["frame_bytes"] * c["count"] <= len(blob.buf),
               "comp src=%s stays inside the payload" % ts.SOURCES[c["src"]])

        # ---- the z bucket: a hosted window is kept UNDER its own plate -------
        # For a WINDOW, `first` is not a bank index — it is the KINDS slot whose
        # live layer the tile must stay under. The moon under the c4 plate needs
        # 4 or the plate's opaque arch covers it whole (the owner's original
        # "the moon is missing" report), while the dial's own apertures must keep
        # 0 so nothing lifts them above the carriage. A wrong value here is a
        # silently invisible or silently over-painted window, never an error, so
        # it is pinned at the producer.
        ok(by["moon_age"]["first"] == 4,
           "moon hosted by the subdial is bucketed under static2: first=%d"
           % by["moon_age"]["first"])
        ok(by["weekday"]["first"] == 0 and by["month"]["first"] == 0,
           "dial-hosted apertures stay below every live layer")
        ok(abs(by["moon_age"]["y"] + by["moon_age"]["w"] // 2 - 333) <= 1,
           "the bucketed moon still rides its host's centre by its bottom edge")

        # ---- the anchor is the shape's, not the source's ---------------------
        # Three kits differing only in the moon's style/rot must disagree about
        # y and agree about x. A fan (style classic, rot 0) is authored as the arc
        # from its baseline upward, so the host point is its bottom edge; the
        # composer's ROUND_MOON styles and any rotating moon are drawn as a whole
        # circle, so the same point is their centre. Getting this backwards puts
        # the moon a radius off its window — the misalignment the owner reported —
        # while every other number in the record stays correct, so nothing else
        # would ever notice. h is 28 here and w 56: only the anchor rule decides
        # whether y is 305 or 319.
        for style, rot, want, label in (
                ("classic", 0, 333 - 28, "fan, unrotated"),
                ("sphere", 0, 333 - 14, "round (ROUND_MOON)"),
                ("graphic", 0, 333 - 14, "graphic (ROUND_MOON)"),
                ("neon",    0, 333 - 14, "neon (ROUND_MOON)"),
                ("classic", 45, 333 - 14, "fan, rotated")):
            rdir = os.path.join(base, "anchor_%s_%s" % (style, rot))
            os.makedirs(os.path.join(rdir, "complications", "c5_moon"))
            for n in ["%02d" % i for i in range(30)]:
                _tile(56, 28, (5, 5, 5, 255)).save(
                    os.path.join(rdir, "complications", "c5_moon", n + ".png"))
            json.dump({"layout": {"comps": [
                {"id": "c4", "type": "subdial", "host": "dial", "angle": 180,
                 "dist": 102, "size": 62, "scale": "date"},
                {"id": "c5", "type": "moon", "host": "c4", "angle": 0, "dist": 2,
                 "size": 29, "style": style, "rot": rot, "underLayer": 4}]}},
                open(os.path.join(rdir, "manifest.json"), "w"))
            rc = bc.bake_complications(rdir, bc.Blob(), verbose=False)[0]
            ok(len(rc) == 1 and rc[0]["y"] == want,
               "moon style=%s rot=%s (%s) anchors at y=%d, got %s"
               % (style, rot, label, want, [c["y"] for c in rc]))
            ok(len(rc) == 1 and rc[0]["x"] == 233 - 28,
               "moon style=%s rot=%s stays x-centred on its host: x=%s"
               % (style, rot, [c["x"] for c in rc]))

        # ---- round-trip through the shipped packer + reader -------------------
        u = roundtrip(comps, aux, bank)
        ok(u["n_counts"] == 7, "non-empty comps make a seven-count blob")
        ok(len(u["comps"]) == 3, "reader recovers all 3 comps")
        drift = []
        for a, b in zip(comps, u["comps"]):
            for f in (n for n, _ in ts.COMP_FIELDS):
                if a[f] != b[f]:
                    drift.append("%s.%s %r!=%r" % (ts.SOURCES[a["src"]], f, a[f], b[f]))
        ok(not drift, "every comp field survives pack/unpack: %s" % drift[:3])

        # ---- refusals: raise, never silently mispack -------------------------
        os.remove(os.path.join(theme, "complications", "c2_day", "WED.png"))
        try:
            bc.bake_complications(theme, bc.Blob(), verbose=False)
            ok(False, "missing weekday tile is refused (returned normally)")
        except SystemExit:
            ok(True, "missing weekday tile is refused")

        bad = os.path.join(base, "bad")
        os.makedirs(os.path.join(bad, "complications", "c2_day"))
        for n in bc._WEEKDAY_TILES:
            _tile(8, 8, (1, 1, 1, 255)).save(os.path.join(bad, "complications", "c2_day", n + ".png"))
        json.dump({"layout": {"comps": [
            {"id": "c2", "type": "window", "host": "dial", "angle": 0, "dist": 0,
             "shows": "barometer"}]}}, open(os.path.join(bad, "manifest.json"), "w"))
        try:
            bc.bake_complications(bad, bc.Blob(), verbose=False)
            ok(False, "unknown source word raises (accepted silently)")
        except KeyError:
            ok(True, "unknown manifest source word raises, not a silent frame-0 comp")
        except BaseException as e:                    # noqa: BLE001
            ok(False, "unknown source raised %s, expected KeyError" % type(e).__name__)

        # The blocks below re-bake the kit: put back the tile the refusal test
        # just deleted — it has already proved its point.
        _write_tiles(os.path.join(theme, "complications", "c2_day"),
                     ["WED"], 60, 30, (10, 20, 30, 255))

        # ---- prep: a complication between dial and subdial still ships it -----
        # The perpetual-calendar kit's REAL drawOrder: the c5 moon is listed
        # between the dial and the c4 plate. A break at the complication entry
        # never collects the plate after it, prep prints "no subdial in this
        # kit", and the dial loses its day ring under the moon — exactly what
        # the owner saw on the panel after the first load of this kit.
        import prep_watch_kit as pw
        porder = [
            {"layer": "base", "files": "base/frame_*.png"},
            {"layer": "complications", "where": "under dial", "items": ["c2", "c3"]},
            {"layer": "dial", "file": "layers/dial.png"},
            {"layer": "complications", "where": "under subdial c4", "items": ["c5"]},
            {"layer": "subdial", "id": "c4", "file": "layers/subdial_01_c4.png"},
            {"layer": "hands"},
            {"layer": "glass", "file": "layers/glass.png"},
        ]
        dfile, subs, merged = pw._static_run(porder)
        ok(dfile == "layers/dial.png" and subs == ["layers/subdial_01_c4.png"]
           and merged is False,
           "subdial listed AFTER the complication is collected, unmerged: %r %r"
           % (subs, merged))
        df2, subs2, merged2 = pw._static_run(
            [o for o in porder if not (o["layer"] == "complications" and o.get("items"))])
        ok(df2 == "layers/dial.png" and subs2 == ["layers/subdial_01_c4.png"]
           and merged2 is True,
           "with nothing in the gap the subdial still merges into static1: %r %r"
           % (subs2, merged2))

        # ---- matte proof: the dial frame above a window is never folded --------
        # A fake of the fold this kit's art really makes: static1 (the dial, as
        # prep emits it) carries ink across the weekday window's box — the bezel
        # the tile tucks UNDER at runtime. With the boxes in the stack the proof
        # must refuse the fold (a folded dial would sit below the tile and the
        # window would show bare, frameless — the owner's second complaint).
        # With no manifest the identical static1 must still fold: the refusal is
        # a proof outcome, not a blanket rule that costs every classic kit a
        # layer of blend traffic.
        import types
        day = by["weekday"]
        s1 = Image.new("RGBA", (bc.SCREEN_W, bc.SCREEN_H), (0, 0, 0, 0))
        s1p = s1.load()
        for yy in range(day["y"] - 6, day["y"] + day["h"] + 6):
            for xx in range(day["x"] - 6, day["x"] + day["w"] + 6):
                s1p[xx, yy] = (9, 9, 9, 255)
        s1.save(os.path.join(theme, "clock_hand_static1.png"))
        style = {"hands": {"order": [3], "shadow": {"on": False},
                           "static1": {"show": True, "pivotX": 233, "pivotY": 233,
                                       "centerX": 233, "centerY": 233, "blend": 0}}}
        pivots = {"static1": (233, 233, 233, 233, 0)}
        margs = types.SimpleNamespace(pad=2, tourbillon=None,
                                      bg_gif=None, bg_frames=None)
        live = bc.build_live_stack(theme, style, pivots, margs, None)
        comp_names = sorted(L.name for L in live if L.name.startswith("comp["))
        ok(comp_names == ["comp[month]", "comp[moon_age]", "comp[weekday]"],
           "every drawn window rides the proof stack: %s" % comp_names)
        chosen, lines = bc.matte_plan(live, "auto")
        ok(all(L.name != "static1" for L in chosen),
           "dial frame over a window box: fold REFUSED, not silently taken: %s"
           % [L.name for L in chosen])
        ok(all(not L.name.startswith("comp[") for L in chosen),
           "a value-driven comp box is never a fold candidate (would freeze): %s"
           % [L.name for L in chosen])
        ok(any("REFUSED" in ln and "static1" in ln for ln in lines),
           "the refusal is printed with its violation numbers: %s"
           % [ln for ln in lines if "static1" in ln][:1])
        mp = os.path.join(theme, "manifest.json")
        cd = os.path.join(theme, "complications")
        os.rename(mp, mp + ".off"); os.rename(cd, cd + ".off")
        live_n = bc.build_live_stack(theme, style, pivots, margs, None)
        chosen_n, _ = bc.matte_plan(live_n, "auto")
        os.rename(mp + ".off", mp); os.rename(cd + ".off", cd)
        ok([L.name for L in chosen_n] == ["static1"],
           "the same static1 with no windows below it still folds: %s"
           % [L.name for L in chosen_n])

        # ---- pre-occlusion gate: the exactness argument rests on ==255 only ---
        # Refused statics ride the bank packer as opaque-only copies because
        # the live layer re-blits identical pixels over them. One sub-opaque
        # texel riding along would be composited twice — these two pins are
        # what keeps that trade lossless rather than merely cheap.
        src = Image.new("RGBA", (8, 8), (10, 20, 30, 247))
        sp = src.load(); sp[0, 0] = (1, 2, 3, 255); sp[3, 4] = (4, 5, 6, 255)
        gp = bc._opaque_only(src).load()
        ok(gp[0, 0][:3] == (1, 2, 3) and gp[0, 0][3] == 255
           and gp[3, 4][3] == 255 and gp[1, 1][3] == 0 and gp[7, 7][3] == 0,
           "_opaque_only keeps only a==255 texels, colours untouched")
        basex = Image.new("RGBA", (8, 8), (8, 8, 8, 255))   # exact in 565
        tinted = bc._opaque_only(Image.new("RGBA", (8, 8), (0, 0, 0, 247)))
        gone = bc.matte_fold(basex, tinted).getpixel((4, 4))[:3]
        ok(gone == (8, 8, 8),
           "a sub-opaque layer pre-occludes nothing: base survives bit-exact")

        # ---- aux: a sub-dial pointer packs as a sprite pair + 9 driving words --
        ab = bc.Blob()
        arows = bc.bake_aux(theme, AUX_STYLE, ab, margs, verbose=False)
        ok(len(arows) == 2, "one pointer with its shadow is TWO rows: %d" % len(arows))
        hand, twin = arows
        ok(ts.SOURCES[hand["src"]] == "date" and hand["kind"] == 0,
           "the hand row is a rotating date pointer")
        ok(hand["value_min"] == 1 and hand["value_max"] == 31 and hand["span"] == 3600,
           "a date ring turns once across 1..31: %s"
           % [hand[k] for k in ("value_min", "value_max", "span")])
        ok(hand["flags"] & ts.FLAG_JUMP and not (hand["flags"] & ts.FLAG_AUX_SHADOW),
           "the kit's snap is JUMP on the hand, and the hand is not its own twin")
        ok(bool(twin["flags"] & ts.FLAG_AUX_SHADOW), "the second row is the shadow twin")
        ok((twin["centerx"], twin["centery"]) == (hand["centerx"] + 2, hand["centery"] + 2)
           and (twin["pivotx"], twin["pivoty"]) == (hand["pivotx"], hand["pivoty"]),
           "the twin shares the hand's pivot and moves only its centre")
        ok(hand["color_off"] + hand["w"] * hand["h"] * 3 == twin["color_off"],
           "the pair packs contiguously, twin right after hand")
        au = roundtrip([], arows, [])
        ok(au["n_counts"] == 7 and len(au["aux"]) == 2,
           "aux rows make a seven-count blob the reader reads back whole")
        adrift = [f for a, b in zip(arows, au["aux"])
                  for f, _t in ts.AUX_FIELDS if a[f] != b[f]]
        ok(not adrift, "every aux field survives pack/unpack: %s" % adrift[:4])
        try:
            bc.bake_aux(theme, {"aux": [dict(AUX_STYLE["aux"][0], valueMax=0)]},
                        bc.Blob(), margs, verbose=False)
            ok(False, "value_max 0 was accepted (a sweep cannot divide by it)")
        except SystemExit:
            ok(True, "an undivisible value_max is refused at the bench")
        try:
            bc.bake_aux(theme, {"aux": [dict(AUX_STYLE["aux"][0], file="nope.png")]},
                        bc.Blob(), margs, verbose=False)
            ok(False, "a style naming an absent sprite returned normally")
        except SystemExit:
            ok(True, "a style whose sprite is absent is refused, not silently hidden")

        # ---- matte: the plate over a hosted window is never folded -------------
        # static2 (the c4 plate) covering the moon's box is exactly the fold that
        # buried this kit's moon. Placing the box at its BUCKET is what lets the
        # proof see it: the same code, and the opposite verdict from the case with
        # no manifest, which still folds.
        moonr = by["moon_age"]
        s2 = Image.new("RGBA", (bc.SCREEN_W, bc.SCREEN_H), (0, 0, 0, 0))
        s2p = s2.load()
        for yy in range(moonr["y"] - 4, moonr["y"] + moonr["h"] + 4):
            for xx in range(moonr["x"] - 4, moonr["x"] + moonr["w"] + 4):
                s2p[xx, yy] = (7, 7, 7, 255)
        s2.save(os.path.join(theme, "clock_hand_static2.png"))
        st2 = {"hands": {"order": [4], "shadow": {"on": False},
                         "static2": {"show": True, "pivotX": 233, "pivotY": 233,
                                     "centerX": 233, "centerY": 233, "blend": 0}}}
        live2 = bc.build_live_stack(theme, st2,
                                    {"static2": (233, 233, 233, 233, 0)}, margs, None)
        nm2 = [L.name for L in live2]
        ok(nm2.index("comp[moon_age]") < nm2.index("static2"),
           "the hosted window rides the proof BELOW its plate: %s" % nm2)
        ok(nm2.index("comp[weekday]") < nm2.index("comp[moon_age]"),
           "a bucket-0 window rides the whole list: %s" % nm2)
        chosen2, lines2 = bc.matte_plan(live2, "auto")
        ok(all(L.name != "static2" for L in chosen2),
           "the plate over a hosted window: fold REFUSED: %s" % [L.name for L in chosen2])
        ok(any("REFUSED" in ln and "static2" in ln for ln in lines2),
           "the refusal names the plate and its violation count: %s"
           % [ln for ln in lines2 if "static2" in ln][:1])

        # ---- re-bucket: a FORCED fold must not leave a window orphaned ---------
        # --matte force takes the fold the proof refused. A record still naming
        # the now-dead bucket would tell the reader to keep the tile under a layer
        # that never paints, so the bake moves it and prints the change.
        untouched = [c["first"] for c in comps if ts.SOURCES[c["src"]] != "moon_age"]
        changed = bc.rebucket_comps(comps, 1 << 4)
        ok([c[0] for c in changed] == ["moon_age"],
           "only the plate-hosted window is re-bucketed: %s" % changed)
        m = [c for c in comps if ts.SOURCES[c["src"]] == "moon_age"][0]
        ok(m["first"] == 0, "its bucket is cleared once the plate is in the bank")
        ok([c["first"] for c in comps if ts.SOURCES[c["src"]] != "moon_age"]
           == untouched, "a bucket nobody folded is left exactly as prep wrote it")
        ok(bc.rebucket_comps([{"src": ts.SOURCES.index("moon_age"), "first": 4}],
                             1 << 3) == [],
           "masking static1 does not touch a static2-hosted window")

        # ---- an underLayer that is not a layer slot is refused, not ignored ----
        bad2 = os.path.join(base, "bad_bucket")
        os.makedirs(os.path.join(bad2, "complications", "c2_day"))
        for n in bc._WEEKDAY_TILES:
            _tile(8, 8, (1, 1, 1, 255)).save(
                os.path.join(bad2, "complications", "c2_day", n + ".png"))
        json.dump({"layout": {"comps": [
            {"id": "c2", "type": "window", "host": "dial", "angle": 0, "dist": 0,
             "shows": "day", "underLayer": 9}]}},
            open(os.path.join(bad2, "manifest.json"), "w"))
        try:
            bc.bake_complications(bad2, bc.Blob(), verbose=False)
            ok(False, "underLayer 9 was accepted (not a KINDS slot)")
        except SystemExit:
            ok(True, "an underLayer outside the 5 layer slots is refused")

        # ---- prep derives the bucket from the kit's drawOrder, not a guess ------
        import prep_watch_kit as pw2
        perp_order = [
            {"layer": "base"},
            {"layer": "complications", "where": "under dial", "items": ["c2", "c3"]},
            {"layer": "dial", "file": "layers/dial.png"},
            {"layer": "complications", "where": "under subdial c4", "items": ["c5"]},
            {"layer": "subdial", "id": "c4", "file": "layers/subdial_01_c4.png"},
            {"layer": "hands"}, {"layer": "glass"}, {"layer": "outer"},
        ]
        man = {"layout": {"comps": [
            {"id": "c2", "host": "dial"}, {"id": "c3", "host": "dial"},
            {"id": "c4", "host": "dial"}, {"id": "c5", "host": "c4"}]}}
        got = pw2._under_layers(perp_order, man, merged=False)
        ok(got == {"c2": 0, "c3": 0, "c4": 0, "c5": 4},
           "the moon under a live subdial plate is the only bucket-4 comp: %s" % got)
        ok(set(pw2._under_layers(perp_order, man, merged=True).values()) == {0},
           "a subdial merged into the dial leaves nothing to sit under: all bucket 0")
        nested = {"layout": {"comps": [
            {"id": "c4", "host": "dial"}, {"id": "c5", "host": "c4"},
            {"id": "c6", "host": "c5"}]}}
        ok(pw2._under_layers(perp_order, nested, merged=False)["c6"] == 4,
           "a comp hosted by a hosted comp still walks up to the plate")
        loop = {"layout": {"comps": [{"id": "a", "host": "b"}, {"id": "b", "host": "a"}]}}
        ok(set(pw2._under_layers(perp_order, loop, merged=False).values()) == {0},
           "a host cycle terminates (bounded walk) instead of hanging")
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if FAILS:
        print("\n%d check(s) failed" % len(FAILS))
        return 1
    print("\nall complication-producer checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
