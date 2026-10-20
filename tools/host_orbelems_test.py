#!/usr/bin/env python3
"""host_orbelems_test.py — the element-table seam, driven on the host.

`main/art/orb_elems.c` answers the one question D32 leaves open between a parsed
element table and a draw: "give me the pixels for (class, slot) at frame N". Nothing
in the tree answered it. `orb_bundle.c` unpacks a row and knows nothing about frames;
`orb_clock3.c` projects a whole table into the clock's single legacy `clk_theme_t`
and refuses every row it cannot fit into that struct's five hand slots — which is why
a theme with an animated carriage or a complication in it is refused today rather than
drawn. This file is the proof that the seam exists, is app-neutral, and refuses in
words.

Three lines are checked, all of them on host gcc against the shipped reader's own
bytes, no board and no partition:

  1. the GOLDEN answers — one animated clock section, every frame of every element
     resolved to the plane the catalogue says it is, and every echoed field (order,
     alpha, flags, extent, pivot, degrees) traced back to the row it came from;
  2. the REFUSALS — a frame past an element's own window, a hidden row, a slot the
     theme never carried, and four forgeries that make the table and the bank disagree.
     Each must answer with its own enum AND the sentence naming its own defect; D26's
     direction is say-cannot-draw-yet, never draw wrong. The mutants replay this exact
     list, because a guard relaxed in the seat() chain often returns the same enum with
     a sentence naming the NEXT guard's defect — and a mislabelled refusal is as useless
     to support as a silent accept;
  3. the MIRRORS — the class names and flag bits this C file copies from
     `theme_schema.py` and `orb_assets.h` are re-read from both and compared, so the
     vocabulary cannot drift the way orbtheme's hand-written FORMAT-1 name ranges
     once did.

R6 is what makes the frame half of this worth a suite of its own: an animation carries
frames and NO rate, so one tick is one frame and `orb_elems_next()`'s wrap-or-clamp
answer is the whole of the timing model. A mutant that flips EF_LOOP's clamp to a wrap
changes no byte of any bundle and changes what the dial shows, which is precisely the
class of bug a field-by-field check catches and a checksum never does.

    python3 tools/host_orbelems_test.py               # golden, refusals, mirrors
    python3 tools/host_orbelems_test.py --mutations    # and the C mutants
"""
import os
import re
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import orbbundle as ob              # noqa: E402  (the writer, the oracle)
import theme_schema as ts           # noqa: E402  (the field tables, the vocabulary)

Elems_C = os.path.join(ROOT, "main", "art", "orb_elems.c")
Bundle_C = os.path.join(ROOT, "main", "art", "orb_bundle.c")

failures, checks = [], [0]


def check(ok, msg):
    checks[0] += 1
    if not ok:
        failures.append(msg)


def eq(got, want, what):
    check(got == want, "%s: got %r, want %r" % (what, got, want))


def contains(hay, needle, what):
    check(needle in hay, "%s: %r not in %r" % (what, needle, hay))



# ---------------------------------------------------------------------------
# the fixture: one clock section, built by the shipped writer
#
# Two plates (the panel background and the crystal), two hands, one shadow pair,
# one four-frame tourbillon carriage that the OS is told to advance, and one
# twelve-frame month bank. The sprite catalogue is the byte layout this suite
# reads back, so the expected answers below are arithmetic and not belief.
# ---------------------------------------------------------------------------

def plane(w, h, masked):
    """One plane's byte count, by the rule orb_asset_slot_bytes() prices."""
    return w * h * (3 if masked else 2)


# (name, class, slot, w, h, masked, frames, flags, order, alpha, px, py, deg)
GOLDEN = (
    ("bg",        "plate",  0,  466, 466, False, 1,  0,                0,  0,   0,   0,      0),
    ("glass",     "plate",  1,   64,  64, True,  1,  ts.EF_ALPHA,      63, 128,   0,   0,      0),
    ("hour",      "hand",   0,   40,  12, True,  1,  0,                 1,  0,  20,   6,      0),
    ("minute",    "hand",   1,   40,  12, True,  1,  ts.EF_ALPHA,       3, 200,  20,   6,      0),
    ("sh_hour",   "sprite", 0,   40,  12, True,  1,  0,                 0,  0,   0,   0,      0),
    ("carriage",  "hand",   5,   24,  24, True,  4,  ts.EF_OS_ADV | ts.EF_LOOP,
                                                                        5,  0,  12,  12,  1800),
    ("comp2",     "bank",   2,   16,  16, True, 12,  ts.EF_LOOP,        7,  0,   0,   0,      0),
)


