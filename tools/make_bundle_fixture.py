#!/usr/bin/env python3
"""Writes the FORMAT 3 fixture every other reader is pinned against.

D33 puts the product's format logic in the browser, which means four readers of one
layout: orbbundle.py (the authority), orb_bundle.c (the device's door), orbnet's
themecontainer.go (the hub's pre-gate) and orbtheme-baker.js (the editor's writer). A
fixture generated here is the one file all four can be pointed at, and the .json next to
it holds the answers the authority gives, so a reader that disagrees fails against a
number rather than against a guess.

The bundle is deliberately not minimal:

  * two sections, so the index walk and the slug sort are exercised;
  * an animated sprite (12 frames, EF_OS_ADV) in the weather section, because D32's
    animated region rides a sprite row and a fixture with only single-frame rows would
    let a writer drop multi-frame planes and still pass;
  * a bank with four plates, because a bank is the other legal multi-frame row and the
    one an OS-advance flag must refuse;
  * real RGB565 planes, so art_size and art_crc are nonzero and the two D29 install
    sums are the file's own numbers rather than constants.

Re-bake:      python3 tools/make_bundle_fixture.py
Then verify:  python3 tools/make_bundle_fixture.py --verify-c   # needs gcc
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orbbundle as ob
import orbtheme as _ot
import theme_schema as ts

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir,
                   "tests", "fixtures", "format3-pair.orbtheme")
SIDE = os.path.splitext(OUT)[0] + ".json"
NAME = "Longplayer Pair"
# Fixed, not a clock: a fixture that changed every time it was baked could not be the one
# file four readers are pinned against, and a diff of the .orbtheme would say nothing about
# whether a reader broke. Both stamps land in the sidecar, so a reader that masks or wraps
# them differently shows up as a size/CRC mismatch rather than as a silent drift.
SERIAL = 20261020
BAKED_MS = 1730000000000

def plane(w, h, seed):
    """One RGB565 plane: a gradient with a seed-dependent offset, so no two planes
    compress or crc alike."""
    px = bytearray()
    for i in range(w * h):
        v = (i * 7 + seed * 61) & 0xFFFF
        px += struct.pack("<H", (v & 0xF800) | ((v >> 5) & 0x3E) | (v >> 11))
    return bytes(px)

def bank(n_planes, w, h, seed):
    """`n_planes` plates laid end to end — the shape a value-per-plate wheel bakes to."""
    return b"".join(plane(w, h, seed + i) for i in range(n_planes))

def elem(n, cls, **kw):
    r = ts.blank_element()
    r.update({"n": n, "class": cls, "n_frames": 1})
    r.update(kw)
    return r

# ---- section: clock — the compiled dialect orb_clock3.c reads -----------------
# Two plates (the base and its overlay), one hour hand, and D34's pointer row: the
# shape the compiled clock's (class, slot) index expects, plus the one class the
# fixture did not carry before — a sub-dial seconds hand turned by the row's own
# drive. It is in the GOLDEN file deliberately: every reader pinned to these bytes
# (orb_bundle.c, orb_theme.c, orbbundle.py, orbtheme-baker.js) then exercises the
# 23-word row and the AUX class on every run, and a writer that dropped the four
# drive columns would change the digest rather than pass quietly.
clock_elems = [
    elem(1, ts.CLASS_NAMES.index("plate"), slot=0, x=0, y=0, w=466, h=466,
         order=0, alpha=255),
    elem(2, ts.CLASS_NAMES.index("plate"), slot=1, x=0, y=0, w=466, h=466,
         order=1, alpha=128, frame0=3),
    elem(3, ts.CLASS_NAMES.index("hand"), slot=0, x=233, y=233, w=40, h=200,
         order=2, pivot_x=20, pivot_y=190, frame0=1),
    elem(4, ts.CLASS_NAMES.index("aux"), slot=0, x=300, y=120, w=28, h=64,
         order=3, alpha=255, pivot_x=14, pivot_y=56, frame0=2,
         kind=ts.MOTION_ROTATE, src=ts.SOURCES.index("second"),
         span=3600, zero=0, vmin=0, vmax=60),
]
# The pointer plate is MASKED (colour then a planar alpha stride), the way every
# hand and every comp tile enters a bank: an AUX row's cost is w*h*3, and a fixture
# that carried it colour-only would pin a shape no bake produces.
# The hand plate is masked for the same arithmetic and the harder reason: D32
# slice 2's catalogue gate in orb_clock3.c keeps only the PLATE row's own key frames
# opaque — every other plane the dial composites must carry a planar alpha stride,
# because a moving sprite without one paints a black box over the dial it crosses.
# A colour-only hand here would pin exactly the shape the device refuses. The hand
# is also ONE frame now: the compiled dial turns one plane per hand slot, so a row
# naming twelve would sit inside the catalogue's letter and outside the dial's use —
# the animated row in this fixture's clock dialect is the pointer, and the multi-frame
# OS-ADV row the writers still pin lives in the weather section.
aux_ptr_art = plane(28, 64, 41) + bytes([0xFF]) * (28 * 64)
hand_art = plane(40, 200, 9) + bytes((i * 13 + 9) & 0xFF for i in range(40 * 200))  # one masked plane
# The glass is a sprite the compositor blends, so it carries the planar alpha
# stride too — the same extent math the shipped overlay sprite is checked against.
overlay_art = plane(466, 466, 52) + bytes((i * 7 + 130) & 0xFF for i in range(466 * 466))
clock_art = plane(466, 466, 3) + hand_art + aux_ptr_art + overlay_art
clock_sprites = [
    {"off": 0, "size": 466 * 466 * 2, "w": 466, "h": 466},
    {"off": 466 * 466 * 2, "size": 40 * 200 * 3, "w": 40, "h": 200},
    {"off": 466 * 466 * 2 + 40 * 200 * 3, "size": 28 * 64 * 3, "w": 28, "h": 64},
    {"off": 466 * 466 * 2 + 40 * 200 * 3 + 28 * 64 * 3, "size": 466 * 466 * 3, "w": 466, "h": 466},
]

# ---- section: weather — an app dialect, animated ------------------------------
# The sprite row is D32's animated region: 12 frames the OS advances at the rate the
# app picks. The layer row is single-frame on purpose; theme_schema refuses an animated
# layer, and a fixture that pretended otherwise would bake into a file the authority
# rejects.
weather_elems = [
    elem(1, ts.CLASS_NAMES.index("sprite"), slot=0, x=32, y=32, w=64, h=64,
         order=0, n_frames=12, flags=ts.EF_OS_ADV, alpha=255),
    elem(2, ts.CLASS_NAMES.index("layer"), slot=0, x=0, y=300, w=200, h=80,
         order=1, alpha=200),
    elem(3, ts.CLASS_NAMES.index("bank"), slot=1, x=120, y=60, w=48, h=48,
         order=2, n_frames=4),
]
wx_sprite_art = b"".join(plane(64, 64, 20 + i) for i in range(12))
wx_layer_art = plane(200, 80, 77)
wx_bank_art = bank(4, 48, 48, 60)
weather_art = wx_sprite_art + wx_layer_art + wx_bank_art
weather_sprites = [
    {"off": 0, "size": len(wx_sprite_art), "w": 64, "h": 64},
    {"off": len(wx_sprite_art), "size": len(wx_layer_art), "w": 200, "h": 80},
    {"off": len(wx_sprite_art) + len(wx_layer_art), "size": len(wx_bank_art),
     "w": 48, "h": 48},
]

SECTIONS = [
    {"slug": "clock", "elements": clock_elems, "art": clock_art,
     "sprites": clock_sprites, "scr_w": 466, "scr_h": 466,
     # D34: orb_clock3 refuses a seated live hand the order table omits — the
     # fixture's own hand at slot 0 must be named, so this clock is a projection
     # -legal dial and not just wire-legal bytes.
     "scalars": {"order0": 0, "order_n": 1}},
    {"slug": "weather", "elements": weather_elems, "art": weather_art,
     "sprites": weather_sprites},
]

def write_vectors(outdir):
    """Dump the fixture's INPUTS, so another implementation can be held against the same
    bytes without re-deriving the model.

        python3 tools/make_bundle_fixture.py --vectors /tmp/bundle3_vectors

    Leaves `model.json` (name, stamps, and every section's elements / scalars / sprites /
    panel) plus one `art_<slug>.bin` of raw encoded pixels per section, and the container
    this model packs to at `container.bin`. tools/watch-composer/_selftest/
    headless-bundle3.test.js feeds those to orbtheme-baker.js's writeBundle3 and requires
    the result to match the committed tests/fixtures/format3-pair.orbtheme byte for byte.

    Pixels travel as raw files rather than as a description of how to draw them because
    Slice 2 is the writer's parity, not the encoders': the art is opaque to write_bundle,
    and a browser producing these planes is Slice 4's job. What must match today is every
    byte the writer itself chooses — offsets, the index, the two CRCs, and the name field.
    """
    os.makedirs(outdir, exist_ok=True)
    secs = []
    for s in SECTIONS:
        art = s.get("art") or b""
        fname = "art_%s.bin" % s["slug"]
        if art:
            with open(os.path.join(outdir, fname), "wb") as f:
                f.write(art)
        secs.append({"slug": s["slug"], "elements": s["elements"],
                     "scalars": s.get("scalars") or {},
                     "sprites": s.get("sprites") or [],
                     "scr_w": s.get("scr_w", 0), "scr_h": s.get("scr_h", 0),
                     "art_file": fname if art else None, "art_len": len(art)})
    tmp = os.path.join(outdir, "container.bin")
    ob.write_bundle(tmp, NAME, SECTIONS, serial=SERIAL, baked_ms=BAKED_MS)
    with open(os.path.join(outdir, "model.json"), "w") as f:
        json.dump({"name": NAME, "serial": SERIAL, "baked_ms": BAKED_MS, "sections": secs},
                  f, indent=1, sort_keys=True)
    print("wrote the FORMAT 3 vector model into %s (%d sections, container %d B)"
          % (outdir, len(secs), os.path.getsize(tmp)))
    return 0

def verify_c():
    """Runs the SHIPPED device reader — main/art/orb_bundle.c, host gcc, the parity
    suite's own harness — over the committed fixture, and holds its answer against the
    sidecar. The point of the file is that four readers agree; this is the leg that
    proves the one on the glass is among them, and it costs one command.

    Needs gcc, so it is a flag rather than a step of --write: a bench without a
    compiler still bakes the fixture, and CI that has one should not be skipping this.
    """
    import tempfile
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import host_orb_bundle_c_test as H
    side = json.load(open(SIDE))
    tmp = tempfile.mkdtemp()
    H.tmpdir = tmp
    binp = H.compile_reader(tmp, "fixture_check", open(H.READER_C).read())
    raw = open(OUT, "rb").read()
    rc, out = H.run_bin(binp, [(9001, 0, 0, [], raw)])
    lines = out.split("\n")
    if rc != 0 or not out.startswith("case9001 ok name"):
        print("C DOOR REFUSED THE FIXTURE: exit %d\n%s" % (rc, out), file=sys.stderr)
        return 1
    # "caseN ok hdr <n_sections> <geom_off> <geom_size> <geom_crc> <art_off>
    #  <art_size> <art_crc> <serial>" — the harness's own field order.
    hdr = [l for l in lines if "ok hdr" in l][0].split()
    fields = ("n_sections", "geom_off", "geom_size", "geom_crc",
              "art_off", "art_size", "art_crc", "serial")
    got = dict(zip(fields, (int(v) for v in hdr[3:11])))
    for k in fields:
        if got[k] != side[k]:
            print("C DOOR DISAGREES ON %s: reader %d, authority %d"
                  % (k, got[k], side[k]), file=sys.stderr)
            return 1
    if not out.startswith("case9001 ok name %s" % side["name"]):
        print("C door read the name as something else: %r" % lines[0], file=sys.stderr)
        return 1
    print("C door reads the fixture as the authority wrote it: %d sections, "
          "%d B region, %d B art" % (got["n_sections"], got["geom_size"],
                                     got["art_size"]))
    return 0

def read_back(path):
    """Print the authority's own read-back of ANY bundle as JSON, so another
    implementation's bytes can be checked through the reference reader rather than
    against a hash of it.

        python3 tools/make_bundle_fixture.py --read-back mybake.orbtheme

    The fields are the sidecar's, so the answer a JS or Go writer gets is the same shape
    the committed fixture is described by: a reader that disagrees fails on a number
    rather than on a guess. A refusal prints the sentence on stderr and exits 1, which is
    the point of the mode — a bundle the authority will not open is a failure, not a
    field to compare.
    """
    hdr, secs = ob.read_bundle(path)
    out = {
        "name": hdr["name"], "format": hdr["fmt"], "abi": hdr["abi"],
        "bytes": os.path.getsize(path), "serial": hdr["serial"],
        "n_sections": hdr["n_sections"], "geom_size": hdr["geom_size"],
        "art_size": hdr["art_size"], "geom_off": hdr["geom_off"],
        "art_off": hdr["art_off"], "geom_crc": hdr["geom_crc"],
        "art_crc": hdr["art_crc"],
        "slugs": [s["blob"]["slug"] for s in secs],
        "n_elem": {s["blob"]["slug"]: len(s["blob"]["elements"]) for s in secs},
        "abi_range": {s["rec"]["slug"] if "slug" in s["rec"] else s["blob"]["slug"]:
                      [s["rec"]["abi_min"], s["rec"]["abi_max"]] for s in secs},
        "scr": {s["blob"]["slug"]: [s["rec"]["scr_w"], s["rec"]["scr_h"]] for s in secs},
        "art_len": {s["blob"]["slug"]: len(s["art"]) for s in secs},
        "frames": {"%s.%s" % (s["blob"]["slug"], ts.CLASS_NAMES[int(e["class"])]):
                   int(e["n_frames"]) for s in secs for e in s["blob"]["elements"]
                   if int(e["n_frames"]) > 1},
    }
    json.dump(out, sys.stdout, indent=1, sort_keys=True)
    sys.stdout.write("\n")
    return 0

def main():
    if "--verify-c" in sys.argv:
        return verify_c()
    if "--read-back" in sys.argv:
        return read_back(sys.argv[sys.argv.index("--read-back") + 1])
    if "--vectors" in sys.argv:
        return write_vectors(sys.argv[sys.argv.index("--vectors") + 1])
    # Written to a sibling and renamed in: the hub's tests read this file straight off
    # disk, and a reader that catches a half-written bundle reports a layout failure the
    # format never had.
    tmp = OUT + ".tmp"
    ob.write_bundle(tmp, NAME, SECTIONS, serial=SERIAL, baked_ms=BAKED_MS)

    hdr, secs = ob.read_bundle(tmp)  # the authority reads back what it wrote
    raw = open(tmp, "rb").read()

    side = {
        "name": NAME,
        # Per-app theming files a bundle by (slug, name): the sidecar carries the
        # card stem for EVERY section's app, so the Go and JS mirrors are pinned
        # against the Python one on each slug the fixture ships with.
        "card_stems": {sec["slug"]: ob.card_stem(NAME, [sec]) for sec in SECTIONS},
        "theme_ids": {sec["slug"]: _ot.theme_id(sec["slug"], NAME) for sec in SECTIONS},
        "format": 3,
        "abi": ob.GEOM_ABI3,
        "bytes": len(raw),
        "serial": SERIAL,
        "baked_ms": BAKED_MS,
        "baked_ms_word": BAKED_MS & 0xFFFFFFFF,   # the header carries the u32 wrap
        "n_sections": hdr["n_sections"],
        "geom_size": hdr["geom_size"],
        "art_size": hdr["art_size"],
        "geom_off": hdr["geom_off"],
        "art_off": hdr["art_off"],
        "geom_crc": hdr["geom_crc"],
        "art_crc": hdr["art_crc"],
        "name_in_header": hdr["name"],
        "slugs": [s["blob"]["slug"] for s in secs],
        "n_elem": {s["blob"]["slug"]: len(s["blob"]["elements"]) for s in secs},
        # The animated rows, by (slug, class): what a writer must reproduce plane for
        # plane, and the reason this fixture carries them at all.
        "frames": {(s["blob"]["slug"], ts.CLASS_NAMES[int(e["class"])]): int(e["n_frames"])
                   for s in secs for e in s["blob"]["elements"]
                   if int(e["n_frames"]) > 1},
        "note": "generated by tools/make_bundle_fixture.py; the .json holds the "
                "authority's own read-back, which is what the other readers match",
    }
    side["frames"] = {"%s.%s" % k: v for k, v in side["frames"].items()}
    side_tmp = SIDE + ".tmp"
    with open(side_tmp, "w") as f:
        json.dump(side, f, indent=2, sort_keys=True)
        f.write("\n")
    # Both renames after both files are whole, so a test pair never reads a sidecar that
    # describes a different bundle than the bytes beside it.
    os.replace(tmp, OUT)
    os.replace(side_tmp, SIDE)

    print("wrote %s (%d B) and %s" % (OUT, len(raw), SIDE))

if __name__ == "__main__":
    sys.exit(main() or 0)

