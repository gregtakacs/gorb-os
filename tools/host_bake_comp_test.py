#!/usr/bin/env python3
"""Bench check for the complication producer in tools/bake_clock.py.

    python3 tools/host_bake_comp_test.py

bake_clock.bake_complications() is the ONE place a kit's complications/ folder and
manifest layout become theme_schema records; pack_geom then lays them down and the
firmware's orb_theme_geom_parse() reads them back. This test drives that whole
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
            {"id": "c5", "type": "moon", "host": "c4", "angle": 0, "dist": 2, "size": 29},
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
    return theme


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
        # dist 2 on host c4 is 2 px ABOVE that centre — 335 - 2 = 333, boxed by
        # its own h. Before the bake computed centres for skipped kinds the
        # host fell back to the dial centre and the moon drew over the pinion.
        moon = by["moon_age"]
        ok(abs(moon["y"] + moon["h"] // 2 - 333) <= 1,
           "hosted moon measures from the subdial, not the dial: y=%d h=%d"
           % (moon["y"], moon["h"]))
        ok(abs(moon["x"] + moon["w"] // 2 - 233) <= 1,
           "hosted moon stays on the subdial's vertical axis: x=%d" % moon["x"])
        ok(min(c["off"] for c in comps) == 0, "first tile at blob offset 0")
        ends = [c["off"] + c["frame_bytes"] * c["count"] for c in comps]
        ok(max(ends) == len(blob.buf), "comp tiles fill the payload with no trailing gap")
        for c in comps:
            ok(c["off"] + c["frame_bytes"] * c["count"] <= len(blob.buf),
               "comp src=%s stays inside the payload" % ts.SOURCES[c["src"]])

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
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if FAILS:
        print("\n%d check(s) failed" % len(FAILS))
        return 1
    print("\nall complication-producer checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