def build(elements=None, sprites=None, art=None, slug="clock"):
    """One section's bundle, through the writer that the device's reader mirrors."""
    return {"slug": slug,
            "elements": elements if elements is not None else golden_elements(),
            "scalars": {},
            "sprites": sprites if sprites is not None else golden_sprites(),
            "art": art if art is not None else golden_art()}


def golden_planes():
    """(w, h, masked, frames) per element, in bank order: a plane per frame."""
    out = []
    for (_n, _c, _s, w, h, masked, frames, _fl, _o, _a, _px, _py, _d) in GOLDEN:
        for f in range(frames):
            out.append((w, h, masked))
    return out


def golden_sprites():
    cat, at = [], 0
    for (w, h, masked) in golden_planes():
        n = plane(w, h, masked)
        cat.append({"off": at, "size": n, "w": w, "h": h})
        at += n
    return cat


def golden_art():
    total = sum(s["size"] for s in golden_sprites())
    return bytes((i * 7 + 3) & 0xFF for i in range(total))


def frame0_of(name):
    """The catalogue row a named element's first frame sits at, by bank order."""
    at, want = 0, [n for (n, _c, _s, _w, _h, _m, _f, _fl, _o, _a, _p, _q, _d) in GOLDEN
                   ].index(name)
    for i, (name_i, _c, _s, _w, _h, _m, frames, *_rest) in enumerate(GOLDEN):
        if i == want:
            return at
        at += frames
    raise AssertionError("no golden element %r" % name)


def golden_elements():
    recs, n = [], 0
    for (name, cls, slot, w, h, masked, frames, flags, order, alpha,
         px, py, deg) in GOLDEN:
        e = ts.blank_element()
        e.update({"n": n, "class": ts.CLASS_NAMES.index(cls), "slot": slot,
                  "flags": flags, "order": order, "alpha": alpha,
                  "frame0": frame0_of(name), "n_frames": frames,
                  "x": 10 + n, "y": 20 + n, "w": w, "h": h,
                  "pivot_x": px, "pivot_y": py, "degrees": deg})
        recs.append(e)
        n += 1
    return recs


# ---------------------------------------------------------------------------
# the driver: the shipped orb_elems.c, host gcc, the writer's own bytes
#
# argv is `bundle [cls slot frame]...`, so one process answers a batch and Python
# owns every expectation. The line it prints is the whole answer record: a field a
# mutant changes is a field that stops matching, with no assertion hidden in the C.
# ---------------------------------------------------------------------------

SHIM_CRC = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{ return (uint32_t)crc32((uLong)crc, buf, (uInt)len); }
"""

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_crc.h"
#include "orb_bundle.h"
#include "orb_elems.h"

/* One query per three argv words, answered on one line. `why` is the LAST field and
 * is printed verbatim, because the sentences are half of what is under test: an
 * answer enum that says the right thing in a sentence that names the wrong slot would
 * be a support ticket nobody can read. */
int main(int argc, char **argv)
{
    if (argc < 2 || ((argc - 2) % 3) != 0) return 2;
    FILE *fp = fopen(argv[1], "rb");
    if (!fp) return 2;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return 2; }
    long n = ftell(fp);
    rewind(fp);
    uint8_t *buf = malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return 2;
    }
    fclose(fp);

    orb_bundle3_hdr_t h;
    orb_bundle_t b;
    const char *why = orb_bundle3_hdr_parse((const uint32_t *)(const void *)buf, &h);
    if (!why) why = orb_bundle_parse(&h, buf, (uint32_t)n, &b);
    if (why) { printf("bundle-refuse %s\n", why); return 3; }
    const orb_bundle_sec_t *sec = orb_bundle_find(&b, "clock");
    if (!sec) { printf("bundle-refuse no clock section\n"); return 3; }

    for (int i = 2; i + 2 < argc; i += 3) {
        unsigned cls  = (unsigned)strtoul(argv[i], NULL, 10);
        unsigned slot = (unsigned)strtoul(argv[i + 1], NULL, 10);
        unsigned fr   = (unsigned)strtoul(argv[i + 2], NULL, 10);
        orb_elem_t e;
        char sent[256];
        sent[0] = 0;
        orb_elems_ans_t a = orb_elems_at(sec, (uint16_t)cls, (uint16_t)slot, fr,
                                         &e, sent, sizeof sent);
        if (a != ORB_ELEMS_OK) {
            printf("q %u %u %u ans %s why %s\n", cls, slot, fr,
                   orb_elems_why(a), sent);
            continue;
        }
        printf("q %u %u %u ans ok order %u alpha %u flags %u frames %u frame0 %u "
               "x %d y %d w %u h %u pivot %d %d deg %u rad %u scale %u off %u size %u "
               "plane %ux%u masked %d os_adv %d loop %d next %u last %u\n",
               cls, slot, fr, e.order, e.alpha, e.flags, e.n_frames, e.frame0,
               e.x, e.y, e.w, e.h, e.pivot_x, e.pivot_y, e.degrees, e.radius,
               e.scale16, e.off, e.size, e.plane_w, e.plane_h, (int)e.masked,
               (int)e.os_adv, (int)e.loop,
               orb_elems_next(&e, fr), orb_elems_last_frame(&e));
    }
    return 0;
}
"""


