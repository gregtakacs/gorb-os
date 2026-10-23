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
static clk_bg_rect_t      rects[CLK_BG_MAX];
static uint16_t           first_[GIF_CAP];
static uint16_t           delays[GIF_CAP];
static uint32_t           bgf[CLK_BG_MAX];
static clk_comp_t         comps[16];
static clk_aux_t          auxs[CLK_AUX_MAX];   /* D34: the pointer table, sized as the
                                                * device sizes it — CLK_AUX_MAX is the
                                                * cap the projection refuses past. */
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
    io.comps = comps; io.comp_cap = 16;
    io.aux   = auxs;  io.aux_cap   = CLK_AUX_MAX;   /* D34 */
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
    printf("scalar tourbillon_start %u\n", th.tourbillon_start);
    for (int i = 0; i < 5; i++)  dump_sprite("hand", i, &th.hand[i]);
    for (int i = 0; i < 5; i++)  dump_sprite("shadow", i, &th.shadow[i]);
    dump_sprite("overlay", 0, &th.overlay);
    for (int i = 0; i < CLK_TOURB_MAX; i++) dump_sprite("tourbillon", i, &th.tourbillon[i]);
    printf("n_comps %u\n", io.out_n_comps);
    for (uint32_t i = 0; i < io.out_n_comps; i++) {
        const clk_comp_t *c = &comps[i];
        printf("comp %u %u %u %u %d %d %u %u %u %u %u %u %d %u %u %u %u\n", i,
               c->tag, c->kind, c->src, c->x, c->y, c->w, c->h, c->count,
               c->first, c->off, c->frame_bytes, c->value_min, c->scale, c->show,
               c->parent_row, (unsigned)c->delay_ms);
    }
    printf("n_aux %u\n", io.out_n_aux);
    /* idx tag kind(motion) src x y w h pivot span zero vmin vmax show flags pivotx
     * pivoty centerx centery — every field the row carried, read back off the struct
     * the projection filled. A pointer that arrived with a zero vmax would have been
     * refused, so a printed vmax is itself the proof the guards let a legal row
     * through. The flag word is the projection's own mapping (CLK_FLAG_AUX_SHADOW =
     * twin), which is how the FORMAT 3 row flag bit and the FORMAT 2 dialect prove
     * out as one contract, and the pivot/centre pair is what lets the twin's line
     * read as §25.2's identity: centre = hand centre + light. */
    for (uint32_t i = 0; i < io.out_n_aux; i++) {
        const clk_aux_t *a = &auxs[i];
        printf("aux %u %u %u %u %d %d %u %u %d %d %d %u %u %u %u %d %d %d %d %u %u\n", i,
               a->tag, a->kind, a->src, a->spr.offx, a->spr.offy, a->spr.w, a->spr.h,
               (int)a->span, (int)a->zero, (int)a->value_min, (unsigned)a->value_max,
               (unsigned)a->spr.color_off, a->spr.show, (unsigned)a->flags,
               (int)a->spr.pivotx, (int)a->spr.pivoty,
               (int)a->spr.centerx, (int)a->spr.centery,
               a->parent_row, (unsigned)a->delay_ms);
    }
    printf("rects %u\n", (unsigned)(th.bg_mode == 1 ? 1u : 0u));
    printf("rect %u %u %u %u %u\n", rects[0].x, rects[0].y, rects[0].w, rects[0].h,
           rects[0].off);
    /* C4: every materialized rect row, and the section's own geometry dialect, so
     * a battery can name the delta rects the projection wrote and the stamp that
     * authorised them without a second reader. `geom abi_max estride bstride` is
     * what the container gate let through. `rectrow i` is the honest list, one
     * line per materialized rect — the L3/L4 gif windows say there are
     * `rect_first[gif_frames]` of them, zero for a keyframe table and the extra
     * planes for a delta plate. */
    printf("geom %u %u %u\n", (unsigned)s->abi_max, (unsigned)s->elem_words,
           (unsigned)s->spr_words);
    {
        uint32_t nmat = (th.gif_frames <= GIF_CAP) ? first_[th.gif_frames] : 0u;
        printf("nmat %u\n", nmat);
        for (uint32_t i = 0; i < nmat && i < CLK_BG_MAX; i++)
            printf("rectrow %u %u %u %u %u %u\n", i, rects[i].x, rects[i].y,
                   rects[i].w, rects[i].h, rects[i].off);
    }
    for (uint32_t i = 0; i <= th.gif_frames; i++) printf("first %u %u\n", i, first_[i]);
    for (uint32_t i = 0; i < th.gif_frames; i++) printf("delay %u %u\n", i, delays[i]);
    printf("bgf %u\n", bgf[0]);
    /* A keyframe bank fills io.bg_frames for its bg_frames extra planes; a delta
     * plate fills it for its gif_frames-1 loop frames (§5.2). Both are the offsets
     * the bgAnim consumer flips through, so print the row's own loop length. */
    {
        uint32_t nb = (th.bg_mode == 1 && th.gif_frames >= 2) ? (th.gif_frames - 1u)
                                                              : th.bg_frames;
        for (uint32_t i = 0; i < nb && i < CLK_BG_MAX; i++) printf("bgframe %u %u\n", i, bgf[i]);
    }
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
    comps, bgl, aux = [], {}, []
    rectrows, geom, nmat = [], None, None
    for line in out.splitlines():
        w = line.split()
        if w[0] == "scalar":
            sc[w[1]] = [int(x) for x in w[2:]]
        elif w[0] in ("n_comps", "n_aux"):
            pass
        elif w[0] == "geom":
            geom = dict(zip(("abi_max", "elem_words", "spr_words"),
                            (int(v) for v in w[1:])))
        elif w[0] == "rectrow":
            rectrows.append(dict(zip(("i", "x", "y", "w", "h", "off"),
                                     (int(v) for v in w[1:]))))
        elif w[0] == "nmat":
            nmat = int(w[1])
        elif w[0] == "aux":
            # idx tag kind src x y w h span zero vmin vmax color_off show flags
            # pivotx pivoty centerx centery parent_row delay_ms — the harness's own
            # order, named here so the battery reads a field and not an index. flags
            # carries the projection's CLK_FLAG_AUX_SHADOW (256). parent_row is the
            # resolved row index + 1 (0 = none); delay_ms the row's own tempo.
            aux.append(dict(zip(("i", "tag", "kind", "src", "x", "y", "w", "h", "span",
                                 "zero", "vmin", "vmax", "off", "show", "flags",
                                 "pivotx", "pivoty", "cx", "cy", "parent_row",
                                 "delay_ms"),
                                (int(v) for v in w[1:]))))
        elif w[0] == "comp":
            comps.append([int(x) for x in w[1:]])
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
        elif w[0] == "bgframe":
            bgl[int(w[1])] = int(w[2])
        elif w[0] == "art":
            art = (int(w[1]), int(w[2]))
        elif w[0] == "scr":
            scr = (int(w[1]), int(w[2]))
        elif w[0] == "counts":
            cnt = (int(w[1]), int(w[2]))
    return dict(sc=sc, spr=spr, first=first, delay=delay, rects=rects, bgf=bgf,
                art=art, scr=scr, counts=cnt, nrect=nrect, comps=comps, bgl=bgl,
                aux=aux, rectrows=rectrows, geom=geom, nmat=nmat)


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
    # D32 ruling C: hand slot 5 IS a thing now — without a scalar naming its
    # length it is the same lie the old tourbillon_n vector was. Slot 6 is the
    # new "no such slot".
    ("a carriage row, no frames named", lambda e, s: e[2].update({"slot": 5}),
     "scalars and the table disagree"),
    ("a hand at slot 6", lambda e, s: e[2].update({"slot": 6}),
     "and no more"),
    # D32 R-C's own pair of guards: the row and the scalar must name one bank,
    # and that bank must fit the array the dial holds.
    ("a carriage length lie", lambda e, s: (e[2].update({"slot": 5}),
                                            s.update({"tourbillon_n": 3})),
     "animation's length"),
    ("a carriage of twenty frames", lambda e, s: (e[2].update({"slot": 5, "n_frames": 20}),
                                                  s.update({"tourbillon_n": 20})),
     "no room for"),
    ("a plate at slot 2", lambda e, s: e[1].update({"slot": 2}),
     "one plate and one overlay"),
    ("no plate row", lambda e, s: e.pop(0), "no plate has no centre"),
    ("a moved plate", lambda e, s: e[0].update({"x": 4}),
     "not the whole panel is not a plate"),
    ("a plate naming 3 frames", lambda e, s: e[0].update({"n_frames": 3}),
     "animation's length"),   # §31: the row and the scalar must name one bank
    ("an animated glass", lambda e, s: e[1].update({"n_frames": 2}),
     "animated glass"),       # §31 rule 5: the overlay is still one plate
    ("a plate animation longer than the dial", lambda e, s: (
        e[0].update({"n_frames": 18}),
        s.update({"bg_mode": 0, "gif_frames": 0, "bg_frames": 17, "bg_loop": 1})),
     "no room for"),
    ("a one-shot plate bank with no tempo", lambda e, s: (
        e[0].update({"n_frames": 3}),
        s.update({"bg_mode": 0, "gif_frames": 0, "bg_frames": 2,
                  "bg_loop": 0, "bg_fps": 0})),
     "no tempo"),
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
# D32's accept side: the refusals above refuse the carriage and the bank in
# their lies; this forges them in their truth. The pair-forged bundle grows a
# carriage row (class hand, slot 5, three synthetic planes) and a month window
# (class bank, slot 0, twelve plates because a year has twelve months), with
# filler pixels — the projection's claim is the TABLE (offsets, extents, the
# domain, the stride); the pixels are R-H's bench question, asked on the glass.
# ---------------------------------------------------------------------------
CAR_W, CAR_H = 64, 64
WIN_W, WIN_H = 40, 30
CAR_PLANE = CAR_W * CAR_H * 3
WIN_PLANE = WIN_W * WIN_H * 3


