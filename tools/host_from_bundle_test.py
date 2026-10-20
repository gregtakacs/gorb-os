#!/usr/bin/env python3
"""host_from_bundle_test.py -- the P6.5 §25.5(1) sentinel for the reverse path.

`bake_clock.py --from-bundle` claims a FORMAT 3 bundle can rebuild the compiled
`clock_assets.{bin,h}` pair with no kit in the loop. This suite holds that claim
with no PIL, no board, no card and no kit zip: it forges a clock bundle from the
shipped pair's OWN geometry using `orbbundle`/`orbtheme` (both PIL-free), runs the
reverse path, and diffs the result against `assets/clock_assets.h` from
`#pragma once` down. The provenance comment is the pair's one sanctioned
difference -- the same rule `.clinerules` uses to verify a kit re-bake.

Then the battery: a lying bundle per refusal the reverse path owes, each required
to draw the sentence that names its own defect. A suite that only checks the happy
path proves the tool rebuilds the one theme it was written against; this one proves
it refuses every table it cannot state (§25.5(1)).

Run: python3 tools/host_from_bundle_test.py
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import orbtheme as ot              # noqa: E402
import orbbundle as ob             # noqa: E402
import theme_schema as ts          # noqa: E402

failures, checks, refusals = [], [0], [0]


def check(ok, msg):
    checks[0] += 1
    if not ok:
        failures.append(msg)
        print("  FAIL " + msg[:2000])


def ck(cond, msg):
    check(bool(cond), msg)


# ---------------------------------------------------------------------------
# The geometry under test is read out of the SHIPPED .h. A suite written against a
# hand-typed copy of the pair tests the copy.
# ---------------------------------------------------------------------------
HDR = open(os.path.join(ROOT, "tests", "fixtures", "avkit-pair.h")).read()
ART = open(os.path.join(ROOT, "tests", "fixtures", "avkit-pair.bin"), "rb").read()
SPRITE_RE = re.compile(
    r"\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
    r"\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),"
    r"\s*(\d+),\s*(\d+)\s*\}")
FIELDS = ("w", "h", "offx", "offy", "pivotx", "pivoty", "centerx", "centery",
          "blend", "show", "color_off", "diffx", "diffy", "diffw", "diffh")


def _sprites(marker):
    """The five-row blocks and the one-line overlay, both straight off the .h."""
    seg = HDR[HDR.index(marker):]
    seg = seg[:seg.index("\n")] if marker == "/* overlay */" else seg[:seg.index("\n  },")]
    return [dict(zip(FIELDS, [int(x) for x in g])) for g in SPRITE_RE.findall(seg)]


hands, shadows = _sprites("/* hand */"), _sprites("/* shadow */")
overlay = _sprites("/* overlay */")[0]
SCR_W = int(re.search(r"#define CLK_SCR_W (\d+)", HDR).group(1))
SCR_H = int(re.search(r"#define CLK_SCR_H (\d+)", HDR).group(1))
GIF_FRAMES = int(re.search(r"#define CLK_GIF_FRAMES (\d+)", HDR).group(1))


def theme_scalars():
    """The 26 clock scalars, read back out of the shipped pair's own initialiser."""
    body = HDR[HDR.index("static const clk_theme_t clk_theme = {"):]

    def g(pat, d=0):
        m = re.search(pat, body)
        return int(m.group(1), 16) if "0x" in pat else int(m.group(1)) if m else d
    order = [int(x) for x in re.search(r"/\* order \*/ \{ ([\d, ]+)\}", body)
             .group(1).split(",")]
    return {
        "bg": int(re.search(r"/\* bg \*/ 0x([0-9A-Fa-f]{6})", body).group(1), 16),
        "plate_follow": g(r"/\* plate_follow \*/ (\d+)"),
        "shadow_on": g(r"/\* shadow_on \*/ (\d+)"),
        "shadow_dx": g(r"/\* shadow dx,dy \*/ (-?\d+),"),
        "shadow_dy": g(r"/\* shadow dx,dy \*/ -?\d+, (-?\d+)"),
        "second_sweep": g(r"/\* sweep,railway,textOverHands,tickRate \*/ (\d+)"),
        "second_railway": g(r"/\* sweep,railway,textOverHands,tickRate \*/ \d+, (\d+)"),
        "text_over_hands": g(r"/\* sweep,railway,textOverHands,tickRate \*/ \d+, \d+, (\d+)"),
        "tick_rate": g(r"/\* sweep,railway,textOverHands,tickRate \*/ \d+, \d+, \d+, (\d+)"),
        "bg_frames": g(r"/\* bg_frames \*/ (\d+)"),
        "bg_fps": g(r"/\* fps,loop,every \*/ (-?\d+)"),
        "bg_loop": g(r"/\* fps,loop,every \*/ -?\d+, (\d+)"),
        "bg_every": g(r"/\* fps,loop,every \*/ -?\d+, \d+, (\d+)"),
        "plate_off": g(r"/\* plate_off \*/ (\d+)"),
        "order0": order[0:1] and order[0] or 0,
        "order1": len(order) > 1 and order[1] or 0,
        "order2": len(order) > 2 and order[2] or 0,
        "order3": len(order) > 3 and order[3] or 0,
        "order4": len(order) > 4 and order[4] or 0,
        "order_n": g(r"/\* order_n \*/ (\d+)"),
        "matte_mask": g(r"/\* matte_mask \*/ (\d+)"),
        "bg_mode": g(r"/\* bg_mode,bg_key_off,gif_frames \*/ (\d+)"),
        "bg_key_off": g(r"/\* bg_mode,bg_key_off,gif_frames \*/ \d+, (\d+)"),
        "gif_frames": g(r"/\* bg_mode,bg_key_off,gif_frames \*/ \d+, \d+, (\d+)"),
        "tourbillon_n": g(r"/\* tourbillon_n,tourbillon_fps \*/ (\d+)"),
        "tourbillon_fps": g(r"/\* tourbillon_n,tourbillon_fps \*/ \d+, (\d+)"),
    }