def compile_driver(tmp, src_text=None, tag="golden"):
    """Host-gcc the shipped orb_elems.c plus the reader, with the crc shim."""
    shim = os.path.join(tmp, "shim")
    os.makedirs(shim, exist_ok=True)
    with open(os.path.join(shim, "esp_rom_crc.h"), "w") as f:
        f.write(SHIM_CRC)
    harness = os.path.join(tmp, "harness_%s.c" % tag)
    with open(harness, "w") as f:
        f.write(HARNESS_C)
    e_src = os.path.join(tmp, "orb_elems_%s.c" % tag)
    with open(e_src, "w") as f:
        f.write(src_text if src_text is not None else open(Elems_C).read())
    binp = os.path.join(tmp, "elems_%s" % tag)
    cc = subprocess.run(
        ["gcc", "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wformat=2",
         "-I", shim, "-I", os.path.join(ROOT, "main", "art"),
         "-o", binp, e_src, Bundle_C, harness, "-lz"],
        capture_output=True, text=True)
    if cc.returncode != 0:
        raise RuntimeError("gcc failed for %s:\n%s" % (tag, cc.stderr))
    return binp


def write_bundle(tmp, sections, tag):
    p = os.path.join(tmp, "%s.orbtheme" % tag)
    ob.write_bundle(p, "orbelems fixture", sections, serial=1)
    return p


def ask(binp, path, queries):
    """Run the driver over a batch of (cls, slot, frame); returns the lines."""
    argv = [binp, path]
    for (c, s, f) in queries:
        argv += [str(c), str(s), str(f)]
    r = subprocess.run(argv, capture_output=True, text=True)
    return r.stdout.rstrip("\n").split("\n") if r.stdout.strip() else []


def parse(line):
    """`q C S F ans ok k v ...` / `q C S F ans <why-word> why <sentence>` -> dict."""
    w = line.split()
    assert w and w[0] == "q", line
    d = {"cls": int(w[1]), "slot": int(w[2]), "frame": int(w[3]), "ans": w[5]}
    if d["ans"] != "ok":
        d["why"] = " ".join(w[7:])
        return d
    i, kv = 6, {}
    while i < len(w):
        if w[i] == "pivot":
            kv["pivot_x"], kv["pivot_y"] = int(w[i + 1]), int(w[i + 2]); i += 3
        elif w[i] == "plane":
            a = w[i + 1].split("x")
            kv["plane_w"], kv["plane_h"] = int(a[0]), int(a[1]); i += 2
        else:
            kv[w[i]] = int(w[i + 1]); i += 2
    d["fields"] = kv
    return d


# ---------------------------------------------------------------------------
# the vectors
# ---------------------------------------------------------------------------

# Every golden element at frame 0, plus every frame of the two animated rows. The
# carriage's four frames and the month bank's twelve are the point of the file: a
# projection that can only ever answer frame 0 is what orb_clock3.c does today.
def golden_queries():
    q = []
    for (name, cls, slot, _w, _h, _m, frames, *_r) in GOLDEN:
        for f in range(frames):
            q.append((ts.CLASS_NAMES.index(cls), slot, f))
    return q