def d32_forge(base, kind="both"):
    """The pair-forged clock bundle plus a carriage row and/or a month window."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art = bytes(sec["art"])
    at = len(art)

    def planes(w, h, cnt):
        nonlocal at
        first = len(spr)
        for _ in range(cnt):
            spr.append({"off": at, "size": w * h * 3, "w": w, "h": h})
            art.extend(b"\x11" * (w * h * 3))
            at += w * h * 3
        return first

    art = bytearray(art)
    proto = dict(els[0])
    car_f0 = win_f0 = 0
    if kind in ("both", "carriage"):
        car_f0 = planes(CAR_W, CAR_H, 3)
        row = dict(proto, n=20, slot=5, flags=0, order=1, alpha=255,
                   frame0=car_f0, n_frames=3, x=120, y=120,
                   w=CAR_W, h=CAR_H, pivot_x=32, pivot_y=32, radius=0,
                   degrees=0, scale16=0, kind=0, src=0)
        row["class"] = ts.CLASS_HAND
        els.append(row)
        sc["tourbillon_n"] = 3
        sc["tourbillon_fps"] = 12
    if kind in ("both", "window"):
        win_f0 = planes(WIN_W, WIN_H, 12)
        row = dict(proto, n=21, slot=0, flags=0, order=3, alpha=255,
                   frame0=win_f0, n_frames=12, x=60, y=200,
                   w=WIN_W, h=WIN_H, pivot_x=0, pivot_y=0, radius=0,
                   degrees=0, scale16=16, kind=0, src=2)   # src 2: month
        row["class"] = ts.CLASS_BANK
        els.append(row)
    out = "%s.d32-%s.orbtheme" % (base, kind)
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": spr, "art": bytes(art),
                      "scr_w": sec["rec"]["scr_w"], "scr_h": sec["rec"]["scr_h"]}])
    return out, car_f0, win_f0, len(sec["art"])


def bake_pair(path):
    """`--from-bundle` on a path, with the header it wrote (accept-side pins)."""
    d = tempfile.mkdtemp(prefix="fkb.")
    p = subprocess.run(
        [sys.executable, os.path.join(HERE, "bake_clock.py"), "--from-bundle", path,
         "--header", os.path.join(d, "o.h"), "--bin", os.path.join(d, "o.bin")],
        capture_output=True, text=True, timeout=180)
    hdr = ""
    try:
        hdr = open(os.path.join(d, "o.h")).read()
    except OSError:
        pass
    return p.returncode, p.stdout + p.stderr, hdr


def d32_battery(exe, base):
    path, car_f0, win_f0, art_len = d32_forge(base, "both")
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "the carriage-and-window bundle was refused: %s" % out[:300])
    if rc:
        return
    p = parsed(out)
    car_off = art_len
    win_off = art_len + 3 * CAR_PLANE
    ck(p["sc"]["tourbillon_n"] == [3] and p["sc"]["tourbillon_fps"] == [12]
       and p["sc"]["tourbillon_start"] == [0],
       "the carriage scalars read %s / %s / %s"
       % (p["sc"]["tourbillon_n"], p["sc"]["tourbillon_fps"],
          p["sc"]["tourbillon_start"]))
    tb = p["spr"]["tourbillon"]
    ck(tb[0][9] == 1, "carriage frame 0 is not shown")
    ck(tb[0][10] == car_off and tb[1][10] == car_off + CAR_PLANE
       and tb[2][10] == car_off + 2 * CAR_PLANE,
       "the bank does not stride the catalogue: %s %s %s"
       % (tb[0][10], tb[1][10], tb[2][10]))
    ck(tb[0][0] == CAR_W and tb[0][6] == 152 and tb[0][7] == 152,
       "the bank's geometry is not the carriage row's own")
    ck(tb[3][9] == 0 and tb[3][0] == 0,
       "the bank ran past tourbillon_n into the array")
    ck(p["spr"]["hand"][2][9] == 1, "R-C: the second hand vanished beside the carriage")
    ck(len(p["comps"]) == 1, "the month window did not project alone: %d rows"
       % len(p["comps"]))
    c = p["comps"][0]
    # The window carries no parent and no per-row tempo, so C4's two appended
    # comp columns (parent_row, delay_ms) land on their zeros with the row.
    ck(c[1:] == [0, 0, 2, 60, 200, WIN_W, WIN_H, 12, 3, win_off, WIN_PLANE, 0, 16, 1,
                 0, 0],
       "the window projected as %s, not the row-and-domain record" % (c,))

    # --from-bundle: the pair carries the bank it is shown and refuses the
    # complication it has no field for, saying so in one sentence each.
    refused, bsent = bake_says(path)
    ck(refused and "complication mapping" in bsent,
       "the pair end refused the window bundle differently: %r" % bsent[:200])
    cpath, _, _, car_len = d32_forge(base, "carriage")
    brc, btxt, bhdr = bake_pair(cpath)
    ck(brc == 0, "the carriage-only bundle: --from-bundle said %r" % btxt[:300])
    if brc == 0:
        ck("/* tourbillon_n,tourbillon_fps */ 3, 12," in bhdr,
           "the rebuilt .h does not carry the bank the row named")
        rows = re.findall(r"\{\s*([\d, -]+)\s*\}, /\* tourb00 \*/", bhdr)
        ck(rows and [int(x) for x in rows[0].split(",")][10] == car_len,
           "the rebuilt tourbillon frame 0 does not sit on its plane")

    # The device's own guards, past the writer's door: one word each, restamped.
    for label, word, val, needle in (
            ("battery source", 18, 9, "no such input"),
            ("a month of three plates", 7, 3, "indexes 12 plates"),
            ("a strip kind", 17, 1, "glyph"),
            ("order rung seven", 3, 7, "five live layers")):
        t = elem_tamper(path, 9, word, val)
        rc, out, err = run(exe, "--refuse", t)
        ck(rc == 0 and needle in out,
           "%s: the device said %r, not %r" % (label, out[:160], needle))
        refused, bsent = bake_says(t)
        ck(refused and "complication mapping" in bsent,
           "%s: the pair end drifted from its one sentence: %r" % (label, bsent[:160]))
    t = elem_tamper(cpath, 8, 15, 9000)          # the carriage's degrees
    rc, out, err = run(exe, "--dump3f", t)
    ck(rc == 0, "the device refused a start orientation: %s" % out[:160])
    ck(parsed(out)["sc"]["tourbillon_start"] == [9000],
       "the start orientation did not land in the theme struct")
    refused, bsent = bake_says(t)
    ck(refused and "tourbillon_start" in bsent,
       "the pair refused a start orientation it cannot carry, but not in words: %r"
       % bsent[:200])
    print("  D32 battery: carriage, window, and the guards between the doors")


# ---------------------------------------------------------------------------
# §31's accept side: a keyframe plate bank, true at both ends. The pair-forged
# bundle grows n-1 colour-only full-panel planes directly behind the plate's —
# the same contiguous, OS-advanced shape as the D32 carriage, pointed at the
# background instead of a hand. The pixels are filler (the bench is the glass's
# question, R-H as ever); the claim is the table, the offsets, the scalars, and
# the plate-frame rows the projection writes for the bgAnim consumer.
# ---------------------------------------------------------------------------
KF_PLANE = SCR_W * SCR_H * 2         # one colour-only keyframe: 434,312 B


def s31_forge(base, n=3, tag="", mutate=None):
    """The pair-forged clock section with the plate row animating n keyframes."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art = bytearray(sec["art"])
    k = n - 1
    art[KF_PLANE:KF_PLANE] = b"\x11" * (KF_PLANE * k)     # the keyframes, plate-first
    for i in range(1, len(spr)):
        spr[i]["off"] += KF_PLANE * k
    spr[1:1] = [{"off": KF_PLANE * (j + 1), "size": KF_PLANE, "w": SCR_W, "h": SCR_H}
                for j in range(k)]
    for e in els:
        if not (e["class"] == ts.CLASS_PLATE and e["slot"] == 0):
            e["frame0"] += k
        else:
            e["n_frames"] = n
    sc.update({"bg_mode": 0, "gif_frames": 0, "bg_frames": k, "bg_fps": 6,
               "bg_loop": 1, "bg_every": 1, "plate_off": 0})
    if mutate:
        mutate(els, sc, spr, art)
    out = "%s.s31%s.orbtheme" % (base, ("-" + tag) if tag else "-anim")
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": spr, "art": bytes(art),
                      "scr_w": sec["rec"]["scr_w"], "scr_h": sec["rec"]["scr_h"]}])
    return out


