#!/usr/bin/env python3
"""mk_bundle_fix.py - FORMAT 3 bench fixtures for the P6.4 streaming door.

A BENCH FIXTURE GENERATOR, not the P6.5 re-bake. The art bank of --from output is
the source FORMAT 2 container's art bytes verbatim, so the device streams real
av-kit bytes into slot-B and the §20.5(1) copy rate / §20.5(3) power-cut are paid
against av-kit's size, not a token blob. The element tables are SYNTHETIC (one
plate, one hand); the clock-geometry-to-app mapping is P6.5's ruled work and
nothing here claims it. read_bundle() proves the file round-trips; it does NOT
prove it draws, because nothing on this build draws a bundle yet (D30 behind
P6.5). Output is refused by the FORMAT 2 install door and stored by the FORMAT 3
one - the same split the device enforces.

  --from FILE --out F   FORMAT 2 in, FORMAT 3 bench bundle out (art byte-identical)
  --probe --dir D       the §24.7 sentence set for the door's four behaviours
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbtheme as ot, orbbundle as ob, theme_schema as ts
PLATE, HAND, SPRITE = ts.CLASS_PLATE, ts.CLASS_HAND, ts.CLASS_SPRITE

def elem(n, cls, **kw):
    r = ts.blank_element(); r["n"] = n; r["class"] = cls; r["n_frames"] = 1
    r.update(kw); return r

def sections(art, w, h):
    """clock carries the whole bank; bench2 is a tiny second section so the file
    exercises the multi-section rebase (a single bank never moves)."""
    tiny = art[:min(192, len(art))]
    return [
        {"slug": "bench2", "scr_w": w, "scr_h": h,
         "elements": [elem(0, SPRITE, w=8, h=8)],
         "sprites": [{"off": 0, "size": len(tiny) or 192, "w": 8, "h": 8}], "art": tiny},
        {"slug": "clock", "scr_w": w, "scr_h": h,
         "elements": [elem(0, PLATE, w=min(464, w), h=min(464, h)),
                      elem(1, HAND, slot=0, order=1, frame0=1, radius=90)],
         "sprites": [{"off": 0, "size": min(w * h * 2, len(art)), "w": min(464, w), "h": min(464, h)}],
         "art": art},
    ]

def from_format2(src, out, name=None):
    h2, _geom, art = ot.read_container(src)
    w = h2.get("scr_w") or 464; h = h2.get("scr_h") or 464
    nm = name or (h2.get("name") or "bench") + " (bench)"
    size = ob.write_bundle(out, nm, sections(art, w, h))
    hdr, secs = ob.read_bundle(out)
    tot = sum(len(s["art"]) for s in secs)
    print("wrote %s: %d B, %d sections, art %d B (source %d B), format %d"
          % (out, size, len(secs), tot, len(art), hdr["fmt"]))
    assert hdr["fmt"] == ob.FORMAT3 and tot >= len(art), "fixture is not a faithful FORMAT 3 copy"

def probes(d):
    os.makedirs(d, exist_ok=True); out = {}
    good = os.path.join(d, "b3_valid.orbtheme")
    ob.write_bundle(good, "b3 valid", sections(b"\x11" * 0x2000, 464, 464))
    out["valid"] = good
    raw = bytearray(open(good, "rb").read()); h3, _ = ob.read_bundle(good)
    raw[h3["geom_off"] + 40] ^= 0xFF       # region CRC torn, past the 128-byte header
    out["region"] = os.path.join(d, "b3_region.orbtheme"); open(out["region"], "wb").write(bytes(raw))
    hv = list(struct.unpack("<32I", raw[:128])); hv[ot.W_NAME + 1] ^= 0x20
    out["header"] = os.path.join(d, "b3_header.orbtheme")
    open(out["header"], "wb").write(struct.pack("<32I", *hv) + bytes(raw[128:]))
    # A FORMAT 2 file: still a theme, must store as one. The real av-kit container
    # when it is on disk (the bench fixture): a hand-stuffed zero-geometry blob
    # trips the panel/plausibility gates for reasons that have nothing to do with
    # the dialect split this arm is proving, and a refused probe proves nothing.
    src23 = "/tmp/m5_avkit.orbtheme"
    out["format"] = os.path.join(d, "b3_format2.orbtheme")
    if os.path.exists(src23):
        open(out["format"], "wb").write(open(src23, "rb").read())
    else:
        ot.write_container(out["format"], "av-kit", b"\x00" * 260, b"\xA5" * 4096, 466, 466)
    out["blob"] = os.path.join(d, "b3_blob.orbtheme")
    open(out["blob"], "wb").write(bytes((i * 9 + 1) & 0xFF for i in range(8192)))
    for k, v in out.items():
        print("probe %-7s -> %s (%d B)" % (k, v, os.path.getsize(v)))

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src"); ap.add_argument("--name"); ap.add_argument("--out")
    ap.add_argument("--dir", default="/tmp/b3"); ap.add_argument("--probe", action="store_true")
    a = ap.parse_args()
    if not a.src and not a.probe: ap.error("give --from or --probe")
    if a.src:
        o = a.out or os.path.join(a.dir, os.path.basename(a.src).replace(".orbtheme", ".b3.orbtheme"))
        os.makedirs(os.path.dirname(o), exist_ok=True); from_format2(a.src, o, a.name)
    if a.probe: probes(a.dir)