def expect(name, frame):
    """The answer record the fixture promises for (name, frame)."""
    for (n, cls, slot, w, h, masked, frames, flags, order, alpha,
         px, py, deg) in GOLDEN:
        if n != name:
            continue
        rows = golden_sprites()
        sp = rows[frame0_of(name) + frame]
        return {
            "order": order,
            "alpha": (alpha if (flags & ts.EF_ALPHA) else 255),
            "flags": flags, "frames": frames, "frame0": frame0_of(name),
            "x": 10 + GOLDEN.index((n, cls, slot, w, h, masked, frames, flags,
                                    order, alpha, px, py, deg)),
            "y": 20 + GOLDEN.index((n, cls, slot, w, h, masked, frames, flags,
                                    order, alpha, px, py, deg)),
            "w": w, "h": h, "pivot_x": px, "pivot_y": py, "deg": deg,
            "rad": 0, "scale": 0,
            "off": sp["off"], "size": sp["size"],
            "plane_w": sp["w"], "plane_h": sp["h"],
            "masked": 1 if masked else 0,
            "os_adv": 1 if (flags & ts.EF_OS_ADV) else 0,
            "loop": 1 if (flags & ts.EF_LOOP) else 0,
            "next": ((frame + 1) if (frame + 1) < frames else
                     (0 if (flags & ts.EF_LOOP) else frames - 1)),
            "last": frames - 1,
        }
    raise AssertionError("no golden element %r" % name)


def golden_checks(binp, path):
    lines = ask(binp, path, golden_queries())
    eq(len(lines), len(golden_queries()), "golden answers for every frame of every row")
    got = {}
    for ln in lines:
        d = parse(ln)
        eq(d["ans"], "ok", "frame %u of (class %u, slot %u) resolves" %
           (d["frame"], d["cls"], d["slot"]))
        got[(d["cls"], d["slot"], d["frame"])] = d["fields"]
    for (name, cls, slot, *_r) in GOLDEN:
        c = ts.CLASS_NAMES.index(cls)
        nframes = GOLDEN[[g[0] for g in GOLDEN].index(name)][6]
        for f in range(nframes):
            w, g = expect(name, f), got.get((c, slot, f))
            if g is None:
                check(False, "no answer for %s frame %d" % (name, f))
                continue
            for k in sorted(w):
                eq(g.get(k), w[k], "%s frame %d .%s" % (name, f, k))


# ---------------------------------------------------------------------------
# refusals: each forgery must answer with its own word, naming its own slot
# ---------------------------------------------------------------------------

W_HDR_CRC_W = 28                       # orbtheme.W_HDR_CRC: hdr_crc's word index in the header


def derived_stride(buf, geom_off, blob_off):
    """The element row width the blob itself carries (§23.5): its length less
    the fixed head, scalars, sprite catalogue and padded slug, over its own row
    count. orb_bundle.c's blob_check derives the same number; the forger uses it
    so a row is landed on the way the reader will, not the way the writer meant."""
    import struct as st
    base = geom_off + blob_off
    n_elem, n_spr, n_scal, n_slug = st.unpack_from("<4I", buf, base)
    fixed = (4 + n_scal) * 4 + n_spr * 16 + n_slug + (-n_slug % 4)
    geom_size = st.unpack_from("<I", buf, geom_off + 8 + 16)[0]      # IDX_GEOM_SIZE
    if not n_elem:
        return ts.ELEMENT_WORDS
    return (geom_size - fixed) // (n_elem * 4)


def elem_word(buf, row, word):
    """The byte offset of one element row's word inside a written bundle.

    The blob head is the writer's four count words plus the 26 carried scalars
    (orbbundle.pack_blob), and the element rows follow it at the writer's own
    stride — theme_schema.ELEMENT_WORDS, 19 now that D32's ruling A appended
    kind and src. The stride is read from the golden walk the writer took, the
    same one `orb_bundle.c`'s blob_check() derives, which is the point: a
    forger that found the row by a different route would be testing its own
    arithmetic.
    """
    import struct as st
    from orbtheme import W_GEOM_OFF
    geom_off = st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    blob_off = st.unpack_from("<I", buf, geom_off + 8 + 12)[0]     # IDX_GEOM_OFF
    stride = derived_stride(buf, geom_off, blob_off)
    return geom_off + blob_off + (4 + 26) * 4 + row * stride * 4 + word * 4