SC = theme_scalars()


def _plane(off, size, w, h):
    return {"off": off, "size": size, "w": w, "h": h}

def _plane(off, size, w, h):
    return {"off": off, "size": size, "w": w, "h": h}


# ---------------------------------------------------------------------------
# The fixture: the shipped pair re-expressed as a FORMAT 3 section, through the
# mapping rebake_bundle.py rules (draw point in x,y; pivot = centre - draw point).
# The art IS the committed .bin, so the .bin half of the proof is byte-exact rather
# than within a re-bake's antialiasing fringe.
# ---------------------------------------------------------------------------
def build_bundle(path, scalars=None, hd=None, sh=None, ov=None, sprites=None,
                 art=None, slug="clock", name="av-kit", plate_extent=None,
                 extra=None, plane_edit=None):
    sc = dict(SC)
    sc.update(scalars or {})
    planes, elems = [], []

    def add(cls, slot, order, e, plane):
        planes.append(plane)
        # `class` is a Python keyword, so the column arrives through the dict and
        # not as a keyword argument. frame0 is patched after the byte sort.
        elems.append(dict(ts.blank_element(), n=len(elems), slot=slot, order=order,
                          x=e[0], y=e[1], w=e[2], h=e[3], pivot_x=e[4], pivot_y=e[5],
                          frame0=len(planes) - 1, n_frames=1, _plane=plane,
                          flags=e[6] if len(e) > 6 else 0, **{"class": cls}))

    pw, ph = plate_extent or (SCR_W, SCR_H)
    add(ts.CLASS_PLATE, 0, 0, (0, 0, pw, ph, 0, 0), _plane(0, pw * ph * 2, pw, ph))
    rank = [sc["order%d" % i] for i in range(5)]
    for i, s in enumerate(hd or []):
        if s["show"] and s["w"] and s["h"]:
            add(ts.CLASS_HAND, i, 2 * rank[i] + 1,
                (s["offx"], s["offy"], s["w"], s["h"], s["centerx"] - s["offx"],
                 s["centery"] - s["offy"]),
                _plane(s["color_off"], s["w"] * s["h"] * 3, s["w"], s["h"]))
    for i, s in enumerate(sh or []):
        if s["show"] and s["w"] and s["h"]:
            add(ts.CLASS_SPRITE, i, 2 * rank[i],
                (s["offx"], s["offy"], s["w"], s["h"], s["centerx"] - s["offx"],
                 s["centery"] - s["offy"]),
                _plane(s["color_off"], s["w"] * s["h"] * 3, s["w"], s["h"]))
    if ov and ov["show"] and ov["w"] and ov["h"]:
        add(ts.CLASS_PLATE, 1, 63,
            (ov["offx"], ov["offy"], ov["w"], ov["h"], ov["pivotx"], ov["pivoty"]),
            _plane(ov["color_off"], ov["w"] * ov["h"] * 3, ov["w"], ov["h"]))
    for meta, plane in (extra or []):
        planes.append(plane)
        elems.append(dict(ts.blank_element(), n=len(elems), **meta))
    # The catalogue is BYTE order and frame0 is the row's seat in it -- the rule
    # rebake_bundle.py applies. The shipped .h's colour offsets interleave the overlay
    # and the hand/shadow pairs, so an append-order table is a table this tool would
    # rightly refuse as non-contiguous.
    planes.sort(key=lambda p: p["off"])
    trim = 0
    if plane_edit:
        trim = plane_edit(planes) or 0
    seat = {id(p): i for i, p in enumerate(planes)}
    for e in elems:
        if "_plane" in e:
            e["frame0"] = seat[id(e.pop("_plane"))]
    elems.sort(key=lambda e: e["frame0"])
    for n, e in enumerate(elems):
        e["n"] = n
    if trim:
        art = (ART if art is None else art)[:len(ART if art is None else art) - trim]
    return ob.write_bundle(path, name, [
        {"slug": slug, "elements": elems, "scalars": sc, "scr_w": SCR_W,
         "scr_h": SCR_H, "sprites": sprites if sprites is not None else planes,
         "art": ART if art is None else art}], baked_ms=1)