# ---------------------------------------------------------------------------
# D34: the pointer row. One row that moves by the drive the THEME wrote into it,
# projected by both ends; then the lies that class makes possible, each refused by
# both doors in the same words. The row is a masked plane, so its cost is w*h*3.
# ---------------------------------------------------------------------------
AUX_W, AUX_H = 28, 64
AUX_PLANE = AUX_W * AUX_H * 3          # colour then the planar alpha stride


def d34_forge(base, tag="", mutate=None, kind=0, src="second", span=3600,
              zero=0, vmin=0, vmax=60, slot=0, size=None):
    """The av-kit clock section with one AUX pointer row appended to its table."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art = bytearray(sec["art"])
    w, h = size or (AUX_W, AUX_H)
    off = len(art)
    art += b"\x22" * (w * h * 3)                       # the pointer's own plate
    els.append(dict(ts.blank_element(), **{
        "n": max(e["n"] for e in els) + 1, "class": ts.CLASS_AUX, "slot": slot,
        "order": 3, "alpha": 255, "frame0": len(spr), "n_frames": 1,
        "x": 300, "y": 120, "w": w, "h": h,
        "pivot_x": w // 2, "pivot_y": h - 8,
        "kind": kind, "src": ts.SOURCES.index(src),
        "span": span, "zero": zero, "vmin": vmin, "vmax": vmax}))
    spr.append({"off": off, "size": w * h * 3, "w": w, "h": h})
    if mutate:
        mutate(els, sc, spr, art)
    out = "%s.d34%s.orbtheme" % (base, ("-" + tag) if tag else "-aux")
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": spr, "art": bytes(art),
                      "scr_w": sec["rec"]["scr_w"], "scr_h": sec["rec"]["scr_h"]}])
    return out


def d34_battery(exe, base):
    """One pointer accepted by both doors, and the pointer-only lies mirrored."""
    path = d34_forge(base, "aux")
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "the device refused a pointer row: %s" % (out or err)[:200])
    if rc == 0:
        p = parsed(out)
        ck(len(p.get("aux", [])) == 1,
           "the projection seated %d pointer rows, not the one the table named"
           % len(p.get("aux", [])))
        a = p["aux"][0]
        ck(a["kind"] == 0 and a["src"] == ts.SOURCES.index("second") and
           a["span"] == 3600 and a["vmax"] == 60,
           "the clk_aux_t did not carry the row\u2019s own drive back: %s" % a)
        ck(a["w"] == AUX_W and a["h"] == AUX_H and a["show"] == 1,
           "the pointer sprite is %s, not the %dx%d plane the row placed" % (a, AUX_W, AUX_H))
        ck(p["counts"] == (9, 9 + 1) or p["counts"][0] == 9,
           "the pointer row is not in the element count: %s" % (p["counts"],))
        ck(p["art"][1] == BLOB + AUX_PLANE,
           "the bank is %s B, not the av-kit\u2019s %d plus the pointer\u2019s %d"
           % (p["art"][1], BLOB, AUX_PLANE))

    # The pair end REFUSES a pointer, and that asymmetry is the ruling rather than a
    # gap: the device has a clk_aux_t table to project into and the compiled .h has no
    # such array, because the pair's own aux table was FORMAT 2's and §26 retired it.
    # Pinned as a refusal with its own sentence, so a future slice that gives the pair
    # a pointer table fails here and has to come back and change this line on purpose.
    brc, btxt, bhdr = bake_pair(path)
    ck(brc != 0, "--from-bundle accepted a pointer row into a .h that has no aux array")
    ck("carries no pointer rows" in btxt,
       "the pair end refused a pointer for some other reason: %r" % btxt[:240])

    # A slide is accepted, and accepted with the geometry a slide needs: no pivot to
    # turn about, a span in pixels. Refusing it would be refusing the half of D34 that
    # is not an angle, so this is the accept side of the same door.
    sp = d34_forge(base, "slide", kind=3, src="battery", span=90, zero=-45,
                   vmin=0, vmax=100, size=(90, 10))
    rc, out, err = run(exe, "--dump3f", sp)
    ck(rc == 0, "the device refused a sliding pointer: %s" % (out or err)[:200])
    if rc == 0:
        a = parsed(out)["aux"][0]
        ck(a["kind"] == 3 and a["span"] == 90 and a["zero"] == -45,
           "the slide did not project its own motion and pixel span: %s" % a)

    # §26 slice 4, the pointer's shadow twin: the flag rides the u8 row column as
    # bit 4, projects onto the runtime pairing flag, and lands the twin's centre on
    # the hand's centre plus the light — the §25.2 identity the centred hands'
    # shadows already carry, said now by the aux table. The hand row forged above
    # draws at (300,120) 28x64 about (14,56); its twin adds two pad each side and
    # takes the light (4,4): draw point (302,122), pivot (16,58), centre (318,180).
    def add_twin(flag=1 << 4, n=1):
        def m(els, sc, spr, art):
            for k2 in range(n):
                w, h, x0, y0 = 32 + 8 * k2, 68 + 4 * k2, 302 + 2 * k2, 122 + 2 * k2
                off = len(art)
                art += b"\x33" * (w * h * 3)
                els.append(dict(ts.blank_element(), **{
                    "n": max(e["n"] for e in els) + 1, "class": ts.CLASS_AUX,
                    "slot": k2 + 1, "order": 3, "alpha": 255, "frame0": len(spr),
                    "n_frames": 1, "x": x0, "y": y0, "w": w, "h": h,
                    "pivot_x": 16 + 4 * k2, "pivot_y": 58 + 2 * k2, "flags": flag,
                    "kind": 0, "src": ts.SOURCES.index("second"),
                    "span": 3600, "zero": 0, "vmin": 0, "vmax": 60}))
                spr.append({"off": off, "size": w * h * 3, "w": w, "h": h})
        return m

    tw = d34_forge(base, "twin", mutate=add_twin())
    rc, out, err = run(exe, "--dump3f", tw)
    ck(rc == 0, "the device refused a pointer with its shadow twin: %s" % (out or err)[:200])
    if rc == 0:
        rows = parsed(out)["aux"]
        ck(len(rows) == 2, "the twin did not project as the second row: %d rows" % len(rows))
        ck(rows[0]["flags"] == 0 and rows[1]["flags"] == 256,
           "the twin flag did not project as CLK_FLAG_AUX_SHADOW on the twin row alone: %s" % rows)
        ck(rows[1]["cx"] == rows[0]["cx"] + 4 and rows[1]["cy"] == rows[0]["cy"] + 4,
           "the twin's centre is not the hand's centre plus the light: %s vs %s" % (rows[1], rows[0]))

    # Orphans are the lies this flag makes possible, and the door names them in the
    # pairing's own words: a twin with no hand before it has no pose to take, and a
    # twin under a twin darkens a shadow, which is a smudge with a story.
    def orphan(els, sc, spr, art):
        for e in els:
            if e["class"] == ts.CLASS_AUX:
                e["slot"] = 1
                e["flags"] = 1 << 4
    orp = d34_forge(base, "orph", mutate=orphan)
    rc, out, err = run(exe, "--refuse", orp)
    ck(rc == 0 and "no hand before it" in out,
       "a twin with no hand before it: the device said %r, not the pairing sentence" % (out or err)[:180])
    stk = d34_forge(base, "stack", mutate=add_twin(n=2))
    rc, out, err = run(exe, "--refuse", stk)
    ck(rc == 0 and "under another shadow" in out,
       "a twin under a twin: the device said %r, not the pairing sentence" % (out or err)[:180])

    # The one lie the WRITER lets through, because ELEMENT_WORDS_MAX is not a thing
    # and a ninth pointer slot is a legal u16 field: the device must refuse it, in the
    # sentence that names the cap. The other pointer lies this slice guards are
    # writer-illegal by construction — theme_schema's own self-test refuses each one
    # (see its D34 vector set), which is the cheaper door, and a hostile card that
    # skipped that door still meets the C one above.
    t = d34_forge(base, "slot8", slot=8)
    rc, out, err = run(exe, "--refuse", t)
    ck(rc == 0 and "pointer slots" in out,
       "a ninth pointer slot: the device said %r, not the cap sentence"
       % (out or err)[:180])
    # A pointer at the LAST legal slot projects, and its ordinal survives: eight rows
    # is the cap, so slot 7 must be the widest accepted table.
    t7 = d34_forge(base, "slot7", slot=7)
    rc, out, err = run(exe, "--dump3f", t7)
    ck(rc == 0, "the device refused a pointer at the last legal slot: %s" % (out or err)[:160])
    print("  D34 battery: pointer accepted by the device, refused by the pair mirror "
          "for its own reason, and past the cap in the cap\u2019s own words")


def s31_battery(exe, base):
    """One honest bank projected by both ends, and the lies it makes possible."""
    path = s31_forge(base, 3, "anim")
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "the device refused a keyframe plate bank: %s" % (out or err)[:200])
    if rc == 0:
        p = parsed(out)
        ck(p["sc"]["bg_frames"] == [2],
           "the projected scalar bank length is %s, not the row's 2 extra"
           % p["sc"].get("bg_frames"))
        ck(p["sc"]["bg_mode"] == [0] and p["sc"]["gif_frames"] == [0],
           "the keyframe loop did not project mode 0 with no delta vocabulary: %s"
           % {k: p["sc"].get(k) for k in ("bg_mode", "gif_frames")})
        ck(p["bgl"] == {0: KF_PLANE, 1: 2 * KF_PLANE},
           "the plate-frame table is %s, not the extra planes' own offsets" % p["bgl"])
        ck(p["counts"] == (8, 10) and p["art"][1] == BLOB + 2 * KF_PLANE,
           "the animated bundle is %s rows over %s B, not (8, 10) over %d"
           % (p["counts"], p["art"][1], BLOB + 2 * KF_PLANE))

    brc, btxt, bhdr = bake_pair(path)
    ck(brc == 0, "the pair end refused the animated plate: %r" % btxt[:300])
    ck("/* bg_frames */ 2, /* fps,loop,every */ 6, 1, 1," in bhdr,
       "the rebuilt .h does not carry the bank the row named")
    m = re.search(r"clk_bg_frames\[\d+\] = \{ ([^}]+) \}", bhdr)
    ck(m and [int(x) for x in m.group(1).split(",")] == [KF_PLANE, 2 * KF_PLANE],
       "the rebuilt bgAnim table does not point at the keyframes")

    # Bank-only lies: each one is writer-legal, and both ends must say the
    # projection's own sentence for it.
    for label, mut, needle in (
            ("a scalar disagreeing with the row",
             lambda e, s, r, a: s.update({"bg_frames": 3}), "animation's length"),
            ("delta mode claiming whole planes",
             lambda e, s, r, a: s.update({"bg_mode": 1}), "does not carry"),
            ("a bank that starts off the plate",
             lambda e, s, r, a: s.update({"plate_off": KF_PLANE}),
             "frame 0 of the loop"),
            ("a keyframe carrying an alpha stride",
             lambda e, s, r, a: r[1].update({"size": r[1]["size"] + SCR_W * SCR_H}),
             "that extent is"),
            ("keyframes shuffled out of bank order",
             lambda e, s, r, a: r.__setitem__(slice(1, 3), r[1:3][::-1]),
             "not contiguous")):
        t = s31_forge(base, 3, "lie", mut)
        rc, out, err = run(exe, "--refuse", t)
        ck(rc == 0 and needle in out,
           "%s: the device said %r, not %r" % (label, (out or err)[:160], needle))
        refused, bsent = bake_says(t)
        ck(refused and needle in bsent,
           "%s: the pair end drifted from the one sentence: %r" % (label, bsent[:160]))
    print("  §31 battery: keyframe bank accepted at both ends, %d bank-only lies mirrored"
          % 5)



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
    ("the plate's zero-frame backstop", "if (!plate.n_frames)",
     "if (0 && !plate.n_frames)", ("elem", 0, 7, 0)),
    ("the plate's existence", "if (at_plate[0] < 0)", "if (0 && at_plate[0] < 0)",
     ("vec", "no plate row")),
    # A row's frame0 and its own w are index-record words on the wire, so forging them
    # means editing a written file: check_element bounds frame0 against a declared
    # count it cannot see, and cannot cross-check w against a catalogue plane at all.
    ("a row's seat in the catalogue", "if (e.frame0 >= ns)", "if (0 && e.frame0 >= ns)",
     ("elem", 1, 6, 99)),
    ("a row's size against its plane", "if (sp.w != e.w || sp.h != e.h)",
     "if (0 && sp.w != e.w || sp.h != e.h)", ("elem", 2, 10, 30)),
    # Anchors name the refusal, not the `if`: the hand bound and the sprite bound
    # share the shape of their test, and a mutant that cannot be compiled must
    # abort the run rather than pass for a caught one.
    ("the hand slot bound", 'if (e.slot >= 6)',
     'if (0 && e.slot >= 6)',
     ("vec", "a hand at slot 6")),
    ("the carriage's capacity", "if (car.n_frames > CLK_TOURB_MAX)",
     "if (0 && car.n_frames > CLK_TOURB_MAX)",
     ("vec", "a carriage of twenty frames")),
    ("the carriage's length against its scalar", "if (t->tourbillon_n != car.n_frames)",
     "if (0 && t->tourbillon_n != car.n_frames)",
     ("vec", "a carriage length lie")),
    ("the tourbillon scalar's claim", "if (t->tourbillon_n && at_hand[5] < 0)",
     "if (0 && t->tourbillon_n && at_hand[5] < 0)", ("vec", "tourbillon_n 5, no rows")),
    ("the plate-frame count under delta mode",
     "if (t->bg_frames && t->bg_mode == 1)",
     "if (0 && t->bg_frames && t->bg_mode == 1)",
     ("vec", "bg_frames 2")),
    ("the plate bank's one length", "if (t->bg_frames != (uint16_t)(plate.n_frames - 1u))",
     "if (0 && t->bg_frames != (uint16_t)(plate.n_frames - 1u))",
     ("vec", "a plate naming 3 frames")),
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


def _blob_row_bytes(buf):
    """The element-row stride of the clock blob (record 0) this file carries —
    derived from that blob's own length exactly as blob_check() derives it
    (§23.5), not from the schema's current width. C4 moved the current width to
    25 words, and the whole point of the append-only rule is that a forger
    aiming at a v3 file must pay the v3 width the file actually lays its rows
    at. The catalogue width is keyed by the record's own stamp, as the device
    reader keys it."""
    import theme_schema as ts
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    rec = geom_off + 8                       # record 0's head, after the index head
    abi = _st.unpack_from("<H", buf, rec + 6)[0]          # IDX_ORDER abi_max
    blob_at = geom_off + _st.unpack_from("<I", buf, rec + IDX_GEOM_OFF)[0]
    n_elem, n_spr, _nsc, n_slug = _st.unpack_from("<4I", buf, blob_at)
    blob_size = _st.unpack_from("<I", buf, rec + IDX_GEOM_SIZE)[0]
    spr_bytes = 24 if abi >= ts.CLOCK_ABI_V4 else 16
    fixed = (4 + 26) * 4 + n_spr * spr_bytes + n_slug + (-n_slug % 4)
    return (blob_size - fixed) // (n_elem * 4) * 4 if n_elem else \
        ts.ELEMENT_WORDS * 4


def elem_tamper(base, row, word, new):
    """One word of one element row, forged after the write and restamped."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    n = sections_of(buf)
    blob_off = _st.unpack_from("<I", buf, geom_off + 8 + IDX_GEOM_OFF)[0]
    at = geom_off + blob_off + BLOB_HEAD_BYTES + row * _blob_row_bytes(buf) + word * 4
    buf[at:at + 4] = _st.pack("<I", new)
    _st.pack_into("<I", buf, 24, _regeom(buf))
    out = "%s.elem%d-%d.orbtheme" % (base, row, word)
    open(out, "wb").write(bytes(buf))
    return out