def spr_word(buf, row, word):
    """The byte offset of one sprite-catalogue row's word: the catalogue follows the
    element rows, four words each, exactly as orb_bundle.c's blob_check() walks it."""
    import struct as st
    from orbtheme import W_GEOM_OFF
    geom_off = st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    blob_off = st.unpack_from("<I", buf, geom_off + 8 + 12)[0]
    head = geom_off + blob_off + (4 + 26) * 4
    n_elem = st.unpack_from("<I", buf, geom_off + blob_off)[0]
    return head + n_elem * derived_stride(buf, geom_off, blob_off) * 4 + row * 4 * 4 + word * 4


def regeom(buf):
    """Restamp every checksum a byte edit broke, innermost first — the discipline
    host_clock3_test.py records: restamp one and the file dies at the wrong gate."""
    import struct as st
    from orbtheme import W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC, W_HDR_CRC, W_SCR_W
    geom_off = st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    geom_size = st.unpack_from("<I", buf, 4 * W_GEOM_SIZE)[0]
    nsec = st.unpack_from("<I", buf, 4 * W_SCR_W)[0]      # FORMAT 3: n_sections rides
    #                                                         scr_w's word (orbbundle)
    for i in range(nsec):
        rec = geom_off + 8 + i * 40
        b_off = st.unpack_from("<I", buf, rec + 12)[0]
        b_size = st.unpack_from("<I", buf, rec + 16)[0]
        st.pack_into("<I", buf, rec + 20,
                     zlib.crc32(bytes(buf[geom_off + b_off:
                                        geom_off + b_off + b_size])) & 0xFFFFFFFF)
    crc = zlib.crc32(bytes(buf[geom_off:geom_off + geom_size])) & 0xFFFFFFFF
    st.pack_into("<I", buf, 4 * W_GEOM_CRC, crc)
    st.pack_into("<I", buf, 4 * W_HDR_CRC, 0)
    return zlib.crc32(bytes(buf[:128])) & 0xFFFFFFFF


def forge_file(tmp, tag, where, row, word, value):
    """The golden file with ONE word rewritten and the header/geom crcs restamped, so
    `orb_bundle` still accepts it and only the element resolver is left to notice. A
    forgery that orb_bundle itself rejects would test orb_bundle, not this seam.
    """
    import struct as st
    p = write_bundle(tmp, [build()], tag)
    buf = bytearray(open(p, "rb").read())
    at = elem_word(buf, row, word) if where == "elem" else spr_word(buf, row, word)
    buf[at:at + 4] = st.pack("<I", value)
    st.pack_into("<I", buf, 4 * W_HDR_CRC_W, regeom(buf))
    fp = os.path.join(tmp, "%s.forged.orbtheme" % tag)
    open(fp, "wb").write(bytes(buf))
    return fp


def refusal_vectors(tmp, path):
    """Every query that must come back REFUSED, on the file that provokes it, and the
    words its refusal sentence has to name. Three ride the golden bundle (a slot that
    was declared but never carried, a frame past a real animation, a switched-off row)
    and four ride a forged one. The same list checks the shipped build's sentences AND
    every mutant, so a guard relaxed in the C cannot answer ok on a bad file.
    """
    HAND, BANK = ts.CLASS_NAMES.index("hand"), ts.CLASS_NAMES.index("bank")
    off = golden_elements()
    off[2]["flags"] = ts.EF_HIDDEN
    p_off = write_bundle(tmp, [build(elements=off)], "hidden")
    return (
        (path,     (HAND, 4, 0),  "no-such-slot",
         ("hand", "slot 4"), "a clean file refuses a declared, uncarried slot"),
        (path,     (BANK, 2, 12), "frame-past-the-animation",
         ("bank", "slot 2"), "a clean file refuses a frame past the animation's end"),
        (p_off,    (HAND, 0, 0),  "slot-carried-empty",
         ("off",), "an EF_HIDDEN row is carried-empty, not absent"),
        # The carriage (element row 5): word 6 frame0, 7 n_frames, 10 w, 11 h. Each
        # forgery has to earn the sentence naming its own defect — an answer enum with
        # a blank sentence behind it is a support ticket nobody can read.
        (forge_file(tmp, "extent-lie", "elem", 5, 10, 999),
         (HAND, 5, 0),  "table-and-catalogue-disagree",
         ("hand", "slot 5", "999"), "an element whose w lies about its plane"),
        (forge_file(tmp, "window-off-the-end", "elem", 5, 6, 9999),
         (HAND, 5, 0),  "table-and-catalogue-disagree",
         ("hand", "slot 5", "catalogue"), "a frame window that runs off the bank"),
        (forge_file(tmp, "window-past-bank", "elem", 5, 7, 4095),
         (HAND, 5, 8),  "table-and-catalogue-disagree",
         ("hand", "slot 5"), "n_frames that overruns the whole catalogue"),
        # The month bank's first plane (catalogue row 9): size word neither an opaque
        # nor a masked plate, the one refusal that says "not a plane this build draws".
        (forge_file(tmp, "size-lie", "spr", 9, 1, 13),
         (BANK, 2, 0),  "table-and-catalogue-disagree",
         ("bank row 9", "bank", "slot 2", "neither"), "a bank row whose size lies"),
    )