TMP = tempfile.mkdtemp(prefix="p65fb_")
OUT_H = os.path.join(TMP, "out.h")
OUT_B = os.path.join(TMP, "out.bin")
SHIPPED_BODY = HDR[HDR.index("#pragma once"):]


def run(bundle, *extra):
    return subprocess.run(
        [sys.executable, os.environ.get("FROM_BUNDLE_TOOL",
                                        os.path.join(HERE, "bake_clock.py")),
         "--from-bundle", bundle, "--bin", OUT_B, "--header", OUT_H] + list(extra),
        capture_output=True, text=True, cwd=ROOT)


def body(path):
    t = open(path).read()
    return t[t.index("#pragma once"):]


def rows(marker, text):
    """One sprite block of a .h body, as eight-number tuples. The block markers also
    appear in the typedef, so the search starts at the initialiser."""
    text = text[text.index("static const clk_theme_t"):]
    seg = text[text.index(marker):]
    seg = seg[:seg.index("\n")] if marker == "/* overlay */" else seg[:seg.index("\n  },")]
    return [tuple(int(x) for x in t)
            for t in re.findall(r"\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
                                r"\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),", seg)]


def show_flags(text):
    """The blend and show columns of the hand block's five rows, in slot order. The
    scan starts at `/* hand */` so the gif delay table's pairs cannot join in."""
    seg = text[text.index("static const clk_theme_t"):]
    seg = seg[seg.index("/* hand */"):]
    seg = seg[:seg.index("/* shadow */")]
    return [(int(a), int(b)) for a, b in re.findall(
        r"\{\s*\d+,\s*\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*-?\d+,\s*"
        r"(\d+),\s*(\d+),", seg)]


def forged(label, **kw):
    p = os.path.join(TMP, label + ".orbtheme")
    build_bundle(p, **kw)
    return p


