#!/usr/bin/env python3
"""host_clock3_test.py — the third leg of the FORMAT 3 reader's three-way rule.

§25.5(2) put the clock section's projection on the device: main/art/orb_clock3.c
turns an element table, a sprite catalogue and 26 carried scalars into the one
clk_theme_t clock_view.c draws. §25.6 ruled that no element column is invented and
that the gif tables are DERIVED, which makes the projection's contract a pair of
numbers rather than a table: the same bundle must reproduce, field for field, the
header a kit bake already ships. That is what this file checks, and it checks it the
way the suite beside it checks everything — host gcc, the shipped C, the writer's own
bytes, no board, no card, no partition.

The oracle is `assets/clock_assets.h` plus `assets/clock_assets.bin`: the pair the
firmware is compiled with today, and the shape the dial actually consumes. §25.6's
round trip proved the product bundle rebuilds that pair byte-identically, so pinning
the C's output to it pins the C to the same truth `bake_clock.py --from-bundle` was
pinned to, from the other side of the wire. The art check is not decoration: a blob
base addressing different pixels would draw a wrong clock while every geometry field
still matched.

Refusal vectors run the other half of D26. Each forged table must draw the sentence
that names its own defect, and `--from-bundle` must refuse the same forgery with the
same words: two ends of one mapping that refuse differently are two mappings.

    python3 tools/host_clock3_test.py              # golden, derivations, refusals
    python3 tools/host_clock3_test.py --mutations   # and the C mutants
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
import orbbundle as ob              # noqa: E402  (the writer, the second oracle)
import struct as _st              # noqa: E402  (the forged words)

READER_C = os.path.join(ROOT, "main", "art", "orb_clock3.c")
BUNDLE_C = os.path.join(ROOT, "main", "art", "orb_bundle.c")
HDR_H = os.path.join(ROOT, "tests", "fixtures", "avkit-pair.h")
BIN_H = os.path.join(ROOT, "tests", "fixtures", "avkit-pair.bin")
REBAKED = "/tmp/b3/avkit.rebake.orbtheme"

failures, checks = [], [0]


def check(ok, msg):
    checks[0] += 1
    if not ok:
        failures.append(msg)
        print("  FAIL " + msg[:1600])


def ck(cond, msg):
    check(bool(cond), msg)


# ---------------------------------------------------------------------------
# The oracle, read out of the SHIPPED .h. Parsed here rather than imported from
# host_from_bundle_test.py so a drift in THIS file's reading fails against a source
# nobody edited for the occasion.
# ---------------------------------------------------------------------------
HDR = open(HDR_H).read()
ART = open(BIN_H, "rb").read()
SCR_W = int(re.search(r"#define CLK_SCR_W (\d+)", HDR).group(1))
SCR_H = int(re.search(r"#define CLK_SCR_H (\d+)", HDR).group(1))
GIFS = int(re.search(r"#define CLK_GIF_FRAMES (\d+)", HDR).group(1))
BLOB = int(re.search(r"#define CLK_BLOB_BYTES (\d+)u", HDR).group(1))
BODY = HDR[HDR.index("static const clk_theme_t clk_theme = {"):]
SPR_RE = re.compile(r"\{\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),"
                    r"\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+),"
                    r"\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\s*\}")


def nums(tag, count):
    """The integers printed after one comment tag, in the shipped .h's own order.

    A tolerant reader rather than one regex per field: the generator prints several
    fields under a single tag (`/* fps,loop,every */ 6, 0, 3600`), so a per-field
    pattern would key on whitespace the generator owes nobody. The count is an
    assert, which is what makes a re-flowed .h fail HERE rather than quietly shifting
    every expectation by one field.
    """
    m = re.search(r"/\* %s \*/\s*([-\d, x]+)" % re.escape(tag), HDR)
    if m is None:
        raise AssertionError("no `/* %s */` line in the shipped .h" % tag)
    txt = m.group(1).split("/*")[0].strip().rstrip(",").strip()
    v = [int(x, 0) for x in re.split(r"[,\s]+", txt) if x]
    assert len(v) == count, "%s: %d values, wanted %d" % (tag, len(v), count)
    return v


def spr_rows(seg, want):
    rows = [list(map(int, m.groups())) for m in SPR_RE.finditer(seg)]
    assert len(rows) == want, "%d sprite rows, wanted %d" % (len(rows), want)
    return rows


def block(tag):
    """One `/* tag */ { ... }` sprite array's own rows.

    The generator prints a lone sprite (`/* overlay */ { ... }`) on the tag's own
    line and an array's rows on the lines after it, so the terminator depends on
    which shape follows the tag."""
    open_tag = "/* %s */ {" % tag
    i = BODY.index(open_tag) + len(open_tag)
    eol = BODY.find("\n", i)
    if BODY[i:eol].strip():
        return "{" + BODY[i:eol]          # re-open the brace the tag consumed
    # The array closes with `},` and the theme's LAST field with a bare `}`.
    ends = [e for e in (BODY.find("\n  },", i), BODY.find("\n  }\n", i)) if e >= 0]
    return BODY[i:min(ends)]


def lst(pat):
    return [int(x) for x in re.search(pat, HDR).group(1).split(",")]


SWEEP = "sweep,railway,textOverHands,tickRate"
FPS3 = "fps,loop,every"
TRIP = "bg_mode,bg_key_off,gif_frames"
TOURB2 = "tourbillon_n,tourbillon_fps"
_spr = nums(SWEEP, 4)
_fps = nums(FPS3, 3)
_trip = nums(TRIP, 3)
_tb = nums(TOURB2, 2)
_dx = nums("shadow dx,dy", 2)
EXP = {
    "bg": nums("bg", 1)[0],
    "plate_follow": nums("plate_follow", 1)[0],
    "shadow_on": nums("shadow_on", 1)[0],
    "shadow_dx": _dx[0], "shadow_dy": _dx[1],
    "second_sweep": _spr[0], "second_railway": _spr[1],
    "text_over_hands": _spr[2], "tick_rate": _spr[3],
    "bg_frames": nums("bg_frames", 1)[0],
    "bg_fps": _fps[0], "bg_loop": _fps[1], "bg_every": _fps[2],
    "plate_off": nums("plate_off", 1)[0],
    "order": [int(x) for x in
              re.search(r"/\* order \*/ \{ ([\d, ]+)\}", BODY).group(1).split(",")],
    "order_n": nums("order_n", 1)[0],
    "matte_mask": nums("matte_mask", 1)[0],
    "bg_mode": _trip[0], "bg_key_off": _trip[1], "gif_frames": _trip[2],
    "tourbillon_n": _tb[0], "tourbillon_fps": _tb[1],
    "hand": spr_rows(block("hand"), 5),
    "shadow": spr_rows(block("shadow"), 5),
    "overlay": spr_rows(block("overlay"), 1)[0],
    "tourbillon": spr_rows(block("tourbillon"), 16),
    "delays": lst(r"clk_gif_delays\[\d+\] = \{ ([^}]*)\}"),
    "first": lst(r"clk_bg_rect_first\[\d+\] = \{ ([^}]*)\}"),
    "bgf": lst(r"static const uint32_t clk_bg_frames\[\d+\] = \{ ([^}]*)\}"),
}
_rs = HDR[HDR.index("clk_bg_rect_t clk_bg_rects"):]
EXP["rects"] = [list(map(int, m.groups())) for m in re.finditer(
    r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}", _rs[:_rs.index("};")])]


# ---------------------------------------------------------------------------
# The C harness. One binary, three modes, built from the shipped projection and the
# shipped bundle reader: dump a bundle's clock section, name the section a bundle
# does not carry, or report the projection's refusal. It includes orb_clock3.c
# rather than linking it so --mutations can drive a private copy of the source
# through the same main — the host_orbapp_reader_test.py arrangement.
# ---------------------------------------------------------------------------
SHIM = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zlib.h>
static inline uint32_t esp_rom_crc32_le(uint32_t c, const uint8_t *b, size_t n)
{ return (uint32_t)crc32((uLong)c, b, (uInt)n); }
"""