# ---------------------------------------------------------------------------
# the mirrors: the vocabulary this C file copies is re-read from both authorities
# ---------------------------------------------------------------------------

def mirror_checks():
    src = open(Elems_C).read()
    hdr = open(os.path.join(ROOT, "main", "art", "orb_elems.h")).read()

    names = re.search(r"CLS_NAME\[8\] = \{(.*?)\};", src, re.S)
    check(names is not None, "orb_elems.c still carries its CLS_NAME mirror")
    if names:
        eq(re.findall(r'"([^"]+)"', names.group(1)), list(ts.CLASS_NAMES),
           "orb_elems.c's class names are theme_schema.CLASS_NAMES, in order")

    for cname, cval in (("EF_ALPHA", ts.EF_ALPHA), ("EF_LOOP", ts.EF_LOOP),
                        ("EF_OS_ADV", ts.EF_OS_ADV), ("EF_HIDDEN", ts.EF_HIDDEN)):
        m = re.search(r"#define\s+%s\s+0x([0-9A-Fa-f]+)u" % cname, src)
        check(m is not None, "orb_elems.c still defines %s" % cname)
        if m:
            eq(int(m.group(1), 16), cval,
               "orb_elems.c's %s is theme_schema's value" % cname)

    # The class ids, against the enum the app ABI freezes. orb_assets.h owns the
    # order; a re-numbered enum has to move this file's defines in the same commit,
    # which is the failure this half of the lint exists to make loud.
    aw = open(os.path.join(ROOT, "main", "assets", "orb_assets.h")).read()
    body = re.search(r"typedef enum \{(.*?)\} orb_asset_class_t;", aw, re.S).group(1)
    seen = 0
    for word, explicit in re.findall(r"ORB_ASSET_(\w+)(?:\s*=\s*(\d))?\s*,", body):
        if word == "CLASS_N":
            break
        if explicit:
            seen = int(explicit)
        m = re.search(r"#define\s+CLS_%s\s+(\d+)" % word.upper(), src)
        if m:
            eq(int(m.group(1)), seen,
               "orb_elems.c's CLS_%s is orb_assets.h's enum position %d" % (word, seen))
        seen += 1

    for call in ("orb_elems_at", "orb_elems_next", "orb_elems_last_frame",
                 "orb_elems_why"):
        check(call in hdr, "the header still promises %s()" % call)


def row_differs(fields, want):
    return any(fields.get(k) != v for k, v in want.items())


def name_for(cls, slot):
    """Which golden row a driver line came back for."""
    for (n, c, s, *_r) in GOLDEN:
        if c == ts.CLASS_NAMES[cls] and s == slot:
            return n
    return "?"



# ---------------------------------------------------------------------------
# mutants: rewrite the C, and the suite must notice
# ---------------------------------------------------------------------------