def abi_lie(base, abi_max):
    """The clock section's own index-record stamp forged after the write and fully
    restamped, so the file reaches the clock gate and not the header's. Decision A
    lives here: a stride that disagrees with the stamp it rides is not a dialect to
    decode. The record's abi_max is the u16 at byte 6 (IDX_ORDER: the first <4H)."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    rec = geom_off + 8                              # record 0's head
    _st.pack_into("<H", buf, rec + 6, abi_max)       # abi_max
    _st.pack_into("<H", buf, rec + 4, abi_max)       # keep abi_min <= abi_max
    _st.pack_into("<I", buf, 24, _regeom(buf))       # full restamp, header included
    out = "%s.abi%d.orbtheme" % (base, abi_max)
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




# ---------------------------------------------------------------------------
# C4 — the delta dialect (geometry ABI v4). The refusals battery proves a v3
# table stays honest; this proves the v4 append is honoured and that the gate
# refuses a lie rather than guessing at it. `c4_forge` re-emits the pair-forged
# clock section at a stamp, so the byte-level contract — exact stride, the
# parent/delay columns, the delta rects — is the shipped C reader's claim, not
# the host packer's own. Every vector bends one field out of an honest v4 base.
# ---------------------------------------------------------------------------


def c4_forge(base, abi=4, mutate=None):
    """The pair-forged clock section re-emitted at geometry stamp `abi`, with an
    optional mutate(els, sc, art, spr) on the read-back table before re-pack."""
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    els = [dict(e) for e in sec["blob"]["elements"]]
    sc = dict(sec["blob"]["scalars"])
    spr = [dict(s) for s in sec["blob"]["sprites"]]
    art = bytearray(sec["art"])
    if mutate:
        mutate(els, sc, art, spr)
    out = "%s.c4-%d.orbtheme" % (base, c4_forge.n)
    c4_forge.n += 1
    ob.write_bundle(out, hdr["name"],
                    [{"slug": "clock", "elements": els, "scalars": sc,
                      "sprites": spr, "art": bytes(art),
                      "scr_w": sec["rec"]["scr_w"], "scr_h": sec["rec"]["scr_h"],
                      "abi_min": abi, "abi_max": abi}])
    return out


c4_forge.n = 0
C4_MONTH = ts.SOURCES.index("month")     # domain 12, the instanced window's value
C4_WINDOW = ts.KINDS.index("window")
C4_SCR = (SCR_W, SCR_H)                  # the panel the plate must fill
C4_PLATE_BYTES = SCR_W * SCR_H * 2       # a colour-only plate plane (RGB565)


def c4_defaults(els, sc, art, spr):
    """Honest v4 base: the two appended columns ride at their defaults, so every
    row reads parent NONE and delay 0 — the shape a flat v4 bake of the av-kit
    pair lays down, and the base every accept and refusal vector starts from."""
    for e in els:
        e.setdefault("parent", ts.PARENT_NONE)
        e.setdefault("delay", 0)


def c4_rect(w, h, dx, dy, art, spr, fill=b"\x22"):
    """Append one delta-plane catalogue row at the bank's tail; returns its offset."""
    at = len(art)
    spr.append({"off": at, "size": w * h * 3, "w": w, "h": h, "diffx": dx, "diffy": dy})
    art.extend(fill * (w * h * 3))
    return at


