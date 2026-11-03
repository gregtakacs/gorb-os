#!/usr/bin/env python3
"""migrate_v6.py — rewrite an old clock theme's geometry as D42's v6 rows.

D42 ruled out legacy: the firmware, the hub and the baker read v6 clock sections and
refuse every older stamp (docs/history/element-model-plan.md §8.1). Themes are rebuilt in the
composer going forward. This tool exists for the bundles already committed to the repo
— test fixtures, measured bench themes, the hub's default — so each does not need a
browser re-bake to keep its pixels. It is a host-side one-way converter, never on a
device or hub path, and nothing reads its input format but it.

The pixel bank is copied byte for byte: only the element table and the scalars change.
The translation is orbtheme-baker.js's bakeClock3 (v6Drive, the hand defaults, the
shadow twin's placement), applied to old rows instead of composer inputs:

  - each HAND slot 0..2 becomes an AUX row with the slot's own mapping (hour12/12,
    minute/60, second/60) or the row's own D41 drive; slots 3 and 4 become still rows;
  - an `inherit` cadence becomes the motion it drew: sweep for hour and minute (jump on
    a railway minute), the section's second_sweep/tick_rate for the seconds;
  - each shadow sprite (when shadow_on) becomes the twin in the slot after its hand;
  - the carriage (hand slot 5) becomes the animated row, turning with the seconds from
    its `degrees`, at 1000/tourbillon_fps ms a frame;
  - each pointer keeps its drive, its motion turned into poses;
  - a matte-folded hand is dropped (its pixels are in the plate already);
  - every retired scalar is written as 0.

    python3 tools/migrate_v6.py IN.orbtheme OUT.orbtheme
"""
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import orbbundle as ob          # noqa: E402
import orbtheme as _ot          # noqa: E402
import theme_schema as ts       # noqa: E402

# The old pointer motions (D34), in their wire order.
ROTATE, RETRO, INDEXED, SLIDE_X, SLIDE_Y = range(5)


def read_old(path):
    """The header, the index and each section's blob, read at whatever stamp it carries.
    The catalogue row is six words from stamp 4 on, four before; the element stride is
    the blob's own (its length says it)."""
    raw = open(path, "rb").read()
    hdr = struct.unpack("<%dI" % _ot.HDR_WORDS, raw[:_ot.HDR_WORDS * 4])
    if hdr[0] != _ot.MAGIC or hdr[1] != ob.FORMAT3:
        raise ValueError("%s is not a FORMAT 3 theme" % path)
    name = _ot._wordstr(hdr, _ot.W_NAME, _ot.NAME_MAX)
    region = raw[hdr[4]:hdr[4] + hdr[5]]
    n, recs = ob.unpack_index(region)
    out = []
    for r in recs:
        blob = region[r["geom_off"]:r["geom_off"] + r["geom_size"]]
        abi = r["abi_max"]
        sw = 6 if abi >= 4 else 4
        ne, ns, nsc, nl = struct.unpack_from("<4I", blob, 0)
        fixed = (4 + nsc + ns * sw) * 4 + nl + (-nl % 4)
        ew = (len(blob) - fixed) // (ne * 4) if ne else ts.ELEMENT_WORDS_V3
        scal = struct.unpack_from("<%dI" % nsc, blob, 16)
        off = (4 + nsc) * 4
        elems = ts.unpack_elements(list(struct.unpack_from("<%dI" % (ne * ew), blob, off)),
                                   ne, stride=ew) if ne else []
        if abi < 4:
            for e in elems:
                if e["parent"] == 0:
                    e["parent"] = ts.PARENT_NONE
        off += ne * ew * 4
        sprs = []
        for i in range(ns):
            o, sz, w, h = struct.unpack_from("<4I", blob, off + i * sw * 4)
            sp = {"off": o, "size": sz, "w": w, "h": h}
            if sw == 6:
                wx, wy = struct.unpack_from("<2I", blob, off + i * sw * 4 + 16)
                sp["diffx"] = (wx & 0xFFFF) - (0x10000 if wx & 0x8000 else 0)
                sp["diffy"] = (wy & 0xFFFF) - (0x10000 if wy & 0x8000 else 0)
            sprs.append(sp)
        off += ns * sw * 4
        slug = blob[off:off + nl].decode()
        scalars = {nm: _ot._sext(scal[i], _ot.THEME_FIELDS[i][1])
                   for i, (nm, _t) in enumerate(_ot.THEME_FIELDS)}
        art = raw[r["art_off"]:r["art_off"] + r["art_size"]]
        if zlib.crc32(art) & 0xFFFFFFFF != r["art_crc"]:
            raise ValueError("section %r bank checksum mismatch" % slug)
        out.append({"slug": slug, "abi": abi, "elements": elems, "sprites": sprs,
                    "scalars": scalars, "art": art, "scr_w": r["scr_w"],
                    "scr_h": r["scr_h"]})
    return name, hdr, out