HARNESS_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orb_bundle.h"
#include "orb_clock3.h"

#define GIF_CAP 64u
static clk_theme_t        th;
static clk_bg_rect_t      rects[4];
static uint16_t           first_[GIF_CAP];
static uint16_t           delays[GIF_CAP];
static uint32_t           bgf[4];
static orb_bundle_t       b;

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "no file %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*n ? (size_t)*n : 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fprintf(stderr, "short read\n"); exit(2); }
    fclose(f);
    return b;
}

/* Every field of the projection, printed under the .h's own field names, so a
 * mismatch in this file's comparison reads like the .h line it disagrees with. */
static void dump_sprite(const char *tag, int i, const clk_sprite_t *s)
{
    printf("%s %d %u %u %d %d %d %d %d %d %d %d %u %d %d %u %u\n", tag, i,
           s->w, s->h, s->offx, s->offy, s->pivotx, s->pivoty, s->centerx,
           s->centery, s->blend, s->show, s->color_off, s->diffx, s->diffy,
           s->diffw, s->diffh);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: t --dump3f|--refuse|--section F [slug]\n"); return 2; }
    long n = 0;
    uint8_t *buf = slurp(argv[2], &n);
    if (n < 128) { printf("refused file is no container at all\n"); return 1; }
    orb_bundle3_hdr_t h;
    const char *r = orb_bundle3_hdr_parse((const uint32_t *)buf, &h);
    if (r) { printf("refused %s\n", r); return 1; }
    r = orb_bundle_parse(&h, buf, (uint32_t)n, &b);
    if (r) { printf("refused %s\n", r); return 1; }

    const char *slug = argc > 3 ? argv[3] : ORB_CLOCK3_SLUG;
    const orb_bundle_sec_t *s = orb_bundle_find(&b, slug);
    if (strcmp(argv[1], "--section") == 0) {
        printf("sections %u", b.n_sections);
        for (uint32_t i = 0; i < b.n_sections; i++) printf(" %s", b.sec[i].slug);
        printf("\n");
        printf("%s %s\n", s ? "found" : "absent", slug);
        return s ? 0 : 1;
    }
    if (!s) { printf("refused no %s section\n", slug); return 1; }

    orb_clock3_io_t io;
    memset(&io, 0, sizeof io);
    io.theme = &th; io.rects = rects; io.rect_first = first_;
    io.gif_delays = delays; io.bg_frames = bgf;
    io.first_cap = GIF_CAP; io.delays_cap = GIF_CAP;
    r = orb_clock3_parse(s, &io);
    if (strcmp(argv[1], "--refuse") == 0) {
        printf("%s %s\n", r ? "refused" : "ACCEPTED", r ? r : "");
        return r ? 0 : 1;
    }
    if (r) { printf("refused %s\n", r); return 1; }

    printf("scalar bg %u\n", th.bg);
    printf("scalar plate_follow %u\n", th.plate_follow);
    printf("scalar shadow_on %u\n", th.shadow_on);
    printf("scalar shadow_dx %d\n", th.shadow_dx);
    printf("scalar shadow_dy %d\n", th.shadow_dy);
    printf("scalar second_sweep %u\n", th.second_sweep);
    printf("scalar second_railway %u\n", th.second_railway);
    printf("scalar text_over_hands %u\n", th.text_over_hands);
    printf("scalar tick_rate %u\n", th.tick_rate);
    printf("scalar bg_frames %u\n", th.bg_frames);
    printf("scalar bg_fps %d\n", th.bg_fps);
    printf("scalar bg_loop %u\n", th.bg_loop);
    printf("scalar bg_every %u\n", th.bg_every);
    printf("scalar plate_off %u\n", th.plate_off);
    printf("scalar order %u %u %u %u %u\n", th.order[0], th.order[1], th.order[2],
           th.order[3], th.order[4]);
    printf("scalar order_n %u\n", th.order_n);
    printf("scalar matte_mask %u\n", th.matte_mask);
    printf("scalar bg_mode %u\n", th.bg_mode);
    printf("scalar bg_key_off %u\n", th.bg_key_off);
    printf("scalar gif_frames %u\n", th.gif_frames);
    printf("scalar tourbillon_n %u\n", th.tourbillon_n);
    printf("scalar tourbillon_fps %u\n", th.tourbillon_fps);
    for (int i = 0; i < 5; i++)  dump_sprite("hand", i, &th.hand[i]);
    for (int i = 0; i < 5; i++)  dump_sprite("shadow", i, &th.shadow[i]);
    dump_sprite("overlay", 0, &th.overlay);
    for (int i = 0; i < CLK_TOURB_MAX; i++) dump_sprite("tourbillon", i, &th.tourbillon[i]);
    printf("rects %u\n", (unsigned)(th.bg_mode == 1 ? 1u : 0u));
    printf("rect %u %u %u %u %u\n", rects[0].x, rects[0].y, rects[0].w, rects[0].h,
           rects[0].off);
    for (uint32_t i = 0; i <= th.gif_frames; i++) printf("first %u %u\n", i, first_[i]);
    for (uint32_t i = 0; i < th.gif_frames; i++) printf("delay %u %u\n", i, delays[i]);
    printf("bgf %u\n", bgf[0]);
    printf("art %u %u\n", io.out_art_off, io.out_art_size);
    printf("scr %u %u\n", s->scr_w, s->scr_h);
    printf("counts %u %u\n", s->n_elem, s->n_spr);
    return 0;
}
"""


def build(mut=None, exe=None):
    """Compile the shipped projection (or one mutated copy) into the harness."""
    d = tempfile.mkdtemp(prefix="clock3.")
    src = READER_C if mut is None else None
    if mut is not None:
        text = open(READER_C).read()
        old, new = mut
        assert old in text, "mutant anchor gone: %r" % (old,)
        assert text.count(old) == 1, "mutant anchor not unique: %r" % (old,)
        src = os.path.join(d, "orb_clock3.c")
        open(src, "w").write(text.replace(old, new))
    exe = exe or os.path.join(d, "t")
    shim = os.path.join(d, "shim.h")
    open(os.path.join(d, "esp_rom_crc.h"), "w").write(SHIM)
    open(shim, "w").write("#include <string.h>\n")
    open(os.path.join(d, "harness.c"), "w").write(HARNESS_C)
    cc = subprocess.run(
        ["gcc", "-O1", "-Wall", "-Wextra", "-Wno-unused-function", "-D_GNU_SOURCE",
         "-include", shim,
         "-I", d, "-I", os.path.join(ROOT, "main", "art"),
         "-I", os.path.join(ROOT, "assets"),
         src, BUNDLE_C, os.path.join(d, "harness.c"),
         "-Wl,--gc-sections", "-o", exe, "-lz"],
        capture_output=True, text=True)
    if cc.returncode:
        sys.exit("clock3 harness would not compile:\n" + cc.stderr[:4000])
    return exe


def run(exe, mode, bundle, arg=None):
    argv = [exe, mode, bundle] + ([arg] if arg else [])
    p = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    return p.returncode, p.stdout, p.stderr


def parsed(out):
    """The harness's dump, as dicts keyed the way EXP is."""
    sc, spr, first, delay = {}, {}, {}, {}
    rects, bgf, art, scr, cnt, nrect = None, None, None, None, None, None
    for line in out.splitlines():
        w = line.split()
        if w[0] == "scalar":
            sc[w[1]] = [int(x) for x in w[2:]]
        elif w[0] in ("hand", "shadow", "tourbillon"):
            spr.setdefault(w[0], {})[int(w[1])] = [int(x) for x in w[2:]]
        elif w[0] == "overlay":
            spr["overlay"] = [int(x) for x in w[2:]]
        elif w[0] == "rect":
            rects = [int(x) for x in w[1:]]
        elif w[0] == "rects":
            nrect = int(w[1])
        elif w[0] == "first":
            first[int(w[1])] = int(w[2])
        elif w[0] == "delay":
            delay[int(w[1])] = int(w[2])
        elif w[0] == "bgf":
            bgf = int(w[1])
        elif w[0] == "art":
            art = (int(w[1]), int(w[2]))
        elif w[0] == "scr":
            scr = (int(w[1]), int(w[2]))
        elif w[0] == "counts":
            cnt = (int(w[1]), int(w[2]))
    return dict(sc=sc, spr=spr, first=first, delay=delay, rects=rects, bgf=bgf,
                art=art, scr=scr, counts=cnt, nrect=nrect)


