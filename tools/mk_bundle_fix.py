#!/usr/bin/env python3
"""mk_bundle_fix.py - FORMAT 3 bench fixtures for the P6.4 streaming door.

A BENCH FIXTURE GENERATOR, not the P6.5 re-bake. The art bank of --from output is
the source FORMAT 2 container's art bytes verbatim, so the device streams real
av-kit bytes into slot-B and the §20.5(1) copy rate / §20.5(3) power-cut are paid
against av-kit's size, not a token blob. The element tables are SYNTHETIC (one
plate, one hand); the clock-geometry-to-app mapping is P6.5's ruled work and
nothing here claims it. read_bundle() proves the file round-trips; it does NOT
prove it draws, because nothing on this build draws a bundle yet (D30 behind
P6.5). FORMAT 2 is retired (§25.5(3)): the device admits one door, and that door
refuses a FORMAT 2 file at the format gate, so this tool's bundle output is the only
thing that stores. The FORMAT 2 file --from reads is the retirement's source material,
not a dialect the device still takes.

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

def pad_bank(art, want):
    """Real av-kit bytes, one zero plane on the end, and the bank length that makes.

    §20.5(2) asks what a CAP-SIZED refill costs, and the number has to come from a file
    the writer and the reader both accept. The reader's rule is that the sprite
    catalogue accounts for the bank byte-for-byte, with every plane's size equal to its
    own extent, so reaching the cap means declaring a plane for the remainder — and a
    plane's size is w*h*3, which does not divide an arbitrary remainder. So the flag
    asks for a CEILING and the tool reports the largest legal bank at or under it,
    within one plane of the cap: 4,128,7xx of 4,128,768. Stating an exact size the
    padding cannot tile would make the tool refuse the measurement it exists to enable.

    Returns (art_bytes, plane_w, plane_h, filler_size).
    """
    room = want - len(art)
    if room <= 0:
        raise SystemExit("refused: --pad-to %d is smaller than the art (%d B)"
                         % (want, len(art)))
    side = 1
    while (side + 1) * (side + 1) * 3 <= room:
        side += 1
    h = room // (side * 3)
    fill = side * h * 3
    return art + bytes(fill), side, h, fill


def sprite_rows(w, h, art_len, filler):
    """The clock section's catalogue: the plate plane over the real bytes, then the
    padding plane the cap asked for. The synthetic table's hand points at frame0 1, so
    the filler row is referenced and drawn — at 8x8 it costs the paint path nothing."""
    rows = [{"off": 0, "size": min(w * h * 2, art_len - filler), "w": w, "h": h}]
    if filler:
        rows.append({"off": art_len - filler, "size": filler, "w": 466,
                     "h": filler // (466 * 3)})
    return rows


def sections(art, w, h, filler=0, only_clock=False):
    """clock carries the whole bank; bench2 is a tiny second section so the file
    exercises the multi-section rebase (a single bank never moves).

    The padding build passes only_clock: the writer's budget gate sums the banks, not
    the span, so a cap-sized clock plus any second section is over budget by
    construction — and refusing that is the gate doing its job, not a fixture problem."""
    tiny = art[:min(192, len(art))]
    clock = {
        "slug": "clock", "scr_w": w, "scr_h": h,
        "elements": [elem(0, PLATE, w=w, h=h),
                     elem(1, HAND, slot=0, order=1, frame0=1, radius=90)],
        "sprites": sprite_rows(w, h, len(art), filler), "art": art,
    }
    if only_clock:
        return [clock]
    return [
        {"slug": "bench2", "scr_w": w, "scr_h": h,
         "elements": [elem(0, SPRITE, w=8, h=8)],
         "sprites": [{"off": 0, "size": len(tiny) or 192, "w": 8, "h": 8}], "art": tiny},
        clock,
    ]

def from_format2(src, out, name=None, pad_to=0):
    h2, _geom, art = ot.read_container(src)
    w = h2.get("scr_w") or 464; h = h2.get("scr_h") or 464
    nm = name or (h2.get("name") or "bench") + " (bench)"
    filler = 0
    if pad_to:
        art, _pw, _ph, filler = pad_bank(art, pad_to)
    size = ob.write_bundle(out, nm, sections(art, w, h, filler, bool(pad_to)))
    hdr, secs = ob.read_bundle(out)
    tot = sum(len(s["art"]) for s in secs)
    print("wrote %s: %d B, %d sections, art %d B (source %d B), format %d"
          % (out, size, len(secs), tot, len(art) - filler, hdr["fmt"]))
    if pad_to:
        room = ob.SLOT_ART_CAP
        print("  at-cap bank %d B of a %d B cap: %.3f%% — %d B under, one plane's remainder"
              % (tot, room, 100.0 * tot / room, room - tot))
    assert hdr["fmt"] == ob.FORMAT3 and tot >= len(art) - filler, \
        "fixture is not a faithful FORMAT 3 copy"

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
    # A FORMAT 2 file: now the door's refusal arm. FORMAT 2 is retired (§25.5(3)), so a
    # real container must be REFUSED at the format gate, not stored. The real av-kit
    # container when it is on disk (the bench fixture): a hand-stuffed zero-geometry blob
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
    ap.add_argument("--pad-to", type=int, default=0,
                    help="pad the clock bank to this byte count with one filler plane, "
                         "for the §20.5(2) at-cap refill (0x3F0000 is the slot cap)")
    a = ap.parse_args()
    if not a.src and not a.probe: ap.error("give --from or --probe")
    if a.src:
        o = a.out or os.path.join(a.dir, os.path.basename(a.src).replace(".orbtheme", ".b3.orbtheme"))
        os.makedirs(os.path.dirname(o), exist_ok=True)
        from_format2(a.src, o, a.name, a.pad_to)
    if a.probe: probes(a.dir)