def blob_forged(label, edit):
    """Build the faithful bundle, decode it, edit its element table, and re-encode it
    through the writer's own blob/index/header packers. Two forgeries (a tourbillon
    row, a text row) are shapes `write_bundle` itself refuses, so they have to reach
    the reader as bytes -- which is exactly what a forged card holds. The art region
    is copied from the original file WHOLE, padding and all, so the only lie in the
    container is the table."""
    src = os.path.join(TMP, label + ".src.orbtheme")
    build_bundle(src, hd=hands, sh=shadows, ov=overlay)
    hdr, secs = ob.read_bundle(src)
    sec = secs[0]
    raw = open(src, "rb").read()
    art_region = raw[secs[0]["rec"]["art_off"]:]
    edit(sec["blob"]["elements"])
    el = sec["blob"]["elements"]
    el.sort(key=lambda e: e["frame0"])
    for n, e in enumerate(el):
        e["n"] = n
    blob = ob.pack_blob(sec["blob"]["slug"], el,
                        sec["blob"]["scalars"], sec["blob"]["sprites"])
    rec = dict(sec["rec"])
    rec["n_elem"] = len(el)
    rec["geom_size"] = len(blob)
    # The slug lives at the blob's tail, so the index's pointer follows the new bytes
    # rather than the old ones -- or the forgery trips the slug check instead of the
    # pairing rule it is meant to test.
    sl = rec["slug_len"]
    rec["slug_off"] = rec["geom_off"] + len(blob) - (sl + (-sl % 4))
    rec["geom_crc"] = zlib.crc32(blob) & 0xFFFFFFFF
    head0 = ob.pack_index([rec]) + blob                      # sizing pass
    rec["art_off"] = 128 + len(head0)                        # where these bytes land
    region = ob.pack_index([rec]) + blob
    head = ob.pack_header3(hdr["name"], 1, len(region),
                           zlib.crc32(region) & 0xFFFFFFFF,
                           rec["art_off"], rec["art_size"], rec["art_crc"], 0, 1)
    out = os.path.join(TMP, label + ".orbtheme")
    open(out, "wb").write(head + region + art_region)
    return out


def restamped_fanout(label):
    """Turn the overlay's row into a hand at slot 5: a tourbillon the table names and
    the .h has no carried columns for."""
    def edit(el):
        for e in el:
            if e["class"] == ts.CLASS_PLATE and e["slot"] == 1:
                e["class"], e["slot"] = ts.CLASS_HAND, 5
    return blob_forged(label, edit)


def restamped_orphan(label):
    """Drop the minute hand's row and keep its shadow's: the contradiction, forged on
    a byte-exact container, so the plane walk stays whole and only the pairing lies."""
    def edit(el):
        keep = [e for e in el if not (e["class"] == ts.CLASS_HAND and e["slot"] == 1)]
        el[:] = keep
    return blob_forged(label, edit)


def restamped_hidden(label):
    """Flip the second hand's row to EF_HIDDEN. The pair must print show=0 with the
    row's geometry intact -- an off switch is a declaration, not an absence."""
    def edit(el):
        for e in el:
            if e["class"] == ts.CLASS_HAND and e["slot"] == 2:
                e["flags"] |= ts.EF_HIDDEN
    return blob_forged(label, edit)


def restamped_textrow(label):
    """Turn the plate's own row into a text row: a class with no field in the pair."""
    def edit(el):
        el[0]["class"] = ts.CLASS_TEXT
    return blob_forged(label, edit)

def must_refuse(label, bundle, *needles):
    """Run the reverse path on a lying bundle: nonzero, saying `refused:`, and
    naming the defect. An accepted forgery is the failure this suite exists for."""
    p = run(bundle)
    out = p.stdout + p.stderr
    refusals[0] += 1
    if p.returncode == 0:
        failures.append("%s: a forged bundle was ACCEPTED" % label)
        return
    ck("Traceback" not in out,
       "%s: died by traceback instead of a sentence: %s" % (label, out[-300:]))
    ck("refused:" in out or "Error" in out,
       "%s: nonzero with no refusal sentence: %s" % (label, out[-300:]))
    for n in needles:
        ck(n in out, "%s: refused, but not in the words naming it (wanted %r): %s"
           % (label, n, out[-400:]))


# ---------------------------------------------------------------------------
# 1. The faithful bundle: the section the forward tool itself would have written.
# ---------------------------------------------------------------------------
print("== the faithful bundle ==")
GOOD = forged("good", hd=hands, sh=shadows, ov=overlay)
p = run(GOOD)
ck(p.returncode == 0, "the faithful bundle was refused: %s"
   % (p.stdout + p.stderr)[-700:])