SCALARS = ("bg", "plate_follow", "shadow_on", "shadow_dx", "shadow_dy",
           "second_sweep", "second_railway", "text_over_hands", "tick_rate",
           "bg_frames", "bg_fps", "bg_loop", "bg_every", "plate_off", "order_n",
           "matte_mask", "bg_mode", "bg_key_off", "gif_frames", "tourbillon_n",
           "tourbillon_fps")


def c_string(lit):
    """A C string literal, as the compiled bytes: adjacent literals concatenate,
    escapes resolve. Refusal sentences are compared byte for byte, so the
    comparison has to read the source the way the compiler does."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', lit)
    return "".join(p.encode().decode("unicode_escape") for p in parts)


def c_sentence(src, needle):
    """One refusal sentence, lifted from the projection's own source, so a
    vector's expectation is a quote from the file rather than a paraphrase of it."""
    i = src.index(needle)
    end = src.index(";", i)
    return c_string(src[i:end])


def compare_golden(p, where):
    """Every field the projection printed, against the shipped pair."""
    for k in SCALARS:
        ck(k not in ("order",) and k in p["sc"], "%s: harness printed no scalar %s"
           % (where, k))
        if k in p["sc"] and k in EXP:
            got = p["sc"][k][0]
            ck(got == EXP[k], "%s: scalar %s is %s, the shipped .h says %s"
               % (where, k, got, EXP[k]))
    o = p["sc"].get("order", [])
    exp_order = EXP["order"] + [0] * (5 - len(EXP["order"]))
    ck(o == exp_order, "%s: layer order is %s, the shipped .h says %s"
       % (where, o, exp_order))
    for tag in ("hand", "shadow"):
        for i in range(5):
            got = p["spr"].get(tag, {}).get(i)
            ck(got == EXP[tag][i], "%s: %s[%d] is %s, the shipped .h says %s"
               % (where, tag, i, got, EXP[tag][i]))
    ck(p["spr"].get("overlay") == EXP["overlay"],
       "%s: overlay is %s, the shipped .h says %s"
       % (where, p["spr"].get("overlay"), EXP["overlay"]))
    for i in range(16):
        ck(p["spr"].get("tourbillon", {}).get(i) == EXP["tourbillon"][i],
           "%s: tourbillon[%d] is not the shipped blank row" % (where, i))
    return p