def v6_drive(motion, span, zero, vmax, x, y):
    """bakeClock3's v6Drive: an old motion as the two poses."""
    d = {"span": span, "zero": zero, "vmin": 0, "vmax": vmax, "dx": 0, "dy": 0,
         "x": x, "y": y,
         "end": ts.END_FLYBACK if motion == RETRO
         else ts.END_WRAP if abs(span) == ts.TURN_10 else ts.END_HOLD}
    if motion in (SLIDE_X, SLIDE_Y):
        if motion == SLIDE_X:
            d["dx"], d["x"] = span, x + zero
        else:
            d["dy"], d["y"] = span, y + zero
        d["span"] = d["zero"] = 0
        d["end"] = ts.END_FLYBACK
    return d


def migrate_clock(sec):
    sc = sec["scalars"]
    els = sec["elements"]
    if sec["abi"] == ts.CLOCK_ABI:
        return sec                                    # already v6
    sweep, rate = int(sc.get("second_sweep", 0)), int(sc.get("tick_rate", 0))
    railway = int(sc.get("second_railway", 0)) and sweep
    matte = int(sc.get("matte_mask", 0))
    shadow_on = int(sc.get("shadow_on", 0))
    sec_cad = ((ts.CAD_SWEEP, min(ts.SWEEP_FPS_MAX, rate) if rate >= 2 else 0) if sweep
               else (ts.CAD_STEP, max(1, rate)))
    hands = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_HAND}
    shadows = {int(e["slot"]): e for e in els if int(e["class"]) == ts.CLASS_SPRITE}
    out, slot = [], [0]

    def take(rec):
        rec = dict(rec)
        rec["class"] = ts.CLASS_AUX
        rec["slot"] = slot[0]
        rec["degrees"] = 0
        slot[0] += 1
        if slot[0] > ts.CLK_ROW_MAX:
            raise ValueError("the theme needs more than %d moving rows" % ts.CLK_ROW_MAX)
        out.append(rec)
        return rec

    SLOT_SRC = {0: ("hour12", 12), 1: ("minute", 60), 2: ("second", 60)}
    sec_row = None
    for k in sorted(s for s in hands if s < 5):
        h = hands[k]
        if matte & (1 << k):
            continue
        r = dict(h, kind=ts.SHAPE_ARM, span=0, zero=0, vmin=0, vmax=0, cadence=0,
                 cad_arg=0, ease=int(h.get("ease", 0)), src=0)
        if int(h["src"]):
            d = v6_drive(int(h["kind"]), int(h["span"]), int(h["zero"]), int(h["vmax"]),
                         int(h["x"]), int(h["y"]))
            src = int(h["src"])
        elif k in SLOT_SRC:
            d = v6_drive(ROTATE, ts.TURN_10, 0, SLOT_SRC[k][1], int(h["x"]), int(h["y"]))
            src = ts.SOURCES.index(SLOT_SRC[k][0])
        else:
            d = None
        if d:
            cad, arg = int(h.get("cadence", 0)), int(h.get("cad_arg", 0))
            if not cad:
                cad, arg = (sec_cad if ts.SOURCES[src] == "second"
                            else (ts.CAD_JUMP, 0) if (k == 1 and railway)
                            else (ts.CAD_SWEEP, 0))
            if int(h["src"]) and int(h["kind"]) == INDEXED and cad == ts.CAD_SWEEP:
                cad = ts.CAD_JUMP
            r.update(src=src, span=d["span"], zero=d["zero"], vmin=d["vmin"],
                     vmax=d["vmax"], dx=d["dx"], dy=d["dy"], end=d["end"], x=d["x"],
                     y=d["y"], cadence=cad, cad_arg=arg)
        else:
            r["ease"] = 0
        hr = take(r)
        if ts.SOURCES[hr["src"]] == "second":
            sec_row = hr
        s = shadows.get(k)
        if s is not None and shadow_on:
            hcx, hcy = int(h["x"]) + int(h["pivot_x"]), int(h["y"]) + int(h["pivot_y"])
            tpx, tpy = hcx - int(s["x"]), hcy - int(s["y"])
            scx, scy = int(s["x"]) + int(s["pivot_x"]), int(s["y"]) + int(s["pivot_y"])
            take(dict(s, kind=ts.SHAPE_ARM, x=scx - tpx + hr["x"] - int(h["x"]),
                      y=scy - tpy + hr["y"] - int(h["y"]), pivot_x=tpx, pivot_y=tpy,
                      order=hr["order"], flags=int(s["flags"]) | ts.EF_AUX_SHADOW,
                      src=0, span=0, zero=0, vmin=0, vmax=0, cadence=0, cad_arg=0,
                      ease=0))
    if 5 in hands:
        c = hands[5]
        fps = max(1, int(sc.get("tourbillon_fps", 0)) or 15)
        cad = (sec_row["cadence"], sec_row["cad_arg"]) if sec_row else sec_cad
        take(dict(c, kind=ts.SHAPE_ARM, src=ts.SOURCES.index("second"), span=ts.TURN_10,
                  zero=(int(c["degrees"]) // 10) % ts.TURN_10, vmin=0, vmax=60,
                  end=ts.END_WRAP, cadence=cad[0], cad_arg=cad[1], ease=0,
                  delay=max(1, round(1000 / fps))))
    for a in sorted((e for e in els if int(e["class"]) == ts.CLASS_AUX),
                    key=lambda e: int(e["slot"])):
        if int(a["flags"]) & ts.EF_AUX_SHADOW:
            prev = out[-1] if out else None
            take(dict(a, kind=ts.SHAPE_ARM, src=0, span=0, zero=0, vmin=0, vmax=0,
                      cadence=0, cad_arg=0, ease=0,
                      x=int(a["x"]) + ((prev["x"] - prev["_ox"]) if prev and "_ox" in prev else 0),
                      y=int(a["y"]) + ((prev["y"] - prev["_oy"]) if prev and "_oy" in prev else 0)))
            continue
        d = v6_drive(int(a["kind"]), int(a["span"]), int(a["zero"]), int(a["vmax"]),
                     int(a["x"]), int(a["y"]))
        cad, arg = int(a.get("cadence", 0)), int(a.get("cad_arg", 0))
        if not cad:
            cad, arg = (ts.CAD_JUMP, 0) if int(a["kind"]) == INDEXED else (ts.CAD_SWEEP, 0)
        r = take(dict(a, kind=ts.SHAPE_ARM, span=d["span"], zero=d["zero"], vmin=d["vmin"],
                      vmax=d["vmax"], dx=d["dx"], dy=d["dy"], end=d["end"], x=d["x"],
                      y=d["y"], cadence=cad, cad_arg=arg))
        r["_ox"], r["_oy"] = int(a["x"]), int(a["y"])
    for r in out:
        r.pop("_ox", None)
        r.pop("_oy", None)
    rest = [e for e in els if int(e["class"]) not in (ts.CLASS_HAND, ts.CLASS_SPRITE,
                                                      ts.CLASS_AUX)]
    new_sc = dict(sc)
    for nm in ts.CLOCK_RETIRED_SCALARS:
        new_sc[nm] = 0
    return dict(sec, abi=ts.CLOCK_ABI, elements=rest + out, scalars=new_sc)


def migrate(src, dst):
    name, hdr, secs = read_old(src)
    sections = []
    for s in secs:
        if s["slug"] == "clock":
            s = migrate_clock(s)
        abi = s["abi"] if s["slug"] == "clock" else ob.GEOM_ABI3
        sections.append({"slug": s["slug"], "elements": s["elements"],
                         "scalars": s["scalars"], "sprites": s["sprites"], "art": s["art"],
                         "scr_w": s["scr_w"], "scr_h": s["scr_h"],
                         "abi_min": abi, "abi_max": abi})
    ob.write_bundle(dst, name, sections, serial=hdr[_ot.W_SERIAL],
                    baked_ms=hdr[_ot.W_BAKED_MS], one_section=len(sections) == 1)
    ob.read_bundle(dst, theme=len(sections) == 1)     # the authority reads it back whole
    return name


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    print("migrated %r: %s -> %s" % (migrate(argv[1], argv[2]), argv[1], argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