def c4_bank_row(els, n, slot, src, frame0, n_frames, flags=0, parent=None, delay=0):
    """A fresh month-window BANK row (the class a parent/instance/delay may ride)."""
    return dict(els[0], n=n, slot=slot, flags=flags, order=1,
                parent=ts.PARENT_NONE if parent is None else parent, delay=delay,
                **{"class": ts.CLASS_BANK, "kind": C4_WINDOW, "src": src},
                x=30, y=30, w=80, h=60, frame0=frame0, n_frames=n_frames)


# A 4-plane delta plate: frame 0 whole at the plate offset, frames 1..3 rects that
# ride the catalogue's diff columns. The row is the loop (bg_frames stays 0) and
# carries its own tempo, so the one-shot guard passes on truth rather than bg_fps.
C4_DELTA_RECTS = [(40, 40, 64, 64), (200, 30, 32, 48), (10, 400, 80, 50)]


def c4_build_delta_bank(els, sc, art, spr, rects, delay):
    """Rebuild the bank so the plate's frames lead it: one whole colour-only plane
    at offset 0, then one colour-only rect (w*h*2) per delta frame. The pair's other
    planes (overlay, hands, shadows — RGB888) shift back by the rect count and every
    non-plate row's frame0 follows. bg_frames stays 0 (the row IS the loop) and the
    row carries its own tempo, so the one-shot guard passes on truth rather than on
    bg_fps. `rects` is [(dx, dy, w, h)] and each rect must fit the plate box."""
    c4_defaults(els, sc, art, spr)
    plate = next(e for e in els if e["class"] == ts.CLASS_PLATE and e["slot"] == 0)
    scr_w, scr_h = C4_SCR
    whole = bytes(art[0:scr_w * scr_h * 2])         # the pair's own plate pixels
    survivors = []                                  # the RGB888 planes that stay
    for sp in spr[1:]:
        survivors.append({"size": sp["size"], "w": sp["w"], "h": sp["h"],
                          "bytes": bytes(art[sp["off"]:sp["off"] + sp["size"]])})
    art[:] = whole
    for dx, dy, w, h in rects:
        art.extend(b"\x22" * (w * h * 2))           # colour-only, like the plate
    for s in survivors:
        art.extend(s["bytes"])
    del spr[:]
    spr.append({"off": 0, "size": scr_w * scr_h * 2, "w": scr_w, "h": scr_h,
                "diffx": 0, "diffy": 0})
    at = scr_w * scr_h * 2
    for dx, dy, w, h in rects:
        spr.append({"off": at, "size": w * h * 2, "w": w, "h": h,
                    "diffx": dx, "diffy": dy})
        at += w * h * 2
    for s in survivors:                             # offsets continue the plate block
        spr.append({"off": at, "size": s["size"], "w": s["w"], "h": s["h"]})
        at += s["size"]
    shift = len(rects)
    for e in els:
        if e is not plate and e.get("frame0", 0):
            e["frame0"] = e["frame0"] + shift
    plate["n_frames"] = 1 + shift
    plate["delay"] = delay
    sc["gif_frames"] = plate["n_frames"]
    sc["bg_mode"], sc["bg_frames"] = 1, 0