# ---------------------------------------------------------------------------
# The fixture, built here rather than trusted from /tmp.
#
# §25.4's product bundle is a re-bake of a FORMAT 2 container, and that container
# carries the art bytes its own bake read — which for av-kit is a delta-rect bank
# rebuilt from the kit's PNGs, whose flat border pixels differ from the shipped
# .bin by a few dozen bytes. Those bytes are the kit bake's business, not this
# projection's: the suite's claim is about the TABLE and the EXTENTS. So the golden
# vector is a bundle forged from the shipped pair — geometry out of the .h, art out
# of the .bin — which makes bank byte-identity true by construction instead of by
# somebody remembering to regenerate a /tmp file. The §25.4 bundle is still driven
# through the same reader as a second, independently written table.
# ---------------------------------------------------------------------------
def geom_blob(art):
    """The FORMAT 2 geometry blob the shipped .h describes, in orbtheme's order."""
    import orbtheme as ot
    counts = [1, GIFS + 1, GIFS, 1, 0, 0, 0]           # rects, first, delays, bgf,
    scal = [EXP["bg"], EXP["plate_follow"], EXP["shadow_on"],
            EXP["shadow_dx"] & 0xFF, EXP["shadow_dy"] & 0xFF,
            EXP["second_sweep"], EXP["second_railway"], EXP["text_over_hands"],
            EXP["tick_rate"], EXP["bg_frames"], EXP["bg_fps"] & 0xFFFF,
            EXP["bg_loop"], EXP["bg_every"], EXP["plate_off"]]
    scal += EXP["order"] + [0] * (5 - len(EXP["order"]))
    scal += [EXP["order_n"], EXP["matte_mask"], EXP["bg_mode"], EXP["bg_key_off"],
             EXP["gif_frames"], EXP["tourbillon_n"], EXP["tourbillon_fps"]]
    assert len(scal) == 26, "the dialect is 26 words, not %d" % len(scal)
    g = bytearray()
    for v in counts + scal:
        g += ot.struct.pack("<I", v & 0xFFFFFFFF) if hasattr(ot, "struct") else \
            __import__("struct").pack("<I", v & 0xFFFFFFFF)
    for row in (EXP["hand"] + EXP["shadow"] + [EXP["overlay"]] + EXP["tourbillon"]):
        for v in row:
            g += __import__("struct").pack("<I", v & 0xFFFFFFFF)
    for r in EXP["rects"]:
        for v in r:
            g += __import__("struct").pack("<I", v & 0xFFFFFFFF)
    for name in ("first", "delays", "bgf"):
        for v in EXP[name]:
            g += __import__("struct").pack("<I", v & 0xFFFFFFFF)
    return bytes(g)