if p.returncode == 0:
    rb = body(OUT_H)
    check(rb == SHIPPED_BODY,
          "the rebuilt .h is NOT byte-identical to the shipped pair (%d vs %d bytes)"
          % (len(rb), len(SHIPPED_BODY)))
    for a, b in [(x, y) for x, y in zip(rb.splitlines(), SHIPPED_BODY.splitlines())
                 if x != y][:4]:
        failures.append("   rebuilt: %r\n   shipped : %r" % (a, b))
    ck(open(OUT_B, "rb").read() == ART, "the rebuilt .bin is not the shipped art")
    prov = open(OUT_H).read()[:open(OUT_H).read().index("#pragma once")]
    ck("good.orbtheme" in prov, "the provenance does not name the bundle it read")
    ck("AUTO-GENERATED" in prov and "Re-bake" in prov,
       "the provenance lost a line a shipped .h always has")
    ck(re.search(r"CLK_BLOB_BYTES %du" % len(ART), rb) is not None,
       "CLK_BLOB_BYTES is not the bank's own size")
    dl = re.search(r"clk_gif_delays\[(\d+)\] = \{ ([^}]*)\}", rb)
    ck(dl and dl.group(1) == str(GIF_FRAMES), "the delay table length is not gif_frames")
    ms = max(1, int(round(1000.0 / max(1, SC["tick_rate"]))))
    ck(dl and set(x.strip() for x in dl.group(2).split(",")) == {str(ms)},
       "the regenerated delays are not one uniform OS-beat value (%d) per R6" % ms)
    rl = re.search(r"clk_bg_rects\[(\d+)\] = \{\n(.*?)\};", rb, re.S)
    ck(rl and rl.group(1) == "1",
       "a FORMAT 3 rebuild emitted more than the placeholder single rect")
    ck(rl and set(rl.group(2).replace(" ", "").replace("\n", "")) <= set("0,{}"),
       "a FORMAT 3 rebuild emitted a nonzero delta rect")
    fr = re.search(r"clk_bg_rect_first\[(\d+)\]", rb)
    ck(fr and fr.group(1) == str(GIF_FRAMES + 1),
       "clk_bg_rect_first is not gif_frames+1 entries")
    for lbl in ("hour", "minute", "second", "static1", "static2"):
        ck(("/* %s */" % lbl) in rb, "slot %s lost its label" % lbl)
    check(rb.count("/* overlay */") == 2,
          "the overlay line lost its duplicated label (the shipped format has two)")
    bl = re.findall(r"\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
                    r"\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),", rb)
    want = [(h["w"], h["h"], h["offx"], h["offy"], h["pivotx"], h["pivoty"],
             h["centerx"], h["centery"]) for h in hands]
    check(rows("/* hand */", rb) == want,
          "the hand geometry drifted: %r != %r" % (rows("/* hand */", rb), want))
    want_sh = [(s["w"], s["h"], s["offx"], s["offy"], s["pivotx"], s["pivoty"],
                s["centerx"], s["centery"]) for s in shadows]
    check(rows("/* shadow */", rb) == want_sh,
          "the shadow CROP pivots drifted (the light-offset recovery is wrong): "
          "%r != %r" % (rows("/* shadow */", rb), want_sh))
    ov_row = rows("/* overlay */", rb)
    check(ov_row == [(overlay["w"], overlay["h"], overlay["offx"], overlay["offy"],
                      overlay["pivotx"], overlay["pivoty"], overlay["centerx"],
                      overlay["centery"])],
          "the overlay geometry drifted: %r" % (ov_row,))
    want = [(h["w"], h["h"], h["offx"], h["offy"], h["pivotx"], h["pivoty"],
             h["centerx"], h["centery"]) for h in hands]

# ---------------------------------------------------------------------------
# 2. The forgeries. Each is one field a lying bundle could carry, and each is
# required to stop the run with the sentence naming it (§25.5(1)).
# ---------------------------------------------------------------------------
print("== the refusals ==")