def c4_mk_delta(els, sc, art, spr):
    c4_build_delta_bank(els, sc, art, spr, C4_DELTA_RECTS, 50)


def c4_mk_inst(els, sc, art, spr):
    """One seated plane, an instanced month window over the full domain of twelve."""
    c4_defaults(els, sc, art, spr)
    f0 = len(spr)
    c4_rect(80, 60, 0, 0, art, spr, b"\x44")
    els.append(c4_bank_row(els, 20, 0, C4_MONTH, f0, 12, flags=ts.EF_INSTANCE))


def c4_mk_comp_on_hand(els, sc, art, spr):
    """A comp hung on the hour hand (its row tag is the comp's parent): one link.
    Instanced so the one seated plane stands for all twelve month plates."""
    c4_defaults(els, sc, art, spr)
    f0 = len(spr)
    c4_rect(80, 60, 0, 0, art, spr, b"\x55")
    hand = next(e for e in els if e["class"] == ts.CLASS_HAND and e["slot"] == 0)
    els.append(c4_bank_row(els, 20, 0, C4_MONTH, f0, 12, flags=ts.EF_INSTANCE,
                           parent=hand["n"]))


def c4_mk_inst_v3(els, sc, art, spr):
    """An instance-flagged month window (the flag is the v4 bit) — emitted at v3."""
    c4_defaults(els, sc, art, spr)
    f0 = len(spr)
    c4_rect(80, 60, 0, 0, art, spr, b"\x44")
    els.append(c4_bank_row(els, 20, 0, C4_MONTH, f0, 12, flags=ts.EF_INSTANCE))