MUTANTS = (
    ("hidden rows answer anyway",
     "if (e->flags & EF_HIDDEN) {", "if (0) {"),
    ("the frame window is not checked",
     "if (frame >= e->n_frames) {", "if (0) {"),
    ("a frame window off the end of the bank",
     "if (row >= sec->n_spr) {", "if (0) {"),
    ("a plane of any size is believed",
     "if (!masked && sp.size != px * 2u) {", "if (0) {"),
    ("table and bank extents are not compared",
     "if (e->w && e->h && (sp.w != e->w || sp.h != e->h)) {", "if (0) {"),
    ("EF_ALPHA is ignored and a theme's alpha draws opaque",
     "out->alpha    = (e->flags & EF_ALPHA) ? e->alpha : 255u;",
     "out->alpha    = 255u;"),
    ("EF_HIDDEN is read as EF_ALPHA's bit",
     "#define EF_HIDDEN  0x08u", "#define EF_HIDDEN  0x01u"),
    ("a loop clamps where it should wrap",
     "return e->loop ? 0u : (uint32_t)e->n_frames - 1u;",
     "return (uint32_t)e->n_frames - 1u;"),
    ("a still row advances anyway",
     "if (!e || e->n_frames < 2u) return 0u;",
     "if (!e) return 0u; if (e->n_frames == 1u) return 1u;"),
    ("the first row of a class answers for every slot",
     "if (e.cls == cls && e.slot == slot) return seat(sec, &e, cls, slot, frame,",
     "if (e.cls == cls) return seat(sec, &e, cls, slot, frame,"),
)


def mutant_reacts(binp, path, vectors):
    """True when the mutant build disagrees with the shipped one, on good data OR bad.

    A relaxed guard is invisible on the well-formed golden bundle — it only changes the
    answer to a file the resolver is supposed to refuse. So every refusal vector is
    replayed here too: if the mutant returns `ok` where the clean build refuses, the
    guard mattered and this catches it.
    """
    lines = ask(binp, path, golden_queries())
    if len(lines) != len(golden_queries()):
        return True
    for ln in lines:
        d = parse(ln)
        if d["ans"] != "ok":
            return True
        if row_differs(d["fields"], expect(name_for(d["cls"], d["slot"]), d["frame"])):
            return True
    for (p, q, want, words, _desc) in vectors:
        d = parse(ask(binp, p, [q])[0])
        if d["ans"] != want:
            return True
        if want != "ok" and not all(w in d.get("why", "") for w in words):
            return True          # refused, but with a sentence naming the wrong defect
    return False


def mutation_checks(tmp, path, vectors):
    src = open(Elems_C).read()
    caught = 0
    for i, (label, old, new) in enumerate(MUTANTS):
        if src.count(old) != 1:
            check(False, "mutant %r no longer matches orb_elems.c exactly once "
                         "(found %d)" % (label, src.count(old)))
            continue
        try:
            mb = compile_driver(tmp, src.replace(old, new, 1), tag="mut%d" % i)
        except RuntimeError:
            caught += 1                        # refused at the compiler: still caught
            check(True, "mutant caught at the compiler: %s" % label)
            continue
        bad = mutant_reacts(mb, path, vectors)
        check(bad, "mutant went UNCAUGHT: %s" % label)
        caught += 1 if bad else 0
    print("  mutants caught %d/%d" % (caught, len(MUTANTS)))


# ---------------------------------------------------------------------------

def clean_file_refusals(binp, vectors):
    """What the shipped resolver says about every bad file it is handed."""
    for (p, q, want, words, desc) in vectors:
        lines = ask(binp, p, [q])
        check(bool(lines), "%s: answered at all" % desc)
        if not lines:
            continue
        d = parse(lines[0])
        eq(d["ans"], want, "%s: refuses as %s" % (desc, want))
        for w in words:
            check(w in d.get("why", ""), "%s: names %r: %r" % (desc, w, d.get("why", "")))


def main():
    do_mut = "--mutations" in sys.argv
    tmp = tempfile.mkdtemp(prefix="orbelems-")
    binp = compile_driver(tmp)
    path = write_bundle(tmp, [build()], "golden")

    # The fixture first: if the catalogue does not account for the bank byte-for-byte,
    # every offset expectation below is arithmetic about a file nobody wrote.
    cat = golden_sprites()
    eq(cat[-1]["off"] + cat[-1]["size"], len(golden_art()),
       "the fixture's catalogue accounts for its bank byte-for-byte")
    eq(sum(g[6] for g in GOLDEN), len(cat),
       "one catalogue row per frame of every golden element")

    vectors = refusal_vectors(tmp, path)

    golden_checks(binp, path)
    clean_file_refusals(binp, vectors)
    mirror_checks()
    if do_mut:
        mutation_checks(tmp, path, vectors)

    print("host_orbelems_test: %d checks, %d failed" % (checks[0], len(failures)))
    for f in failures[:20]:
        print("  FAIL " + f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