# A tourbillon named by the scalars with nothing in the table to fan out: the .h
# would print sixteen blank records behind a bank no column carries.
must_refuse("tourbillon_n lies",
            forged("tourb", hd=hands, sh=shadows, ov=overlay,
                   scalars={"tourbillon_n": 4}), "tourbillon")

# A tourbillon in the table: a hand row past the five slots the dialect owns. The
# writer refuses a plane past its own bank, so this forgery is applied to the blob
# bytes after the fact and the checksums re-stamped -- a forged bundle, not a
# composed one, which is exactly the input the reader has to survive.
must_refuse("hand slot fan-out", restamped_fanout("fanout"), "tourbillon")

# A plate that is not the whole panel: the compiled clock has nowhere to put it.
must_refuse("sub-panel plate",
            forged("smallplate", hd=hands, sh=shadows, ov=overlay,
                   plate_extent=(100, 100),
                   sprites=[_plane(0, 100 * 100 * 2, 100, 100)]), "plate")

# A plate naming a frame run: the delta rects are the shape §25.3 refuses.
must_refuse("plate names a frame run",
            forged("framerun", hd=hands, sh=shadows, ov=overlay,
                   scalars={"bg_frames": 3}), "plate frames")

# A bank the catalogue cannot account for: the art runs 540 B past the last row.
# The writer refuses a plane past its own bank, so the forgery grows the bank
# instead -- the same lie, seen from the end a legitimate writer will let you write.
must_refuse("unaccounted bank",
            forged("short", hd=hands, sh=shadows, ov=overlay, art=ART + b"\0" * 540),
            "accounts for")

# A catalogue row whose extent contradicts the element binding it. The row is
# self-consistent (size still equals its own w*h*3), it is the LAST plane in byte
# order, and the bank shrinks by exactly the difference, so neither the contig rule
# nor the sum rule can fire first: the only lie left is the extent, and the refusal
# has to name it.
must_refuse("plane extent lies",
            forged("extent", hd=hands, sh=shadows, ov=overlay,
                   plane_edit=lambda p: (
                       p[-1].update(w=p[-1]["w"] - 2,
                                    size=p[-1]["size"] - 2 * p[-1]["h"] * 3),
                       2 * p[-1]["h"] * 3)[1]),
            "different layers")

# A catalogue that skips bytes: a hole no element can draw from. The hole moves the
# first sprite's payload without resizing it, so the walk trips over the gap itself.
must_refuse("non-contiguous catalogue",
            forged("gap", hd=hands, sh=shadows, ov=overlay, art=ART + b"\0" * 4,
                   plane_edit=lambda p: p[1].update(off=p[1]["off"] + 4)),
            "contiguous")

# A shadow with no hand: the light offset is only recoverable from the hand under
# it, so the pair would print an offset the bundle cannot state. One hand row is
# dropped and its shadow row kept, which is the contradiction, forged honestly.
must_refuse("shadow with no hand", restamped_orphan("orphan"), "no hand")

# EF_HIDDEN is a theme's own off switch, not an absence: the row must come back with
# show=0 AND its geometry, or the flag is silently becoming "no hand at all" and the
# next slot's blank centre would replace a real one.
hid = restamped_hidden("hidden")
p = run(hid)
ck(p.returncode == 0, "an EF_HIDDEN row was refused: %s" % (p.stdout + p.stderr)[-400:])
if p.returncode == 0:
    hrow = rows("/* hand */", body(OUT_H))
    ck(hrow[2][0] == hands[2]["w"] and hrow[2][2] == hands[2]["offx"],
       "a hidden hand lost its geometry instead of keeping it with show=0")
    fl = show_flags(body(OUT_H))
    ck(fl[2][1] == 0, "a hidden hand printed show=%d, not 0" % fl[2][1])
    ck(fl[:5] == [(0, 1), (0, 1), (0, 0), (0, 0), (0, 0)],
       "the hidden row moved some other slot's show flag: %r" % (fl[:5],))
    ck(body(OUT_H) != SHIPPED_BODY,
       "an EF_HIDDEN row rebuilt byte-identical to the shipped pair: the flag was ignored")