def c4_mk_rect_outside(els, sc, art, spr):
    """A delta plane wider than its plate box (500 wide on a 466 plate): the
    projection refuses a delta that is not a delta before it reaches the bank."""
    c4_build_delta_bank(els, sc, art, spr, [(0, 0, 500, 10)], 40)



def c4_battery(exe, base):
    # ---- honest flat v4 table: stride 25/6, appended columns at zero, no rects ----
    g = parsed(run(exe, "--dump3f", c4_forge(base, 4, c4_defaults))[1])
    ck(g["geom"] == {"abi_max": 4, "elem_words": 25, "spr_words": 6},
       "the section stamped %s, not the v4 25/6 the writer packed" % (g["geom"],))
    ck(g["nmat"] == 0 and g["rectrows"] == [],
       "a flat v4 table materialized %s rects, not zero" % (g["rectrows"],))
    ck(all(c[15] == 0 and c[16] == 0 for c in g["comps"]),
       "a flat v4 comp read parent/delay nonzero: %s" % (g["comps"],))
    print("  C4 flat v4 table: stride 25/6, zero appended columns, no rects")

    # ---- delta plate: accept, one rect row per extra plane at plate.xy + diff ----
    g = parsed(run(exe, "--dump3f", c4_forge(base, 4, c4_mk_delta))[1])
    ck(g["geom"]["abi_max"] == 4 and g["geom"]["elem_words"] == 25
       and g["geom"]["spr_words"] == 6,
       "the delta bank rode a v%s stamp at %s words, not 4 at 25/6"
       % (g["geom"]["abi_max"], g["geom"]["elem_words"]))
    ck(g["nmat"] == 3 and len(g["rectrows"]) == 3,
       "a 4-plane delta plate materialized %s rects, not three" % (g["nmat"],))
    got = [(r["x"], r["y"], r["w"], r["h"]) for r in g["rectrows"]]
    ck(got == [(0 + dx, 0 + dy, w, h) for dx, dy, w, h in C4_DELTA_RECTS],
       "the delta rects are %s, not the plate origin plus each diff %s"
       % (got, C4_DELTA_RECTS))
    ck(set(g["delay"].values()) == {50},
       "the plate's own 50 ms tempo did not reach the delay table: %s" % (g["delay"],))
    print("  C4 delta plate: 3 rects at plate.xy + diff, tempo honoured")

    # ---- instance flag: a month window over the fixed domain, frame_bytes 0 ----
    g = parsed(run(exe, "--dump3f", c4_forge(base, 4, c4_mk_inst))[1])
    inst = [c for c in g["comps"] if c[3] == C4_MONTH]
    ck(len(inst) == 1 and inst[0][11] == 0,
       "the instanced month window did not project with frame_bytes 0: %s" % (g["comps"],))
    print("  C4 instance: month window over 12 plates, frame_bytes 0")

    # ---- parent: a comp on the hour hand resolves to that row's index + 1 ----
    g = parsed(run(exe, "--dump3f", c4_forge(base, 4, c4_mk_comp_on_hand))[1])
    bank = [c for c in g["comps"] if c[3] == C4_MONTH]
    ck(len(bank) == 1 and bank[0][15] != 0,
       "the hung comp's parent_row did not resolve: %s" % (g["comps"],))
    print("  C4 parent: a comp resolves its rotator to a row index")

    # ---- refusals the writer emits and the projection refuses ----
    for what, mut, needle in (
            ("EF_INSTANCE under v3", c4_mk_inst_v3, "instance flag under geometry v3"),
            ("a delta outside its box", c4_mk_rect_outside, "outside its")):
        abi = 3 if mut is c4_mk_inst_v3 else 4
        rc, out, err = run(exe, "--refuse", c4_forge(base, abi, mut))
        ck(rc == 0 and needle in out,
           "%s was not refused in its own words (%r): %r" % (what, needle, out[:220]))

    # ---- the two parent refusals are writer-refused, so they reach the reader as a
    # damaged card: a forged word on an honest v4 table. The plate row is index 0;
    # its parent column is word 23 (0xFFFF = none, a legal value the tamper replaces).
    v4 = c4_forge(base, 4, c4_defaults)
    for what, par, needle in (
            ("a plate claiming a parent", 2, "names a parent"),
            ("a parent of 0", 0, "parent 0")):
        rc, out, err = run(exe, "--refuse", elem_tamper(v4, 0, 23, par))
        ck(rc == 0 and needle in out,
           "%s was not refused in its own words (%r): %r" % (what, needle, out[:240]))

    # ---- Decision A: a stride that disagrees with its stamp refuses at the gate ----
    lying = abi_lie(c4_forge(base, 3, c4_defaults), 4)   # 23-wide table, v4 stamp
    rc, out, err = run(exe, "--refuse", lying)
    ck(rc == 0 and "clock rows are" in out and "that stamp writes" in out,
       "a 23-wide table under a v4 stamp was not refused by the stride gate: %r"
       % out[:240])

    # ---- a tempo on a one-frame row: forged word, check_element refuses a written one ----
    rc, out, err = run(exe, "--refuse",
                       elem_tamper(c4_forge(base, 4, c4_defaults), 0, 24, 120))
    ck(rc == 0 and "one frame" in out and "tempo" in out,
       "a delay on a single-frame row was not refused: %r" % out[:240])
    print("  C4 refusals: plate parent, parent 0, instance under v3, delta outside box, "
          "stride/stamp, one-frame tempo")


if __name__ == "__main__":
    EXE, PAIR = main()
    refusals(EXE, PAIR)
    d32_battery(EXE, PAIR)
    s31_battery(EXE, PAIR)
    d34_battery(EXE, PAIR)
    c4_battery(EXE, PAIR)
    if "--mutations" in sys.argv:
        mutants(EXE, PAIR)
    print("%d checks, %d failures" % (checks[0], len(failures)))
    sys.exit(1 if failures else 0)