def pair_bundle(path):
    """A FORMAT 3 clock bundle whose table came from the shipped .h and whose bank IS
    the shipped .bin — the golden vector, forgeable without PIL because the pair
    already holds every number a kit bake would read."""
    import orbtheme as ot
    import orbtheme as _t
    art = ART
    geom = geom_blob(art)
    f2 = path + ".f2"
    ot.write_container(f2, "av-kit", geom, art, SCR_W, SCR_H)
    import rebake_bundle as rbx
    rbx.rebake(f2, path)
    return path



def main():
    exe = build()
    if not os.path.exists(REBAKED):
        sys.exit("no product bundle at %s — run rebake_bundle.py first" % REBAKED)
    pair = pair_bundle(os.path.join(tempfile.mkdtemp(prefix="clock3f."), "pair.orbtheme"))

    # ---- the golden: the pair-forged bundle, then the product bundle ----
    for where, path in (("pair-forged bundle", pair), ("product bundle", REBAKED)):
        rc, out, err = run(exe, "--dump3f", path)
        ck(rc == 0, "%s projected cleanly (rc %d: %s%s)"
           % (where, rc, out[:300], err[:300]))
        if rc:
            continue
        p = compare_golden(parsed(out), where)
        # ---- the derived block, against the pair §25.6 proved identical ----
        ck(p["nrect"] == len(EXP["rects"]) == 1,
           "the gif block carries %s rects where the pair carries %d"
           % (p["nrect"], len(EXP["rects"])))
        ck(p["rects"] == EXP["rects"][0],
           "the one rect is %s, the shipped pair's is %s" % (p["rects"], EXP["rects"][0]))
        ck(p["bgf"] == EXP["bgf"][0], "the plate-frame row is %s, not the keyframe"
           % p["bgf"])
        ck(len(p["first"]) == len(EXP["first"]),
           "the index table has %d rows, the pair has %d" % (len(p["first"]), len(EXP["first"])))
        ck(all(v == 0 for v in p["first"].values()) == all(v == 0 for v in EXP["first"]),
           "the index windows do not all close at zero")
        ck(len(p["delay"]) == len(EXP["delays"]),
           "the delay table has %d rows, the pair has %d" % (len(p["delay"]), len(EXP["delays"])))
        ck(sorted(set(p["delay"].values())) == sorted(set(EXP["delays"])),
           "the delay table is %s, the pair's is %s"
           % (sorted(set(p["delay"].values())), sorted(set(EXP["delays"]))))
        beat = (1000 + EXP["tick_rate"] // 2) // max(1, EXP["tick_rate"])
        ck(set(p["delay"].values()) == {beat},
           "the delays are not one uniform figure at the OS beat (%d ms)" % beat)
        # ---- the blob base: where the pixels are, and that they are the pair's ----
        hdr, secs = ob.read_bundle(path)
        sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
        ck(p["art"] == (sec["rec"]["art_off"], len(sec["art"])),
           "the projection named the bank at %s, the record says (%d, %d)"
           % (p["art"], sec["rec"]["art_off"], len(sec["art"])))
        ck(p["art"][1] == BLOB == len(ART),
           "the bank is %s B; the compiled pair's blob is %d B" % (p["art"][1], len(ART)))
        if path == pair:
            # The one place a bank's BYTES are the claim: this bundle was forged from
            # the pair, so equality is construction and not lineage. The product
            # bundle's bank is a kit re-bake's pixels, checked for length and extents
            # below rather than for byte identity it never promised.
            ck(bytes(sec["art"]) == ART,
               "the pair-forged bundle's clock bank is not the shipped .bin")
            ck(p["counts"] == (8, 8),
               "the pair-forged table is %s rows, not the 8 the shipped pair has"
               % (p["counts"],))
        else:
            ck(len(sec["art"]) == p["art"][1] == BLOB,
               "the product bundle's bank is %d B against a projected %s of %d"
               % (len(sec["art"]), p["art"][1], BLOB))
        ck(p["scr"] == (SCR_W, SCR_H), "%s: the panel pair is %s" % (where, p["scr"]))
    print("  golden: both bundles project to the shipped pair")
    return exe, pair




# ---------------------------------------------------------------------------
# Refusals. Each vector forges ONE lie into a copy of the pair-forged bundle and
# requires both ends of the mapping to refuse it: the shipped C, and the host
# reverse path `bake_clock.py --from-bundle`. A needle is the phrase §25.6 pinned
# as shared between them, checked against both sentences rather than against a
# paraphrase — the two ends print the same rule in their own numbers.
# ---------------------------------------------------------------------------
import theme_schema as ts          # noqa: E402


def forge(base, mutate):
    """The pair-forged clock bundle with one element-table or scalar lie applied."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    mutate(els, sc)
    out = "%s.%d.forged" % (base, forge.n)
    forge.n += 1
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": sec["blob"]["sprites"], "art": bytes(sec["art"]),
                      "scr_w": sec["rec"]["scr_w"], "scr_h": sec["rec"]["scr_h"]}])
    return out


forge.n = 0


def drop_hour_hand(els, sc):
    """The hour hand's row gone, its shadow left hanging: §25.2's crop-pivot
    identity needs the hand, so the light offset is not recoverable."""
    els[:] = [e for e in els
              if not (e["class"] == ts.CLASS_HAND and e["slot"] == 0)]


def _unused_dup(els, sc):
    """A second row for the plate's own slot, under a fresh `n`.

    The n has to move: theme_schema.check_table refuses a repeated row identity at
    the WRITER's door, and a forgery the writer will not emit is not a thing the
    device can be handed. With a distinct n the table is writer-legal and the only
    guard left standing is the projection's own (class, slot) rule."""
    els.append(dict(els[0], n=max(e["n"] for e in els) + 1))


def bake_says(path):
    """`--from-bundle`'s verdict on a forged bundle: (refused, sentence)."""
    d = tempfile.mkdtemp(prefix="fkb.")
    p = subprocess.run(
        [sys.executable, os.path.join(HERE, "bake_clock.py"), "--from-bundle", path,
         "--header", os.path.join(d, "o.h"), "--bin", os.path.join(d, "o.bin")],
        capture_output=True, text=True, timeout=180)
    txt = p.stdout + p.stderr
    if p.returncode == 0:
        return False, "ACCEPTED"
    m = re.search(r"refused:.*", txt, re.S)
    return True, (m.group(0)[:500] if m else txt[:500])


REFUSALS = (
    # A class the compiled clock has no field for, and a second row for one slot,
    # are refused by theme_schema.check_table at pack_blob's door: no bundle exists
    # that carries them, so the projection's matching guards are the backstop for a
    # hand-written or damaged file and are pinned in the mutant battery instead
    # (D26's "the refusal belongs to the bake" rule, orb_bundle.h's own wording for
    # a too-wide table). What the writer WILL emit and the dial must still refuse:
    ("a text row, which check_table allows", lambda e, s: e[0].update({"class": ts.CLASS_TEXT, "n_frames": 1, "radius": 0, "degrees": 0, "scale16": 0}),
     "complication mapping"),
    ("a hand at slot 5", lambda e, s: e[2].update({"slot": 5}),
     "diff-rect fields ride no bundle column"),
    ("a plate at slot 2", lambda e, s: e[1].update({"slot": 2}),
     "one plate and one overlay"),
    ("no plate row", lambda e, s: e.pop(0), "no plate has no centre"),
    ("a moved plate", lambda e, s: e[0].update({"x": 4}),
     "not the whole panel is not a plate"),
    ("a plate naming 3 frames", lambda e, s: e[0].update({"n_frames": 3}),
     "holds one keyframe"),
    ("tourbillon_n 5, no rows", lambda e, s: s.update({"tourbillon_n": 5}),
     "scalars and the table disagree"),
    ("bg_frames 2", lambda e, s: s.update({"bg_frames": 2}), "does not carry"),
    ("a shadow with no hand", drop_hour_hand, "light offset is only recoverable"),
)


def refusals(exe, base):
    for what, mut, needle in REFUSALS:
        path = forge(base, mut)
        rc, out, err = run(exe, "--refuse", path)
        cre = rc == 0
        csent = out[:400]
        refused, bsent = bake_says(path)
        ck(cre, "%s: the C projection accepted it (%s)" % (what, csent[:160]))
        ck(refused, "%s: --from-bundle accepted it" % what)
        ck(needle in csent, "%s: the C said %r, not the sentence naming it (%r)"
           % (what, csent[:200], needle))
        ck(needle in bsent, "%s: --from-bundle said %r, missing %r"
           % (what, bsent[:200], needle))
    print("  refusals: %d forgeries, both ends agree" % len(REFUSALS))



# ---------------------------------------------------------------------------
# Entry. The ordinary run is golden, derivations and refusals; --mutations adds the
# guard battery, which requires the run to CHANGE when a guard stops guarding.
# ---------------------------------------------------------------------------


def sprite_forgery(base, how):
    """A clock bundle whose catalogue lies while its element table stays honest.

    pack_blob validates only that each plane fits the bank, so a non-contiguous,
    mis-sized or short catalogue IS writer-legal — which is exactly why the
    projection owns the contiguity, extent and accounting rules, and why these three
    guards need a driver of their own rather than the golden table."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art = bytes(sec["art"])
    if how == "panel":
        # Not a catalogue lie at all: the section asks for a 466x233 panel, which is a
        # thing a bundle may legitimately say and this dial must refuse. Written here
        # because it needs the same per-section keys.
        pass
    elif how == "swap":
        spr[1], spr[2] = spr[2], spr[1]
    elif how == "shrink":
        spr[3]["size"] -= 4
    elif how == "short":
        spr.pop()
    out = "%s.spr-%s.orbtheme" % (base, how)
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": [dict(e) for e in sec["blob"]["elements"]],
                      "scalars": dict(sec["blob"]["scalars"]), "sprites": spr,
                      "art": art, "scr_w": sec["rec"]["scr_w"],
                      "scr_h": 233 if how == "panel" else sec["rec"]["scr_h"]}])
    return out


from orbtheme import W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC, W_HDR_CRC
IDX_N_ELEM, IDX_GEOM_OFF, IDX_GEOM_SIZE, IDX_GEOM_CRC = 8, 12, 16, 20
IDX_SCR = 36
ELEM_WORDS_BYTES = ob._ts.ELEMENT_WORDS * 4   # the stride the writer lays rows at today: 19
BLOB_HEAD_BYTES = (4 + 26) * 4


def sections_of(buf):
    """FORMAT 3 carries the section count in the header word FORMAT 2 spent on scr_w."""
    from orbtheme import W_SCR_W
    return _st.unpack_from("<I", buf, 4 * W_SCR_W)[0]


def _regeom(buf):
    """Restamp every checksum a forged byte invalidates, innermost first.

    Three figures ride a bundle's index and header, and a byte edit breaks all three:
    each record's geom_crc is that SECTION's own blob checksum, the header's geom_crc
    covers the whole region, and the header's hdr_crc covers the header with its own
    field zero. Restamp only one and the file dies at a gate that is not the guard
    under test — which is how a first cut of this battery reported
    "geometry checksum mismatch" for all four word-level vectors.
    """
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    geom_size = _st.unpack_from("<I", buf, 4 * W_GEOM_SIZE)[0]
    for i in range(sections_of(buf)):
        rec = geom_off + 8 + i * 40
        b_off = _st.unpack_from("<I", buf, rec + IDX_GEOM_OFF)[0]
        b_size = _st.unpack_from("<I", buf, rec + IDX_GEOM_SIZE)[0]
        _st.pack_into("<I", buf, rec + IDX_GEOM_CRC,
                      zlib.crc32(bytes(buf[geom_off + b_off:
                                           geom_off + b_off + b_size])) & 0xFFFFFFFF)
    crc = zlib.crc32(bytes(buf[geom_off:geom_off + geom_size])) & 0xFFFFFFFF
    _st.pack_into("<I", buf, 4 * W_GEOM_CRC, crc)
    _st.pack_into("<I", buf, 4 * W_HDR_CRC, 0)
    _st.pack_into("<I", buf, 4 * W_HDR_CRC,
                  zlib.crc32(bytes(buf[:128])) & 0xFFFFFFFF)
    return crc


def word_tamper(base, rec_i, field_byte, new):
    """One 32-bit word at one byte offset of one index record, forged AFTER the write, region checksum
    restamped. The guards this drives are the ones theme_schema.check_table closes at
    the writer's door, so a hand-edited file is the only way to put them in front of
    the reader — which is precisely the damage a card that lost a sector can do."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    geom_size = _st.unpack_from("<I", buf, 4 * W_GEOM_SIZE)[0]
    n = sections_of(buf)
    rec = geom_off + 8 + rec_i * 40
    buf[rec + field_byte: rec + field_byte + 4] = _st.pack("<I", new)
    crc = zlib.crc32(bytes(buf[geom_off:geom_off + geom_size])) & 0xFFFFFFFF
    _st.pack_into("<I", buf, 24, crc)                    # header geom_crc (word 6)
    for i in range(n):                                   # every record's own copy
        r = geom_off + 8 + i * 40
        _st.pack_into("<I", buf, r + 16, crc)             # IDX_ORDER geom_crc
    out = "%s.word%d-%d.orbtheme" % (base, rec_i, field_byte)
    open(out, "wb").write(bytes(buf))
    return out


# (guard, source anchor, break, driver). The driver names the input ONLY this guard
# refuses: a clean golden table satisfies every one of them, so breaking a guard and
# re-running the golden proves nothing — §25.6's lesson about the name check that
# compared itself. `vec` is a REFUSALS name, `spr` a catalogue lie, `word` a forged
# index record past the writer's door.
MUTANTS = (
    ("the catalogue's contiguity rule", "if (sp.off != at)", "if (0 && sp.off != at)",
     ("spr", "swap")),
    ("a plane's own extent", "if (sp.size != want)", "if (0 && sp.size != want)",
     ("spr", "shrink")),
    ("the bank's byte-for-byte accounting", "if (at != art)", "if (0 && at != art)",
     ("spr", "short")),
    ("the plate's full-panel rule",
     "if (plate.x || plate.y || plate.w != (uint16_t)scr_w || plate.h != (uint16_t)scr_h)",
     "if (0 && (plate.x || plate.y || plate.w != (uint16_t)scr_w ||"
     " plate.h != (uint16_t)scr_h))", ("vec", "a moved plate")),
    ("the plate's one-frame rule", "if (plate.n_frames != 1)",
     "if (0 && plate.n_frames != 1)", ("vec", "a plate naming 3 frames")),
    ("the plate's existence", "if (at_plate[0] < 0)", "if (0 && at_plate[0] < 0)",
     ("vec", "no plate row")),
    # A row's frame0 and its own w are index-record words on the wire, so forging them
    # means editing a written file: check_element bounds frame0 against a declared
    # count it cannot see, and cannot cross-check w against a catalogue plane at all.
    ("a row's seat in the catalogue", "if (e.frame0 >= ns)", "if (0 && e.frame0 >= ns)",
     ("elem", 0, 6, 99)),
    ("a row's size against its plane", "if (sp.w != e.w || sp.h != e.h)",
     "if (0 && sp.w != e.w || sp.h != e.h)", ("elem", 2, 10, 30)),
    # Anchors name the refusal, not the `if`: `if (e.slot >= 5)` guards both the hand
    # and the sprite bound and is not unique, and a mutant that cannot be compiled
    # must abort the run rather than pass for a caught one.
    ("the hand slot bound", 'if (e.slot >= 5)\n                    return reject("a tourbillon',
     'if (0)\n                    return reject("a tourbillon',
     ("vec", "a hand at slot 5")),
    ("the tourbillon scalar's claim", "if (t->tourbillon_n)",
     "if (0 && t->tourbillon_n)", ("vec", "tourbillon_n 5, no rows")),
    ("the plate-frame count", "if (t->bg_frames)", "if (0 && t->bg_frames)",
     ("vec", "bg_frames 2")),
    # check_table refuses a repeated (class, slot) and an unknown class, so the
    # projection's matching guards are the backstop for a damaged file: driven as a
    # forged row class, which the writer would never emit.
    ("one row per slot", "if (*cell >= 0)", "if (0 && *cell >= 0)",
     ("elem", 2, 1, 3)),
    ("the shadow's hand", 'if (at_hand[i] < 0)\n            return reject("a shadow',
     'if (0)\n            return reject("a shadow', ("vec", "a shadow with no hand")),
    ("the panel pair", "if (scr_w != CLK_SCR_W || scr_h != CLK_SCR_H)",
     "if (0 && (scr_w != CLK_SCR_W || scr_h != CLK_SCR_H))", ("spr", "panel")),
)


def mutant_input(drv, golden, by_name):
    """The file one guard exists to refuse."""
    kind = drv[0]
    if kind == "vec":
        return forge(golden, by_name[drv[1]])
    if kind == "spr":
        return sprite_forgery(golden, drv[1])
    if kind == "rec":
        return word_tamper(golden, drv[1], drv[2], drv[3])
    if kind == "elem":
        return elem_tamper(golden, drv[1], drv[2], drv[3])
    raise AssertionError("no such mutant driver %r" % (kind,))


def elem_tamper(base, row, word, new):
    """One word of one element row, forged after the write and restamped."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    geom_size = _st.unpack_from("<I", buf, 4 * W_GEOM_SIZE)[0]
    n = sections_of(buf)
    blob_off = _st.unpack_from("<I", buf, geom_off + 8 + IDX_GEOM_OFF)[0]
    at = geom_off + blob_off + BLOB_HEAD_BYTES + row * ELEM_WORDS_BYTES + word * 4
    buf[at:at + 4] = _st.pack("<I", new)
    _st.pack_into("<I", buf, 24, _regeom(buf))
    out = "%s.elem%d-%d.orbtheme" % (base, row, word)
    open(out, "wb").write(bytes(buf))
    return out


def mutants(exe, golden_path):
    """Each guard broken in a private copy, and asked about the lie only it refuses.
    An uncaught mutant is a device-side refusal nobody is watching — which is how
    §24.7's generic format line got as far as a dial."""
    by_name = {v[0]: v[1] for v in REFUSALS}
    honest = {}
    for what, old, new, drv in MUTANTS:
        path = mutant_input(drv, golden_path, by_name)
        if path not in honest:
            honest[path] = run(exe, "--refuse", path)
        hrc, hout, _ = honest[path]
        ck("refused" in hout, "%s: the honest reader accepted the input that only"
           " the %s guard refuses (%s)" % (what, what, hout[:120]))
        m = build(mut=(old, new))       # sys.exits if the rewrite will not compile
        rc, out, err = run(m, "--refuse", path)
        ck(rc != hrc or out != hout,
           "uncaught mutant: %s — broken, it answered %r for %s"
           % (what, out[:120], path.split("/")[-1]))
    print("  mutants: %d guards, each asked about its own lie" % len(MUTANTS))




if __name__ == "__main__":
    EXE, PAIR = main()
    refusals(EXE, PAIR)
    if "--mutations" in sys.argv:
        mutants(EXE, PAIR)
    print("%d checks, %d failures" % (checks[0], len(failures)))
    sys.exit(1 if failures else 0)