# A row of a class the compiled clock has no field for. Forged into the bytes: a
# text row over the plate's own plane is a shape the writer would refuse outright.
must_refuse("text row", restamped_textrow("textrow"), "no field")

# A bundle whose one section is not the clock.
must_refuse("no clock section",
            forged("noslug", hd=hands, sh=shadows, ov=overlay, slug="weather"),
            "no 'clock' section")

# A FORMAT 2 container: no element table to rebuild from.
v2 = os.path.join(TMP, "v2.orbtheme")
ot.write_container(v2, "av-kit",
                   ot.pack_geom(SC, hands, shadows, overlay, [],
                                [0, 0, 0, 0, 0], [0] * (GIF_FRAMES + 1),
                                [67] * GIF_FRAMES, [0]),
                   ART, SCR_W, SCR_H, baked_ms=1)
must_refuse("format 2 input", v2, "format 2")

# A damaged bank: the reader's own checksum stops it before the reverse path runs.
tampered = bytearray(open(GOOD, "rb").read())
tampered[len(tampered) - 100] ^= 0x20
torn = os.path.join(TMP, "torn.orbtheme")
open(torn, "wb").write(bytes(tampered))
must_refuse("damaged bank", torn, "checksum")


# ---------------------------------------------------------------------------
# 3. The command contract: --from-bundle refuses a source-side flag rather than
# ignoring it, and never writes a container it was not asked for.
# ---------------------------------------------------------------------------
print("== the command contract ==")


def cli(label, argv, *needles):
    p = subprocess.run([sys.executable,
                        os.environ.get("FROM_BUNDLE_TOOL",
                                       os.path.join(HERE, "bake_clock.py"))] + argv,
                       capture_output=True, text=True, cwd=ROOT)
    refusals[0] += 1
    out = p.stdout + p.stderr
    if p.returncode == 0:
        failures.append("%s: exited 0, wanted a refusal" % label)
        return
    ck("Traceback" not in out, "%s: traceback: %s" % (label, out[-300:]))
    for n in needles:
        ck(n in out, "%s: wanted %r in %s" % (label, n, out[-350:]))


cli("--force-sweep is a source decision",
    ["--from-bundle", GOOD, "--bin", OUT_B, "--header", OUT_H, "--force-sweep"],
    "--from-bundle takes no --force-sweep")
cli("--tick-rate is a source decision",
    ["--from-bundle", GOOD, "--bin", OUT_B, "--header", OUT_H, "--tick-rate", "8"],
    "--from-bundle takes no --tick-rate")
cli("--orbtheme would be a downgrade",
    ["--from-bundle", GOOD, "--bin", OUT_B, "--header", OUT_H, "--orbtheme",
     os.path.join(TMP, "downgrade.orbtheme")], "--orbtheme is the")
cli("a kit dir and a bundle are two sources",
    ["assets", "--from-bundle", GOOD, "--bin", OUT_B, "--header", OUT_H], "named both")
cli("no source at all", [], "give a theme directory")
cli("--bundle-section names a real section",
    ["--from-bundle", GOOD, "--bin", OUT_B, "--header", OUT_H, "--bundle-section",
     "weather"], "no 'weather' section")
check(not os.path.exists(os.path.join(TMP, "downgrade.orbtheme")),
      "--from-bundle wrote a FORMAT 2 container it was refused permission for")

# The section flag still works: naming the one section present is the same run.
p = run(GOOD, "--bundle-section", "clock")
ck(p.returncode == 0, "--bundle-section clock (the default) was refused")

# Idempotence: rebuilding twice from the same bundle writes the same bytes.
if os.path.exists(OUT_H):
    run(GOOD)
    first = open(OUT_H).read()
    run(GOOD)
    check(open(OUT_H).read() == first,
          "two rebuilds of one bundle wrote different .h files")
else:
    check(False, "no rebuilt .h exists to compare: the faithful bundle never wrote one")

# ---------------------------------------------------------------------------
print("== summary ==")
print("host_from_bundle_test: %d checks, %d refusal vectors, %d failure(s)"
      % (checks[0], refusals[0], len(failures)))
for f in failures:
    print("  FAIL " + f)
sys.exit(1 if failures else 0)


