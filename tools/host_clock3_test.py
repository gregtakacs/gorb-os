#!/usr/bin/env python3
"""host_clock3_test.py — the third leg of the FORMAT 3 reader's three-way rule.

§25.5(2) put the clock section's projection on the device: main/art/orb_clock3.c
turns an element table, a sprite catalogue and 26 carried scalars into the clk_theme_t
and the row table orb_stage.c draws. D42 made that table v6 and nothing older: every
hand, shadow, pointer and the carriage is one AUX row (docs/history/element-model-plan.md),
and the tempo the scheduler keeps is derived from the rows. This file checks the
projection the way the suite beside it checks everything — host gcc, the shipped C,
the writer's own bytes, no board, no card, no partition.

The golden vector is the compiled pair the firmware used to be built with
(tests/fixtures/avkit-pair.{h,bin}): its FORMAT 2 geometry is mapped to a bundle by
rebake_bundle.py and written as v6 rows by migrate_v6.py, its bank byte-identical to
the .bin. The projection must land every hand where the pair drew it, every shadow
where the pair cast it, and every pixel the pair addressed.

Refusal vectors run the other half of D26. Each forged table must draw the sentence
that names its own defect from the C, and the Python schema (theme_schema's
check_element / check_table / check_clock_rows, the writer's door) must refuse the
same table: two ends of one mapping that refuse differently are two mappings.

    python3 tools/host_clock3_test.py              # golden, batteries, refusals
    python3 tools/host_clock3_test.py --mutations   # and the C mutants
"""
import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import orbbundle as ob              # noqa: E402  (the writer, the second oracle)
import struct as _st                # noqa: E402  (the forged words)
import theme_schema as ts           # noqa: E402
import migrate_v6                   # noqa: E402

READER_C = os.path.join(ROOT, "main", "art", "orb_clock3.c")
BUNDLE_C = os.path.join(ROOT, "main", "art", "orb_bundle.c")
HDR_H = os.path.join(ROOT, "tests", "fixtures", "avkit-pair.h")
BIN_H = os.path.join(ROOT, "tests", "fixtures", "avkit-pair.bin")
REBAKED = "/tmp/b3/avkit.rebake.orbtheme"   # the §25.4 product bundle, if present

failures, checks = [], [0]


def check(ok, msg):
    checks[0] += 1
    if not ok:
        failures.append(msg)
        print("  FAIL " + msg[:1600])


def ck(cond, msg):
    check(bool(cond), msg)


# ---------------------------------------------------------------------------
# The oracle, read out of the pair's .h: the geometry every row must reproduce.
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
    """The integers printed after one comment tag, in the pair .h's own order. The
    count is an assert, so a re-flowed .h fails HERE rather than shifting fields."""
    m = re.search(r"/\* %s \*/\s*([-\d, x]+)" % re.escape(tag), HDR)
    if m is None:
        raise AssertionError("no `/* %s */` line in the pair .h" % tag)
    txt = m.group(1).split("/*")[0].strip().rstrip(",").strip()
    v = [int(x, 0) for x in re.split(r"[,\s]+", txt) if x]
    assert len(v) == count, "%s: %d values, wanted %d" % (tag, len(v), count)
    return v


def spr_rows(seg, want):
    rows = [list(map(int, m.groups())) for m in SPR_RE.finditer(seg)]
    assert len(rows) == want, "%d sprite rows, wanted %d" % (len(rows), want)
    return rows


def block(tag):
    """One `/* tag */ { ... }` sprite array's own rows."""
    open_tag = "/* %s */ {" % tag
    i = BODY.index(open_tag) + len(open_tag)
    eol = BODY.find("\n", i)
    if BODY[i:eol].strip():
        return "{" + BODY[i:eol]
    ends = [e for e in (BODY.find("\n  },", i), BODY.find("\n  }\n", i)) if e >= 0]
    return BODY[i:min(ends)]


def lst(pat):
    return [int(x) for x in re.search(pat, HDR).group(1).split(",")]


_spr = nums("sweep,railway,textOverHands,tickRate", 4)
_fps = nums("fps,loop,every", 3)
_trip = nums("bg_mode,bg_key_off,gif_frames", 3)
_tb = nums("tourbillon_n,tourbillon_fps", 2)
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
# A sprite row's fields, in the .h's (and the harness's) order.
SPR_F = ("w", "h", "offx", "offy", "pivotx", "pivoty", "centerx", "centery", "blend",
         "show", "color_off", "diffx", "diffy", "diffw", "diffh")


# ---------------------------------------------------------------------------
# The C harness. One binary, three modes, built from the shipped projection and the
# shipped bundle reader: dump a bundle's clock section, name the section a bundle
# does not carry, or report the projection's refusal. It includes orb_clock3.c
# rather than linking it so --mutations can drive a private copy of the source
# through the same main.
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
static clk_row_t          rows[CLK_ROW_MAX];   /* D42: sized as the device sizes it */
static clk_sound_t        snds[CLK_SND_SLOTS];
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
    io.rows = rows;   io.row_cap = CLK_ROW_MAX;      /* D42 */
    io.sounds = snds; io.sound_cap = CLK_SND_SLOTS;  /* D39 */
    r = orb_clock3_parse(s, &io);
    if (strcmp(argv[1], "--refuse") == 0) {
        printf("%s %s\n", r ? "refused" : "ACCEPTED", r ? r : "");
        return r ? 0 : 1;
    }
    if (r) { printf("refused %s\n", r); return 1; }

    printf("scalar bg %u\n", th.bg);
    printf("scalar second_sweep %u\n", th.second_sweep);
    printf("scalar tick_rate %u\n", th.tick_rate);
    printf("scalar bg_frames %u\n", th.bg_frames);
    printf("scalar bg_fps %d\n", th.bg_fps);
    printf("scalar bg_loop %u\n", th.bg_loop);
    printf("scalar bg_every %u\n", th.bg_every);
    printf("scalar plate_off %u\n", th.plate_off);
    printf("scalar bg_mode %u\n", th.bg_mode);
    printf("scalar bg_key_off %u\n", th.bg_key_off);
    printf("scalar gif_frames %u\n", th.gif_frames);
    printf("scalar plate_disc %u\n", th.plate_disc);
    printf("scalar overlay_disc %u\n", th.overlay_disc);
    printf("scalar tourbillon_n %u\n", th.tourbillon_n);
    printf("scalar tourbillon_fps %u\n", th.tourbillon_fps);
    printf("scalar car_row %u\n", th.car_row);
    printf("scalar pick_n");
    for (int i = 0; i < CLK_ROW_MAX; i++) printf(" %u", th.pick_n[i]);
    printf("\n");
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
    /* D42: every field of every row the projection seated, then its sprite. */
    printf("n_rows %u\n", io.out_n_rows);
    for (uint32_t i = 0; i < io.out_n_rows; i++) {
        const clk_row_t *w = &rows[i];
        printf("row %u %u %u %u %u %u %u %u %u %u %d %d %d %d %u %u %u %u %u\n", i,
               w->tag, w->shape, w->src, w->digit, w->end, w->cadence, w->cad_arg,
               w->ease, w->order, w->zero, w->span, w->dx, w->dy, w->value_min,
               w->value_max, w->flags, w->parent_row, w->delay_ms);
        dump_sprite("rowspr", (int)i, &w->spr);
    }
    printf("pattern %u\n", (unsigned)io.out_pattern);
    printf("values %u %u %u\n", (unsigned)io.out_values_off, (unsigned)io.out_values_len,
           (unsigned)io.out_values_ver);   /* radar-plan §2.11 */
    for (uint32_t i = 0; i < io.out_n_sounds; i++)
        printf("sound %u %u %u %u %u %u %u\n", (unsigned)snds[i].slot, (unsigned)snds[i].n,
               (unsigned)snds[i].pcm_off, (unsigned)snds[i].beat_chz,
               (unsigned)snds[i].first_beat, (unsigned)snds[i].flags, (unsigned)snds[i].enc);
    printf("rects %u\n", (unsigned)(th.bg_mode == 1 ? 1u : 0u));
    printf("rect %u %u %u %u %u\n", rects[0].x, rects[0].y, rects[0].w, rects[0].h,
           rects[0].off);
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


def scratch(prefix):
    """A temp dir removed at exit: each run writes ~170 MB of bundles under clock3f., and
    leaving them behind filled the 32 GB /tmp tmpfs (339 runs, 2026-11-05)."""
    d = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, d, True)
    return d


def build(mut=None, exe=None):
    """Compile the shipped projection (or one mutated copy) into the harness."""
    d = scratch("clock3.")
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
         "-I", os.path.join(ROOT, "main", "audio"),   # orb_adpcm.h: a sound's plane size
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


ROW_F = ("i", "tag", "shape", "src", "digit", "end", "cadence", "cad_arg", "ease",
         "order", "zero", "span", "dx", "dy", "vmin", "vmax", "flags", "parent_row",
         "delay_ms")


def parsed(out):
    """The harness's dump, as dicts."""
    sc, spr, first, delay = {}, {}, {}, {}
    rects, bgf, art, scr, cnt, nrect = None, None, None, None, None, None
    comps, bgl, rows = [], {}, []
    rectrows, geom, nmat = [], None, None
    sounds, pattern, values = [], None, None
    for line in out.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "scalar":
            sc[w[1]] = [int(x) for x in w[2:]]
        elif w[0] in ("n_comps", "n_rows"):
            pass
        elif w[0] == "geom":
            geom = dict(zip(("abi_max", "elem_words", "spr_words"),
                            (int(v) for v in w[1:])))
        elif w[0] == "rectrow":
            rectrows.append(dict(zip(("i", "x", "y", "w", "h", "off"),
                                     (int(v) for v in w[1:]))))
        elif w[0] == "nmat":
            nmat = int(w[1])
        elif w[0] == "sound":
            sounds.append(dict(zip(("slot", "n", "off", "beat", "first", "flags", "enc"),
                                   (int(v) for v in w[1:]))))
        elif w[0] == "pattern":
            pattern = int(w[1])
        elif w[0] == "values":
            values = tuple(int(v) for v in w[1:])
        elif w[0] == "row":
            rows.append(dict(zip(ROW_F, (int(v) for v in w[1:]))))
        elif w[0] == "rowspr":
            rows[int(w[1])]["spr"] = dict(zip(SPR_F, (int(v) for v in w[2:])))
        elif w[0] == "comp":
            comps.append([int(x) for x in w[1:]])
        elif w[0] == "tourbillon":
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
                rows=rows, rectrows=rectrows, geom=geom, nmat=nmat,
                sounds=sounds, pattern=pattern, values=values)


# The scalars v6 still carries, against the pair .h's values.
SCALARS = ("bg", "bg_frames", "bg_fps", "bg_loop", "bg_every", "plate_off",
           "bg_mode", "bg_key_off", "gif_frames")


def expected_rows():
    """The rows the pair's hands and shadows must project to, by migrate_v6's rules:
    each drawn hand slot in slot order, its shadow twin in the next slot when the pair
    casts shadows. A hand row's sprite is the pair's hand sprite verbatim; a twin
    lands its arbour (the hand's centre in its own art) on the pair's shadow centre."""
    out = []
    SRC = {0: ("hour12", 12), 1: ("minute", 60), 2: ("second", 60)}
    for k in range(5):
        hs = dict(zip(SPR_F, EXP["hand"][k]))
        if not hs["w"] or (EXP["matte_mask"] >> k) & 1:
            continue
        src, vmax = SRC.get(k, ("none", 0))
        out.append({"kind": "hand", "slot": k, "src": ts.SOURCES.index(src),
                    "vmax": vmax, "spr": hs})
        ss = dict(zip(SPR_F, EXP["shadow"][k]))
        if EXP["shadow_on"] and ss["w"] and k < 3:
            crop = (hs["centerx"] - ss["offx"], hs["centery"] - ss["offy"])
            out.append({"kind": "twin", "slot": k, "spr": dict(
                ss, pivotx=crop[0], pivoty=crop[1],
                offx=ss["centerx"] - crop[0], offy=ss["centery"] - crop[1])})
    return out


def compare_golden(p, where):
    """Every field the projection printed, against the pair."""
    for k in SCALARS:
        ck(p["sc"].get(k, [None])[0] == EXP[k],
           "%s: scalar %s is %s, the pair .h says %s" % (where, k, p["sc"].get(k), EXP[k]))
    sweep = p["sc"]["second_sweep"][0]
    ck(sweep == EXP["second_sweep"],
       "%s: the derived beat is sweep %d, the pair swept %d" % (where, sweep, EXP["second_sweep"]))
    if not EXP["second_sweep"]:
        ck(p["sc"]["tick_rate"][0] == max(1, EXP["tick_rate"]),
           "%s: the derived tick rate is %s, the pair stepped %d a second"
           % (where, p["sc"]["tick_rate"], EXP["tick_rate"]))
    ck(p["spr"].get("overlay") == EXP["overlay"],
       "%s: overlay is %s, the pair .h says %s" % (where, p["spr"].get("overlay"), EXP["overlay"]))
    want = expected_rows()
    rows = p["rows"]
    ck(len(rows) == len(want), "%s: %d rows projected, the pair draws %d"
       % (where, len(rows), len(want)))
    for r, w in zip(rows, want):
        if w["kind"] == "hand":
            ck(r["flags"] == 0 and r["src"] == w["src"],
               "%s: hand slot %d projected as %s" % (where, w["slot"], r))
            if w["vmax"]:
                ck(r["span"] == 3600 and r["vmax"] == w["vmax"] and r["end"] == ts.END_WRAP
                   and r["cadence"] != 0,
                   "%s: hand slot %d does not turn once over its domain: %s" % (where, w["slot"], r))
        else:
            ck(r["flags"] == 256 and r["src"] == 0 and r["cadence"] == 0,
               "%s: the shadow of slot %d is not a twin: %s" % (where, w["slot"], r))
        for f in ("w", "h", "offx", "offy", "pivotx", "pivoty", "centerx", "centery",
                  "show", "color_off"):
            ck(r["spr"][f] == w["spr"][f],
               "%s: %s %d's %s is %s, the pair drew %s"
               % (where, w["kind"], w["slot"], f, r["spr"][f], w["spr"][f]))
    return p


def pair_bundle(path):
    """A v6 clock bundle whose table came from the pair .h and whose bank IS the pair
    .bin: the FORMAT 2 container the pair describes, re-baked (rebake_bundle.py maps
    it to rows and writes them as v6)."""
    import orbtheme as ot
    import rebake_bundle as rbx
    f2 = path + ".f2"
    ot.write_container(f2, "av-kit", geom_blob(ART), ART, SCR_W, SCR_H)
    rbx.rebake(f2, path)
    return path


def geom_blob(art):
    """The FORMAT 2 geometry blob the pair .h describes, in orbtheme's order."""
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
        g += _st.pack("<I", v & 0xFFFFFFFF)
    for row in (EXP["hand"] + EXP["shadow"] + [EXP["overlay"]] + EXP["tourbillon"]):
        for v in row:
            g += _st.pack("<I", v & 0xFFFFFFFF)
    for r in EXP["rects"]:
        for v in r:
            g += _st.pack("<I", v & 0xFFFFFFFF)
    for name in ("first", "delays", "bgf"):
        for v in EXP[name]:
            g += _st.pack("<I", v & 0xFFFFFFFF)
    return bytes(g)


def main():
    exe = build()
    d = scratch("clock3f.")
    pair = pair_bundle(os.path.join(d, "pair.orbtheme"))
    golden = [("pair-forged bundle", pair)]
    if os.path.exists(REBAKED):
        # §25.4's product bundle is a second, independently written table: an older
        # stamp, so it reaches this reader through the migration like every theme
        # committed before D42.
        prod = os.path.join(d, "product.orbtheme")
        migrate_v6.migrate(REBAKED, prod)
        golden.append(("product bundle", prod))
    for where, path in golden:
        rc, out, err = run(exe, "--dump3f", path)
        ck(rc == 0, "%s projected cleanly (rc %d: %s%s)" % (where, rc, out[:300], err[:300]))
        if rc:
            continue
        p = compare_golden(parsed(out), where)
        ck(p["geom"] == {"abi_max": 6, "elem_words": 33, "spr_words": 6},
           "%s: the section reads as %s, not v6 33/6" % (where, p["geom"]))
        ck(p["nrect"] == len(EXP["rects"]) == 1,
           "the gif block carries %s rects where the pair carries %d"
           % (p["nrect"], len(EXP["rects"])))
        ck(p["rects"] == EXP["rects"][0],
           "the one rect is %s, the pair's is %s" % (p["rects"], EXP["rects"][0]))
        ck(p["bgf"] == EXP["bgf"][0], "the plate-frame row is %s, not the keyframe" % p["bgf"])
        ck(len(p["first"]) == len(EXP["first"]),
           "the index table has %d rows, the pair has %d" % (len(p["first"]), len(EXP["first"])))
        ck(len(p["delay"]) == len(EXP["delays"]),
           "the delay table has %d rows, the pair has %d" % (len(p["delay"]), len(EXP["delays"])))
        beat = (1000 + p["sc"]["tick_rate"][0] // 2) // max(1, p["sc"]["tick_rate"][0])
        ck(set(p["delay"].values()) == {beat},
           "the delays are not one uniform figure at the dial's beat (%d ms): %s"
           % (beat, sorted(set(p["delay"].values()))))
        hdr, secs = ob.read_bundle(path)
        sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
        ck(p["art"] == (sec["rec"]["art_off"], len(sec["art"])),
           "the projection named the bank at %s, the record says (%d, %d)"
           % (p["art"], sec["rec"]["art_off"], len(sec["art"])))
        ck(p["art"][1] == BLOB == len(ART),
           "the bank is %s B; the pair's blob is %d B" % (p["art"][1], len(ART)))
        if path == pair:
            ck(bytes(sec["art"]) == ART, "the pair-forged bundle's clock bank is not the .bin")
        ck(p["scr"] == (SCR_W, SCR_H), "%s: the panel pair is %s" % (where, p["scr"]))
    print("  golden: the pair projects to v6 rows where the pair drew its hands")
    return exe, pair


# ---------------------------------------------------------------------------
# Forging. `forge` writes through the writer's checks, as a bake would; `forge_raw`
# writes the same table with the writer's clock rules switched off, so a lie the
# writer refuses can still be put in front of the device — the hand-written or
# damaged card the projection's own guards exist for. `py_verdict` is the writer's
# answer on the same rows: None, or the sentence it refuses with.
# ---------------------------------------------------------------------------
def read_clock(base):
    hdr, secs = ob.read_bundle(base)
    sec = [s for s in secs if s["blob"]["slug"] == "clock"][0]
    return (hdr, sec, [dict(e) for e in sec["blob"]["elements"]],
            dict(sec["blob"]["scalars"]), [dict(s) for s in sec["blob"]["sprites"]],
            bytearray(sec["art"]))


def write_clock(out, name, els, sc, spr, art, abi=None, scr=(None, None)):
    abi = ob.GEOM_ABI6 if abi is None else abi
    ob.write_bundle(out, name,
                    [{"slug": "clock", "elements": els, "scalars": sc, "sprites": spr,
                      "art": bytes(art), "scr_w": scr[0] or SCR_W, "scr_h": scr[1] or SCR_H,
                      "abi_min": abi, "abi_max": abi}])
    return out


class _Unchecked:
    """The writer with its clock rules off: table, row and stamp checks no-ops."""
    def __enter__(self):
        self.saved = (ts.check_table, ts.check_clock_rows, ts.CLOCK_RETIRED_SCALARS,
                      ob.read_bundle)
        ts.check_table = lambda recs: recs
        ts.check_clock_rows = lambda recs, scalars=None: (0, 1)
        ts.CLOCK_RETIRED_SCALARS = ()
        return self

    def __exit__(self, *a):
        (ts.check_table, ts.check_clock_rows, ts.CLOCK_RETIRED_SCALARS,
         ob.read_bundle) = self.saved


def py_verdict(els, sc):
    """The writer's door on a table: None when it would pack, else its sentence."""
    try:
        ts.check_table(els)
        ts.check_clock_rows(els, sc)
        for n in ts.CLOCK_RETIRED_SCALARS:
            if int(sc.get(n, 0)):
                raise ValueError("clock scalar %r is %d: D42 retired it" % (n, sc[n]))
        return None
    except ValueError as e:
        return str(e)


def forge(base, mutate, raw=False, abi=None, scr=(None, None)):
    """The pair-forged section with mutate(els, sc, spr, art) applied, written back.
    Returns (path, the writer's verdict on the mutated table)."""
    hdr, sec, els, sc, spr, art = read_clock(base)
    if mutate:
        mutate(els, sc, spr, art)
    verdict = py_verdict(els, sc)
    out = "%s.%d.forged.orbtheme" % (base, forge.n)
    forge.n += 1
    if raw:
        with _Unchecked():
            write_clock(out, hdr["name"], els, sc, spr, art, abi, scr)
    else:
        write_clock(out, hdr["name"], els, sc, spr, art, abi, scr)
    return out, verdict


forge.n = 0


def rows_of(els):
    return [e for e in els if int(e["class"]) == ts.CLASS_AUX]


def hour_row(els):
    return [e for e in rows_of(els) if ts.SOURCES[int(e["src"])] == "hour12"][0]


def sec_row(els):
    return [e for e in rows_of(els) if ts.SOURCES[int(e["src"])] == "second"][0]


def add_plane(spr, art, w, h, fill=b"\x22", masked=True):
    off = len(art)
    size = w * h * (3 if masked else 2)
    art.extend(fill * size)
    spr.append({"off": off, "size": size, "w": w, "h": h, "diffx": 0, "diffy": 0})
    return len(spr) - 1


def add_values(els, spr, art, n=40, ver=3, plane=None, **kw):
    """radar-plan §2.11: a values row, slot 0, its plane n bytes by 1 (plane overrides
    the catalogue row's w, h and size)."""
    off = len(art)
    pl = dict({"w": n, "h": 1, "size": n}, **(plane or {}))
    art.extend(bytes(range(256)) * (pl["size"] // 256 + 1))
    del art[off + pl["size"]:]
    spr.append({"off": off, "size": pl["size"], "w": pl["w"], "h": pl["h"], "diffx": 0, "diffy": 0})
    e = dict(ts.blank_element(), **{"n": max(int(x["n"]) for x in els) + 1, "class": ts.CLASS_VALUES,
                                    "slot": 0, "frame0": len(spr) - 1, "n_frames": 1, "src": ver})
    e.update(kw)
    els.append(e)
    return e


def add_row(els, spr, art, slot, w=28, h=64, x=300, y=120, **kw):
    """One AUX row at `slot` on its own masked plane; kw overrides any column."""
    f0 = add_plane(spr, art, w, h)
    r = dict(ts.blank_element(), **{
        "n": max(int(e["n"]) for e in els) + 1, "class": ts.CLASS_AUX, "slot": slot,
        "order": 3, "alpha": 255, "frame0": f0, "n_frames": 1, "x": x, "y": y,
        "w": w, "h": h, "pivot_x": w // 2, "pivot_y": h - 8})
    r.update(kw)
    els.append(r)
    return r


def add_pick_row(els, spr, art, slot, n, sizes=None, w=28, h=64, **kw):
    """A row whose n frames the app picks (theme-pool-plan §2.6): n planes appended to
    the catalogue, no frame time. `sizes` gives each frame its own (w, h)."""
    f0 = len(spr)
    for i in range(n):
        fw, fh = sizes[i] if sizes else (w, h)
        add_plane(spr, art, fw, fh)
    return add_row(els, spr, art, slot, w=w, h=h, frame0=f0, n_frames=n, delay=0, **kw)


def add_font(els, spr, art, slot, w=8, h=14, n=len(ts.FONT_CHARSET), cover=False, sizes=None,
             keep=None, **kw):
    """A font (theme-text-plan §2.1): a TEXT row whose n frames are its glyph cells, each
    RGB565A8, or with cover=True its coverage alone (OS v1.2.12). `sizes` forges each
    glyph's byte count. `keep` (OS v1.2.17): the charset indices it carries, every other
    frame empty (0x0, 0 B)."""
    f0 = len(spr)
    for g in range(n):
        if keep is not None and g not in keep:
            spr.append({"off": len(art), "size": 0, "w": 0, "h": 0, "diffx": 0, "diffy": 0})
        elif sizes:
            off = len(art)
            art.extend(b"\x22" * sizes[g])
            spr.append({"off": off, "size": sizes[g], "w": w, "h": h, "diffx": 0, "diffy": 0})
        elif cover:
            off = len(art)
            art.extend(b"\x80" * (w * h))
            spr.append({"off": off, "size": w * h, "w": w, "h": h, "diffx": 0, "diffy": 0})
        else:
            add_plane(spr, art, w, h)
    r = dict(ts.blank_element(), **{
        "n": max(int(e["n"]) for e in els) + 1, "class": ts.CLASS_TEXT, "slot": slot,
        "frame0": f0, "n_frames": n, "w": w, "h": h, "pivot_x": 3, "pivot_y": 1})
    r.update(kw)
    els.append(r)
    return r


def add_label(els, spr, art, slot, **kw):
    """A label (§2.2): a text row reading the app's string in font 0."""
    cols = dict(w=120, h=24, x=100, y=20, pivot_x=60, pivot_y=12, kind=ts.SHAPE_TEXT,
                src=ts.SOURCES.index("text"), str=0xFF10, radius=4, digit=1, end=1)
    cols.update(kw)
    return add_row(els, spr, art, slot, **cols)


def _fonted(**label):
    """Two fonts and a label at the next free slot, the label's columns overridden."""
    def m(els, sc, spr, art):
        add_font(els, spr, art, 0)
        add_font(els, spr, art, 1, n=len(ts.FONT_ASCII))
        add_label(els, spr, art, next_slot(els), **label)
    return m


def next_slot(els):
    return max([int(e["slot"]) for e in rows_of(els)] + [-1]) + 1
from orbtheme import W_GEOM_OFF, W_GEOM_SIZE, W_GEOM_CRC, W_HDR_CRC
IDX_N_ELEM, IDX_GEOM_OFF, IDX_GEOM_SIZE, IDX_GEOM_CRC = 8, 12, 16, 20
IDX_SCR = 36
ELEM_WORDS_BYTES = ob._ts.ELEMENT_WORDS * 4   # the clock's v6 row: 33 words
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




def c4(fn):
    """The delta helpers take (els, sc, art, spr); forge hands (els, sc, spr, art)."""
    return lambda els, sc, spr, art: fn(els, sc, art, spr)


def elem_tamper(base, row, word, new):
    """One word of one element row, forged after the write and restamped."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    blob_off = _st.unpack_from("<I", buf, geom_off + 8 + IDX_GEOM_OFF)[0]
    at = geom_off + blob_off + BLOB_HEAD_BYTES + row * ELEM_WORDS_BYTES + word * 4
    buf[at:at + 4] = _st.pack("<I", new & 0xFFFFFFFF)
    _st.pack_into("<I", buf, 24, _regeom(buf))
    out = "%s.elem%d-%d.%d.orbtheme" % (base, row, word, elem_tamper.n)
    elem_tamper.n += 1
    open(out, "wb").write(bytes(buf))
    return out


elem_tamper.n = 0


def abi_lie(base, abi_max, header=None):
    """The clock section's own index-record stamp (and optionally the header's) forged
    after the write and restamped, so the file reaches the gate under test."""
    buf = bytearray(open(base, "rb").read())
    geom_off = _st.unpack_from("<I", buf, 4 * W_GEOM_OFF)[0]
    rec = geom_off + 8
    _st.pack_into("<H", buf, rec + 6, abi_max)
    _st.pack_into("<H", buf, rec + 4, abi_max)
    if header is not None:
        _st.pack_into("<I", buf, 8, header)          # the header's GEOM_ABI word
    _st.pack_into("<I", buf, 24, _regeom(buf))
    out = "%s.abi%d-%s.orbtheme" % (base, abi_max, header)
    open(out, "wb").write(bytes(buf))
    return out


# Element row words (theme_schema.ELEMENT_FIELDS order).
W = {n: i for i, (n, _t) in enumerate(ts.ELEMENT_FIELDS)}


def row_index(path, pred):
    """The element-table index of the first row `pred` accepts, in the written file."""
    _h, secs = ob.read_bundle(path) if not path.endswith("raw") else (None, None)
    els = secs[0]["blob"]["elements"]
    return [i for i, e in enumerate(els) if pred(e)][0]


# ---------------------------------------------------------------------------
# Refusals: one lie per vector, forged without the writer's clock rules, refused by
# the C in its own sentence; and the writer, asked about the same table, refuses it
# too (or the vector is the device's alone, py None, and says so).
# ---------------------------------------------------------------------------
def _plate(els):
    return [e for e in els if int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 0][0]


def _glass(els):
    return [e for e in els if int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 1][0]


def _set_row(pick, **kw):
    def m(els, sc, spr, art):
        pick(els).update(kw)
    return m


def _new_row(**kw):
    def m(els, sc, spr, art):
        add_row(els, spr, art, next_slot(els), **kw)
    return m


SWEEP_SEC = dict(src=ts.SOURCES.index("second"), span=3600, vmax=60,
                 cadence=ts.CAD_SWEEP, end=ts.END_WRAP)
def _two_values(e, s, p, a):
    add_values(e, p, a)
    add_values(e, p, a)


REFUSALS = (
    # (what, mutate, the C's needle, the writer's needle or None = the device's alone)
    # radar-plan §2.11: a theme's values
    ("values at slot 1", lambda e, s, p, a: add_values(e, p, a, slot=1), "one values blob, at slot 0",
     "one values blob"),
    ("values of version 0", lambda e, s, p, a: add_values(e, p, a, ver=0), "layout version 0",
     "layout version 0"),
    ("values that place", lambda e, s, p, a: add_values(e, p, a, w=8, h=1), "values place nothing",
     "values place nothing"),
    ("values of two planes", lambda e, s, p, a: add_values(e, p, a, n_frames=2), "one blob",
     "carries one frame"),
    ("values past 4096", lambda e, s, p, a: add_values(e, p, a, n=4097), "values are 1 to 4096 bytes", None),
    ("values two rows tall", lambda e, s, p, a: add_values(e, p, a, plane={"w": 20, "h": 2, "size": 40}),
     "values are 1 to 4096 bytes", None),
    ("two values rows", _two_values, "two rows for values slot 0", None),
    ("a font of one glyph", lambda e, s, p, a: e[0].update({"class": ts.CLASS_TEXT, "n_frames": 1}),
     "carries 1 glyphs", "ASCII whole"),
    ("a hand row", lambda e, s, p, a: hour_row(e).update({"class": ts.CLASS_HAND,
                                                          "src": 0, "kind": 0, "span": 0,
                                                          "vmax": 0, "cadence": 0,
                                                          "end": 0}),
     "every hand, shadow and carriage", None),
    ("a row at slot 64", lambda e, s, p, a: add_row(e, p, a, 64), "moving rows",
     "seats 64 moving rows"),
    ("a plate at slot 2", lambda e, s, p, a: _glass(e).update({"slot": 2}),
     "one plate and one overlay", None),
    ("no plate row", lambda e, s, p, a: e.remove(_plate(e)), "nothing to restore from", None),
    ("a moved plate", lambda e, s, p, a: _plate(e).update({"x": 4}),
     "not the whole panel is not a plate", None),
    ("a plate naming 3 frames", lambda e, s, p, a: _plate(e).update({"n_frames": 3}),
     "the row is the loop", None),
    ("an animated glass", lambda e, s, p, a: _glass(e).update({"n_frames": 2}),
     "animated glass", None),
    ("a one-shot plate bank with no tempo", lambda e, s, p, a: (
        _plate(e).update({"n_frames": 3}),
        s.update({"bg_mode": 0, "gif_frames": 0, "bg_frames": 2, "bg_loop": 0, "bg_fps": 0})),
     "no tempo", None),
    ("bg_frames 2", lambda e, s, p, a: s.update({"bg_frames": 2}), "does not carry", None),
    ("a retired tick_rate", lambda e, s, p, a: s.update({"tick_rate": 4}),
     "D42 retired it", "D42 retired it"),
    ("a retired order table", lambda e, s, p, a: s.update({"order_n": 3}),
     "D42 retired it", "D42 retired it"),
    # ---- the row's own columns (row_check / check_element) ----
    ("a shape past the vocabulary", _set_row(hour_row, kind=4), "is shape 4", "not a row's shape"),
    # D43: the drum row (a barrel the OS rolls), refused by the C and the writer alike
    ("a drum that turns", _set_row(hour_row, kind=ts.SHAPE_DRUM), "drum that turns", "a drum only rolls"),
    ("a drum with no radius", _set_row(hour_row, kind=ts.SHAPE_DRUM, span=0, zero=0, dx=0, dy=10, end=ts.END_WRAP),
     "radius 0", "radius 0"),
    ("a drum rolling less than its strip", _set_row(hour_row, kind=ts.SHAPE_DRUM, span=0, zero=0, dx=0, dy=10,
                                                    radius=20, end=ts.END_WRAP), "over a strip", "over a strip"),
    ("a drum that does not wrap", lambda e, s, p, a: hour_row(e).update({"kind": ts.SHAPE_DRUM, "span": 0, "zero": 0,
        "dx": 0, "dy": hour_row(e)["h"], "radius": 20, "end": ts.ENDS.index("hold")}),
     "does not wrap", "wrap"),
    ("a text shape", _set_row(hour_row, kind=ts.SHAPE_TEXT), "so it reads source text",
     "so it reads source text"),
    ("a string offset", _set_row(hour_row, str=4), "only a text row reads them",
     "only a text row names fonts"),
    ("a label that moves", _fonted(dx=6), "names a motion", "names no motion"),
    ("a label aligned past end", _fonted(digit=3), "aligns by", "aligns by"),
    ("a label with no tone 0", _fonted(str=0xFFFF), "no font for tone 0", "no font for tone 0"),
    ("a label naming an absent font", _fonted(str=0xFF20), "does not carry", "does not carry"),
    ("a label of two panels", _fonted(n_frames=2), "one panel", "one panel"),
    ("a label padded shut", _fonted(radius=12), "leaves no room", "leaves no room"),
    ("a font past the slots", lambda e, s, p, a: add_font(e, p, a, ts.FONT_SLOTS),
     "seats 15 fonts", "seats 15 fonts"),
    ("a font off the origin", lambda e, s, p, a: add_font(e, p, a, 0, x=5),
     "sits at 0,0", "sits at 0,0"),
    ("a font with no space", lambda e, s, p, a: add_font(e, p, a, 0, pivot_x=0),
     "a space of 0", "a space of 0"),
    ("a font of two forms", lambda e, s, p, a: add_font(
        e, p, a, 0, sizes=[8 * 14] + [8 * 14 * 3] * (len(ts.FONT_CHARSET) - 1)),
     "a font's cells are one shape", None),
    ("a glyph neither coverage nor colour", lambda e, s, p, a: add_font(
        e, p, a, 0, sizes=[8 * 14 * 2] * len(ts.FONT_CHARSET)),
     "of coverage or", None),
    # OS v1.2.17: a sparse font carries at least one glyph, all in one form
    ("a font that carries no glyph", lambda e, s, p, a: add_font(e, p, a, 0, cover=True, keep=set()),
     "carries no glyph", None),
    ("a sparse font of two forms", lambda e, s, p, a: add_font(
        e, p, a, 0, keep={16, 17}, sizes=[8 * 14] * 17 + [8 * 14 * 3] * (len(ts.FONT_CHARSET) - 17)),
     "a font's cells are one shape", None),
    ("a source the dial cannot read", _set_row(hour_row, src=ts.SOURCES.index("text")),
     "no such input", "no input of this dial"),
    ("degrees on a row", _set_row(hour_row, degrees=900), "angle at rest", "angle at rest"),
    ("a still row with a motion", _new_row(dx=4), "names a motion", "still row"),
    ("a moving row with no cadence", _set_row(hour_row, cadence=0), "names no cadence",
     "names when it moves"),
    ("an end rule past the vocabulary", _set_row(hour_row, end=3), "end rule 3",
     "not an end rule"),
    ("a digit past the vocabulary", _set_row(hour_row, digit=5), "reads digit 5",
     "not a digit selector"),
    ("a roll with an argument", _set_row(hour_row, cadence=6, cad_arg=2), "cadence argument",
     "takes no argument"),
    ("a cadence past the vocabulary", _set_row(hour_row, cadence=7), "names cadence 7",
     "not a cadence"),
    ("an empty domain", _set_row(hour_row, vmin=12), "at least one unit", "at least one unit"),
    ("a row that goes nowhere", _set_row(hour_row, span=0, end=ts.END_HOLD),
     "neither turns nor travels", "neither turns nor travels"),
    ("a pose past a turn", _set_row(hour_row, zero=3700), "within one turn", "within one turn"),
    ("a travel off the panel", _set_row(hour_row, dx=500, span=0, end=ts.END_FLYBACK),
     "panel", "panel"),
    ("a wrap that travels", _set_row(hour_row, dx=40), "a wrap needs a whole turn",
     "P1 must look like P0"),
    ("a wrap short of a turn", _set_row(hour_row, span=1800), "a wrap needs a whole turn",
     "P1 must look like P0"),
    ("a pivot off the plate", _set_row(hour_row, pivot_x=999), "outside its own",
     "outside its own"),
    ("a picked row past its frames",
     lambda e, s, p, a: add_pick_row(e, p, a, next_slot(e), 65, **SWEEP_SEC),
     "a row holds", "to pick from"),
    ("a picked frame of another size",
     lambda e, s, p, a: add_pick_row(e, p, a, next_slot(e), 2, sizes=[(28, 64), (30, 64)],
                                     **SWEEP_SEC),
     "describe different layers", None),
    ("a twin under a picked row",
     lambda e, s, p, a: (add_pick_row(e, p, a, next_slot(e), 2, **SWEEP_SEC),
                         add_row(e, p, a, next_slot(e), flags=ts.EF_AUX_SHADOW)),
     "the twin would shade", "shade one frame"),
    ("a twin with a motion of its own",
     _new_row(flags=ts.EF_AUX_SHADOW, **SWEEP_SEC), "a twin takes", "whole pose"),
    ("a step of 0", _set_row(sec_row, cadence=ts.CAD_STEP, cad_arg=0), "steps 0 times",
     "step 0"),
    ("an argument on a jump", _set_row(hour_row, cadence=ts.CAD_JUMP, cad_arg=3),
     "takes none", "takes no argument"),
    ("a sweep faster than the panel", _set_row(sec_row, cadence=ts.CAD_SWEEP, cad_arg=31),
     "at most", "30 at most"),
    ("stop-to-go on the hour", _set_row(hour_row, cadence=ts.CAD_STOP_TO_GO, cad_arg=585),
     "seconds hand's rhythm", "seconds hand's rhythm"),
    ("drag on the hour", _set_row(hour_row, cadence=ts.CAD_DRAG, cad_arg=120),
     "drag holds a calendar", "drag holds a calendar"),
    ("an ease past a second", _set_row(hour_row, cadence=ts.CAD_JUMP, ease=1500),
     "at most a second", "at most a second"),
    ("an ease on nothing", _set_row(hour_row, cadence=ts.CAD_SWEEP, ease=120),
     "nothing to ease", "nothing to ease"),
    ("a cadence on the plate", lambda e, s, p, a: _plate(e).update({"cadence": ts.CAD_JUMP}),
     "driven row's question", "driven row's question"),
    ("a pose on the plate", lambda e, s, p, a: _plate(e).update({"dx": 3}),
     "names a pose", "moving row's question"),
    # ---- the table's (check_clock_rows) ----
    ("a twin with no row before it",
     lambda e, s, p, a: add_row(e, p, a, next_slot(e) + 1, flags=ts.EF_AUX_SHADOW),
     "no row before it", "no row before it"),
    ("a twin under a twin",
     lambda e, s, p, a: (add_row(e, p, a, next_slot(e), flags=ts.EF_AUX_SHADOW),
                         add_row(e, p, a, next_slot(e), flags=ts.EF_AUX_SHADOW)),
     "under another twin", "under another twin"),
    ("two animated rows",
     lambda e, s, p, a: [add_row(e, p, a, next_slot(e), n_frames=1, delay=0)
                         for _ in range(2)] and [
         (r.update({"n_frames": 2, "delay": 80}),
          p.insert(int(r["frame0"]) + 1, dict(p[int(r["frame0"])])))
         for r in rows_of(e)[-2:]] and None,
     "one animated row", "one animated"),
    ("stop-to-go with no jumping minute",
     _set_row(sec_row, cadence=ts.CAD_STOP_TO_GO, cad_arg=585),
     "no minute row jumps", "no minute row jumps"),
    ("seconds rows that disagree",
     lambda e, s, p, a: (sec_row(e).update({"cadence": ts.CAD_STEP, "cad_arg": 4}),
                         add_row(e, p, a, next_slot(e), **dict(SWEEP_SEC, cadence=ts.CAD_STEP,
                                                               cad_arg=2))),
     "keeps one beat", "one beat"),
    ("a parent that is not a moving row",
     lambda e, s, p, a: hour_row(e).update({"parent": _glass(e)["n"]}),
     "not a moving row", "not a moving row"),
    ("a child off its turning parent's arbour",
     lambda e, s, p, a: add_row(e, p, a, next_slot(e), x=10, y=10, parent=hour_row(e)["n"],
                                **SWEEP_SEC),
     "shares its arbour", "shares its arbour"),
)


def refusals(exe, base):
    for what, mut, cneedle, pneedle in REFUSALS:
        # an animated-row vector shifts every later frame0 by the plane it inserted
        def mutate(els, sc, spr, art, mut=mut):
            n0 = len(spr)
            mut(els, sc, spr, art)
            if len(spr) > n0 and what in ("two animated rows",):
                _respread(els, spr, art)
        path, verdict = forge(base, mutate, raw=True)
        rc, out, err = run(exe, "--refuse", path)
        ck(rc == 0 and cneedle in out,
           "%s: the C said %r, not the sentence naming it (%r)" % (what, out[:240], cneedle))
        if pneedle is None:
            ck(True, "")
        else:
            ck(verdict is not None and pneedle in verdict,
               "%s: the writer said %r, missing %r" % (what, verdict, pneedle))
    print("  refusals: %d forgeries, the device and the writer agree" % len(REFUSALS))


def _respread(els, spr, art):
    """After a vector duplicated catalogue rows in place, re-lay the bank so every
    plane is contiguous again and every row's frame0 still names its own plane."""
    planes, at = [], 0
    for sp in spr:
        planes.append(bytes(art[sp["off"]:sp["off"] + sp["size"]]) if sp["off"] + sp["size"]
                      <= len(art) else b"\x22" * sp["size"])
    art[:] = b"".join(planes)
    for sp in spr:
        sp["off"] = at
        at += sp["size"]


# ---------------------------------------------------------------------------
# D42's accept side: rows the writer emits, projected column for column.
# ---------------------------------------------------------------------------
def rows_battery(exe, base):
    def several(els, sc, spr, art):
        s0 = next_slot(els)
        # a retrograde fan: hour12 from 210 over 120 degrees, flying back, eased
        add_row(els, spr, art, s0, src=ts.SOURCES.index("hour12"), span=1200, zero=2100,
                vmax=12, cadence=ts.CAD_JUMP, end=ts.END_FLYBACK, ease=200)
        # a sliding bar: 160 px across a minute, flying back
        add_row(els, spr, art, s0 + 1, w=40, h=8, x=100, y=300, pivot_x=20, pivot_y=4,
                src=ts.SOURCES.index("second"), dx=160, vmax=60, cadence=ts.CAD_SWEEP,
                end=ts.END_FLYBACK)
        # a date's units ring behind a window: 0..9, one turn backwards, jumping
        add_row(els, spr, art, s0 + 2, w=64, h=64, x=200, y=200, pivot_x=32, pivot_y=32,
                kind=ts.SHAPE_RING, src=ts.SOURCES.index("date"), digit=ts.DIGIT_UNITS,
                span=-3600, vmax=10, cadence=ts.CAD_JUMP, end=ts.END_WRAP)
        # a still bridge over everything, and its twin
        add_row(els, spr, art, s0 + 3, w=60, h=20, x=200, y=40, pivot_x=30, pivot_y=10,
                order=40)
        add_row(els, spr, art, s0 + 4, w=64, h=24, x=200, y=42, pivot_x=32, pivot_y=12,
                order=40, flags=ts.EF_AUX_SHADOW)
    path, verdict = forge(base, several)
    ck(verdict is None, "rows accept: the writer refused an honest table: %s" % verdict)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "rows accept: the device refused it: %r" % out[:300])
    if rc == 0:
        rows = parsed(out)["rows"]
        fan, bar, ring, still, twin = rows[-5:]
        ck((fan["src"], fan["span"], fan["zero"], fan["vmax"], fan["end"], fan["cadence"],
            fan["ease"]) == (ts.SOURCES.index("hour12"), 1200, 2100, 12, ts.END_FLYBACK,
                             ts.CAD_JUMP, 200),
           "rows: the fan projected as %s" % fan)
        ck((bar["dx"], bar["dy"], bar["span"], bar["end"]) == (160, 0, 0, ts.END_FLYBACK),
           "rows: the slide projected as %s" % bar)
        ck((ring["shape"], ring["digit"], ring["span"], ring["vmax"]) ==
           (ts.SHAPE_RING, ts.DIGIT_UNITS, -3600, 10), "rows: the ring projected as %s" % ring)
        ck(still["src"] == 0 and still["cadence"] == 0 and still["order"] == 40,
           "rows: the still bridge projected as %s" % still)
        ck(twin["flags"] == 256, "rows: the twin flag did not project: %s" % twin)
        ck(twin["spr"]["centerx"] == still["spr"]["centerx"] + 2,
           "rows: the twin's centre is not its row's centre plus the light: %s / %s"
           % (twin["spr"], still["spr"]))
    # The tempo is the rows': a sweep naming its rate, a step, and a sweep that rides
    # the plate's animation.
    for label, mut, want in (
            ("a sweep at 8", _set_row(sec_row, cadence=ts.CAD_SWEEP, cad_arg=8), (1, 8)),
            ("a step of 4", _set_row(sec_row, cadence=ts.CAD_STEP, cad_arg=4), (0, 4)),
            ("a step of 1", _set_row(sec_row, cadence=ts.CAD_STEP, cad_arg=1), (0, 1))):
        path, verdict = forge(base, mut)
        rc, out, err = run(exe, "--dump3f", path)
        g = parsed(out)
        got = (g["sc"]["second_sweep"][0], g["sc"]["tick_rate"][0])
        ck(rc == 0 and got == want, "tempo, %s: the device derived %s, not %s" % (label, got, want))
        _h, secs = ob.read_bundle(path)
        py = ts.clock_tempo(secs[0]["blob"]["elements"], secs[0]["blob"]["scalars"])
        ck(tuple(py) == want, "tempo, %s: the schema derived %s, not %s" % (label, py, want))
    # a stop-to-go second released by a jumping minute
    def station(els, sc, spr, art):
        [r for r in rows_of(els) if ts.SOURCES[int(r["src"])] == "minute"][0].update(
            cadence=ts.CAD_JUMP, cad_arg=0, ease=150)
        sec_row(els).update(cadence=ts.CAD_STOP_TO_GO, cad_arg=585)
    path, verdict = forge(base, station)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0 and verdict is None, "the station dial was refused: %r %r" % (out[:200], verdict))
    # the cap: slot 23 is the last legal row
    path, verdict = forge(base, _new_row(slot=23) if False else
                          (lambda e, s, p, a: add_row(e, p, a, ts.CLK_ROW_MAX - 1)))
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "a row at the last legal slot was refused: %r" % out[:200])
    # theme-text-plan §2: two fonts and a label reading them, projected as a text row
    # OS v1.2.12: the plate stored as the panel's disc: accepted and flagged; the same
    # size on a plate that is not the panel's is refused
    def disc_plate(els, sc, spr, art):
        p = [e for e in els if int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 0][0]
        sp = spr[int(p["frame0"])]
        sp["size"] = ts.disc_bytes()
        _respread(els, spr, art)
        sc["plate_off"] = sp["off"]
    path, verdict = forge(base, disc_plate)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0 and parsed(out)["sc"].get("plate_disc") == [1],
       "a disc plate was refused or not flagged: %r" % out[:240])
    plain = parsed(run(exe, "--dump3f", forge(base, None)[0])[1])
    ck(plain["sc"].get("plate_disc") == [0], "a square plate was flagged as a disc")
    def disc_short(els, sc, spr, art):
        disc_plate(els, sc, spr, art)
        p = [e for e in els if int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 0][0]
        spr[int(p["frame0"])]["size"] = ts.disc_bytes() - 2
        _respread(els, spr, art)
    path, verdict = forge(base, disc_short)
    rc, out, err = run(exe, "--refuse", path)
    ck(rc == 0 and "claims" in out, "a plate a pixel short of the disc was not refused: %r" % out[:240])

    # OS v1.2.12: the glass overlay as a masked disc, and the preview as its own disc,
    # when the base carries them: accepted, the glass flagged
    def disc_glass(els, sc, spr, art):
        for e in els:
            c, sl = int(e["class"]), int(e["slot"])
            sp = spr[int(e["frame0"])]
            if c == ts.CLASS_PLATE and sl == 1:      # the pair's glass, made the panel's
                e.update(x=0, y=0, w=ts.DISC_SIDE, h=ts.DISC_SIDE)
                sp.update(w=ts.DISC_SIDE, h=ts.DISC_SIDE, size=ts.disc_px() * 3)
            elif c == ts.CLASS_PREVIEW and sp["w"] == sp["h"]:
                sp["size"] = ts.disc_px(sp["w"]) * 2
        _respread(els, spr, art)
    _h, _secs = ob.read_bundle(base)
    _b = _secs[0]["blob"]
    has_glass = any(int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 1 for e in _b["elements"])
    if has_glass:
        path, verdict = forge(base, disc_glass)
        rc, out, err = run(exe, "--dump3f", path)
        ck(rc == 0 and parsed(out)["sc"].get("overlay_disc") == [1],
           "a disc glass was refused or not flagged: %r / %r" % (
               parsed(out)["sc"].get("overlay_disc") if rc == 0 else None,
               run(exe, "--refuse", path)[1][:240]))
    else:
        print("  (the base carries no glass overlay: the disc glass rides the editor's suites)")

    # OS v1.2.12: a coverage font with its colours, and a second font row in another
    # colour naming the same glyph frames, both accepted
    def covered(els, sc, spr, art):
        f = add_font(els, spr, art, 0, cover=True)
        ts.font_colours(f, "#39FF14", "#B8892E")
        twin = dict(f, n=max(int(e["n"]) for e in els) + 1, slot=1)
        ts.font_colours(twin, "#8A9A90")
        els.append(twin)
        add_label(els, spr, art, next_slot(els), str=0xFF10)
    path, verdict = forge(base, covered)
    rc, out, err = run(exe, "--refuse", path)
    ck("refused" not in out and verdict is None,
       "a coverage font and its colour twin were refused: %r %r" % (out[:240], verdict))
    # OS v1.2.17: a sparse font, a temperature's digits, the minus and the degree sign
    # alone, every other frame empty
    def digits(els, sc, spr, art):
        keep = {ts.FONT_CHARSET.index(c) for c in "0123456789-\u00b0"}
        add_font(els, spr, art, 0, cover=True, keep=keep)
        add_label(els, spr, art, next_slot(els), str=0xFFF0)
    path, verdict = forge(base, digits)
    rc, out, err = run(exe, "--refuse", path)
    ck("refused" not in out and verdict is None,
       "a sparse font of digits was refused: %r %r" % (out[:240], verdict))
    path, verdict = forge(base, _fonted())
    ck(verdict is None, "fonts accept: the writer refused an honest table: %s" % verdict)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "fonts accept: the device refused it: %r" % out[:300])
    if rc == 0:
        lab = parsed(out)["rows"][-1]
        ck((lab["shape"], lab["src"], lab["digit"], lab["end"]) ==
           (ts.SHAPE_TEXT, ts.SOURCES.index("text"), 1, 1), "fonts: the label projected as %s" % lab)
    print("  rows: a fan, a slide, a digit ring, a still bridge and its twin; the tempo "
          "the rows ask for; two fonts and a label")


# ---------------------------------------------------------------------------
# D32 and §31's accept sides, at v6: a carriage (the one animated row) and a month
# window, then a keyframe plate bank.
# ---------------------------------------------------------------------------
CAR_W, CAR_H = 64, 64
WIN_W, WIN_H = 40, 30
CAR_PLANE = CAR_W * CAR_H * 3
WIN_PLANE = WIN_W * WIN_H * 3


def d32_battery(exe, base):
    def both(els, sc, spr, art):
        f0 = len(spr)
        for _ in range(3):
            add_plane(spr, art, CAR_W, CAR_H, b"\x11")
        els.append(dict(ts.blank_element(), **{
            "n": 20, "class": ts.CLASS_AUX, "slot": next_slot(els), "order": 1,
            "alpha": 255, "frame0": f0, "n_frames": 3, "delay": 83, "x": 120, "y": 120,
            "w": CAR_W, "h": CAR_H, "pivot_x": 32, "pivot_y": 32,
            "src": ts.SOURCES.index("second"), "span": 3600, "zero": 900, "vmax": 60,
            "cadence": ts.CAD_SWEEP, "end": ts.END_WRAP}))
        w0 = len(spr)
        for _ in range(12):
            add_plane(spr, art, WIN_W, WIN_H, b"\x11")
        els.append(dict(ts.blank_element(), **{
            "n": 21, "class": ts.CLASS_BANK, "slot": 0, "order": 3, "alpha": 255,
            "frame0": w0, "n_frames": 12, "x": 60, "y": 200, "w": WIN_W, "h": WIN_H,
            "scale16": 16, "kind": 0, "src": ts.SOURCES.index("month")}))
    path, verdict = forge(base, both)
    ck(verdict is None, "D32: the writer refused the carriage and window: %s" % verdict)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "D32: the carriage-and-window bundle was refused: %s" % out[:300])
    if rc:
        return
    p = parsed(out)
    art_len = len(ART)
    car_off, win_off = art_len, art_len + 3 * CAR_PLANE
    ck(p["sc"]["tourbillon_n"] == [3] and p["sc"]["tourbillon_fps"] == [12],
       "D32: the carriage derived %s frames @ %s fps, not 3 @ 12"
       % (p["sc"]["tourbillon_n"], p["sc"]["tourbillon_fps"]))
    car = p["rows"][p["sc"]["car_row"][0]]
    ck(car["zero"] == 900 and car["delay_ms"] == 83 and car["src"] == ts.SOURCES.index("second"),
       "D32: the carriage row projected as %s" % car)
    tb = p["spr"]["tourbillon"]
    ck(tb[0][9] == 1 and tb[0][10] == car_off and tb[1][10] == car_off + CAR_PLANE and
       tb[2][10] == car_off + 2 * CAR_PLANE,
       "D32: the bank does not stride the catalogue: %s" % [tb[i][10] for i in range(3)])
    ck(tb[0][0] == CAR_W and tb[0][6] == 152 and tb[0][7] == 152,
       "D32: the bank's geometry is not the carriage row's own")
    ck(tb[3][9] == 0 and tb[3][0] == 0, "D32: the bank ran past its frames into the array")
    ck(len(p["comps"]) == 1, "D32: the month window did not project alone")
    c = p["comps"][0]
    ck(c[1:] == [0, 0, 2, 60, 200, WIN_W, WIN_H, 12, 3, win_off, WIN_PLANE, 0, 16, 1, 0, 0],
       "D32: the window projected as %s" % (c,))
    # the device's own guards, past the writer's door: one word each, restamped
    wi = [i for i, e in enumerate(ob.read_bundle(path)[1][0]["blob"]["elements"])
          if int(e["class"]) == ts.CLASS_BANK][0]
    for label, word, val, needle in (
            ("battery source", W["src"], 9, "no such input"),
            ("a month of three plates", W["n_frames"], 3, "indexes 12 plates"),
            ("a strip kind", W["kind"], 1, "glyph"),
            ("depth 64", W["order"], 64, "rungs")):
        rc, out, err = run(exe, "--refuse", elem_tamper(path, wi, word, val))
        ck(rc == 0 and needle in out, "D32 %s: the device said %r, not %r"
           % (label, out[:160], needle))
    print("  D32 battery: the carriage as the animated row, the month window, the guards")


KF_PLANE = SCR_W * SCR_H * 2         # one colour-only keyframe: 434,312 B


def s31_mutate(n=3, more=None):
    def m(els, sc, spr, art):
        k = n - 1
        art[KF_PLANE:KF_PLANE] = b"\x11" * (KF_PLANE * k)     # the keyframes, plate-first
        for i in range(1, len(spr)):
            spr[i]["off"] += KF_PLANE * k
        spr[1:1] = [{"off": KF_PLANE * (j + 1), "size": KF_PLANE, "w": SCR_W, "h": SCR_H,
                     "diffx": 0, "diffy": 0} for j in range(k)]
        for e in els:
            if not (int(e["class"]) == ts.CLASS_PLATE and int(e["slot"]) == 0):
                if int(e["n_frames"]) or int(e["frame0"]):
                    e["frame0"] = int(e["frame0"]) + k
            else:
                e["n_frames"] = n
                e["delay"] = 0
                e["flags"] = int(e["flags"]) & ~ts.EF_DELTA_WRAP
        sc.update({"bg_mode": 0, "gif_frames": 0, "bg_frames": k, "bg_fps": 6,
                   "bg_loop": 1, "bg_every": 1, "plate_off": 0, "bg_key_off": 0})
        if more:
            more(els, sc, spr, art)
    return m


def s31_battery(exe, base):
    path, verdict = forge(base, s31_mutate(3))
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "the device refused a keyframe plate bank: %s" % (out or err)[:200])
    if rc == 0:
        p = parsed(out)
        ck(p["sc"]["bg_frames"] == [2], "§31: the bank length is %s" % p["sc"].get("bg_frames"))
        ck(p["bgl"] == {0: KF_PLANE, 1: 2 * KF_PLANE},
           "§31: the plate-frame table is %s, not the extra planes' offsets" % p["bgl"])
        ck(p["art"][1] == BLOB + 2 * KF_PLANE, "§31: the bank is %s B" % p["art"][1])
    for label, mut, needle in (
            ("a scalar disagreeing with the row",
             lambda e, s, r, a: s.update({"bg_frames": 3}), "animation's length"),
            ("delta mode claiming whole planes",
             lambda e, s, r, a: s.update({"bg_mode": 1}), "does not carry"),
            ("a bank that starts off the plate",
             lambda e, s, r, a: s.update({"plate_off": KF_PLANE}), "frame 0 of the loop"),
            ("a keyframe carrying an alpha stride",
             lambda e, s, r, a: r[1].update({"size": r[1]["size"] + SCR_W * SCR_H}),
             "that extent is"),
            ("keyframes shuffled out of bank order",
             lambda e, s, r, a: r.__setitem__(slice(1, 3), r[1:3][::-1]), "not contiguous")):
        t, _v = forge(base, s31_mutate(3, mut), raw=True)
        rc, out, err = run(exe, "--refuse", t)
        ck(rc == 0 and needle in out, "§31 %s: the device said %r, not %r"
           % (label, (out or err)[:160], needle))
    print("  §31 battery: keyframe bank accepted, 5 bank lies refused")


C4_MONTH = ts.SOURCES.index("month")     # domain 12, the instanced window's value
C4_WINDOW = ts.KINDS.index("window")
C4_SCR = (SCR_W, SCR_H)                  # the panel the plate must fill
C4_PLATE_BYTES = SCR_W * SCR_H * 2       # a colour-only plate plane (RGB565)


def c4_defaults(els, sc, art, spr):
    """Honest v6 base: the appended columns ride at their defaults, so every row
    reads parent NONE and delay 0 unless it names otherwise — the base every
    accept and refusal vector starts from."""
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


def c4_build_delta_bank(els, sc, art, spr, rects, delay, wrap=None):
    """Rebuild the bank so the plate's frames lead it: one whole colour-only plane
    at offset 0, then one colour-only rect (w*h*2) per delta frame. The pair's other
    planes (overlay, hands, shadows — RGB888) shift back by the rect count and every
    non-plate row's frame0 follows. bg_frames stays 0 (the row IS the loop) and the
    row carries its own tempo, so the one-shot guard passes on truth rather than on
    bg_fps. `rects` is [(dx, dy, w, h)] and each rect must fit the plate box.
    `wrap`, when given, is §37's closing rect: one extra colour-only sprite past the
    plate's frame window, claimed by the row's EF_DELTA_WRAP bit.
    A `rects` entry of None is a still in the loop: a zero-size row that ships no
    pixels and applies nothing — §37 teaches the loader to accept it where C5's
    contract always promised it."""
    c4_defaults(els, sc, art, spr)
    plate = next(e for e in els if e["class"] == ts.CLASS_PLATE and e["slot"] == 0)
    scr_w, scr_h = C4_SCR
    whole = bytes(art[0:scr_w * scr_h * 2])         # the pair's own plate pixels
    survivors = []                                  # the RGB888 planes that stay
    for sp in spr[1:]:
        survivors.append({"size": sp["size"], "w": sp["w"], "h": sp["h"],
                          "bytes": bytes(art[sp["off"]:sp["off"] + sp["size"]])})
    art[:] = whole
    for rect in rects:
        if rect is not None:
            art.extend(b"\x22" * (rect[2] * rect[3] * 2))   # colour-only, like the plate
    if wrap:
        art.extend(b"\x33" * (wrap[2] * wrap[3] * 2))
    for s in survivors:
        art.extend(s["bytes"])
    del spr[:]
    spr.append({"off": 0, "size": scr_w * scr_h * 2, "w": scr_w, "h": scr_h,
                "diffx": 0, "diffy": 0})
    at = scr_w * scr_h * 2
    for rect in rects:
        if rect is None:                            # a still: zero size, applies nothing
            spr.append({"off": at, "size": 0, "w": 0, "h": 0, "diffx": 0, "diffy": 0})
            continue
        dx, dy, w, h = rect
        spr.append({"off": at, "size": w * h * 2, "w": w, "h": h,
                    "diffx": dx, "diffy": dy})
        at += w * h * 2
    if wrap:
        wdx, wdy, ww, wh = wrap
        spr.append({"off": at, "size": ww * wh * 2, "w": ww, "h": wh,
                    "diffx": wdx, "diffy": wdy})
        at += ww * wh * 2
    for s in survivors:                             # offsets continue the plate block
        spr.append({"off": at, "size": s["size"], "w": s["w"], "h": s["h"]})
        at += s["size"]
    shift = len(rects) + (1 if wrap else 0)
    for e in els:
        if e is not plate and e.get("frame0", 0):
            e["frame0"] = e["frame0"] + shift
    plate["n_frames"] = 1 + len(rects)
    plate["delay"] = delay
    if not delay:
        sc["bg_fps"] = 15     # tempo off the scalar when the row names none — the v3
                              # door below has no column for a row tempo at all
    if wrap:
        plate["flags"] = plate.get("flags", 0) | ts.EF_DELTA_WRAP
    sc["gif_frames"] = plate["n_frames"]
    sc["bg_mode"], sc["bg_frames"] = 1, 0


def c4_mk_delta(els, sc, art, spr):
    c4_build_delta_bank(els, sc, art, spr, C4_DELTA_RECTS, 50)


C4_WRAP = (300, 300, 48, 48)


def c4_mk_delta_wrap(els, sc, art, spr):
    """§37: the honest shape — the loop's four rects plus the closing delta one
    row past the plate window, with the bit the row pays for them."""
    c4_build_delta_bank(els, sc, art, spr, C4_DELTA_RECTS, 50, wrap=C4_WRAP)


def c4_mk_wrap_no_row(els, sc, art, spr):
    """The lie rowDelta never ships: the bit claimed with no closing sprite after
    the window. The plate gate refuses it before the walk ever counts rows."""
    c4_build_delta_bank(els, sc, art, spr, C4_DELTA_RECTS, 50)
    plate = next(e for e in els if e["class"] == ts.CLASS_PLATE and e["slot"] == 0)
    plate["flags"] = plate.get("flags", 0) | ts.EF_DELTA_WRAP


def c4_mk_wrap_outside(els, sc, art, spr):
    """A closing rect that overhangs the plate (400+100 on a 466 panel): the delta
    must sit inside the plate it closes back to, or it paints beside it forever."""
    c4_build_delta_bank(els, sc, art, spr, C4_DELTA_RECTS, 50, wrap=(400, 400, 100, 100))


def c4_mk_wrap_on_hand(els, sc, art, spr):
    """The closing bit forged onto a moving row: the loop it names is the plate's
    background, and a hand has none."""
    c4_defaults(els, sc, art, spr)
    hand = hour_row(els)
    hand["flags"] = hand.get("flags", 0) | ts.EF_DELTA_WRAP


def c4_mk_delta_wide(els, sc, art, spr):
    """A 21-frame delta bank: inside the gif table's own cap, past the dial's
    invalid-area table (CLK_BG_MAX, 20: one second of the fastest gears) — the
    materialization would drop rects and repaint stale texels, so the loader
    refuses the bank to a wider dial."""
    c4_build_delta_bank(els, sc, art, spr,
                        [(4, 4, 8, 8)] * 20 + [(8, 8, 8, 8)], 50)


def c4_mk_delta_still(els, sc, art, spr):
    """A still mid-loop (C5 §7.2's contract, §37's loader fix): the row exists at
    zero extent, ships no pixels, and the device applies nothing that tick."""
    c4_build_delta_bank(els, sc, art, spr, [None, (200, 30, 32, 48)], 50)


def c4_mk_inst(els, sc, art, spr):
    """One seated plane, an instanced month window over the full domain of twelve."""
    c4_defaults(els, sc, art, spr)
    f0 = len(spr)
    c4_rect(80, 60, 0, 0, art, spr, b"\x44")
    els.append(c4_bank_row(els, 40, 0, C4_MONTH, f0, 12, flags=ts.EF_INSTANCE))


def c4_mk_comp_on_hand(els, sc, art, spr):
    """A comp hung on the hour hand (its row tag is the comp's parent): one link.
    Instanced so the one seated plane stands for all twelve month plates."""
    c4_defaults(els, sc, art, spr)
    f0 = len(spr)
    c4_rect(80, 60, 0, 0, art, spr, b"\x55")
    hand = hour_row(els)
    els.append(c4_bank_row(els, 40, 0, C4_MONTH, f0, 12, flags=ts.EF_INSTANCE,
                           parent=hand["n"]))


def c4_mk_rect_outside(els, sc, art, spr):
    """A delta plane wider than its plate box (500 wide on a 466 plate): the
    projection refuses a delta that is not a delta before it reaches the bank."""
    c4_build_delta_bank(els, sc, art, spr, [(0, 0, 500, 10)], 40)



def c4_mk_preview(els, sc, art, spr):
    """§37's preview row as the shipped bakes write it: one class 9 plane, square,
    one frame, at the bank's running end — colour-only like the plate, because its
    class price is RGB565 alone and the third byte would be a plane of 255s no
    picker consults (orb_asset_slot_bytes names the exception; the projection's
    walk must price it the same way or the bake is refused at the install door)."""
    c4_defaults(els, sc, art, spr)
    at = len(art)
    art.extend(b"\x44" * (C4_PREVIEW * C4_PREVIEW * 2))          # colour only
    spr.append({"off": at, "size": C4_PREVIEW * C4_PREVIEW * 2,
                "w": C4_PREVIEW, "h": C4_PREVIEW, "diffx": 0, "diffy": 0})
    els.append(dict(els[0], n=max(e["n"] for e in els) + 1, slot=0,
                    flags=0, delay=0, parent=ts.PARENT_NONE,
                    **{"class": ts.CLASS_PREVIEW},
                    x=8, y=8, w=C4_PREVIEW, h=C4_PREVIEW,
                    frame0=len(spr) - 1, n_frames=1))


def c4_mk_preview_lie(els, sc, art, spr):
    """The same row claiming the RGB888 width: a masked preview is either a torn
    bank or a baker that forgot the exception, and the walk owes it the extent
    sentence rather than a drawn plane of invented alphas."""
    c4_defaults(els, sc, art, spr)
    at = len(art)
    art.extend(b"\x44" * (C4_PREVIEW * C4_PREVIEW * 3))          # colour + alpha
    spr.append({"off": at, "size": C4_PREVIEW * C4_PREVIEW * 3,
                "w": C4_PREVIEW, "h": C4_PREVIEW, "diffx": 0, "diffy": 0})
    els.append(dict(els[0], n=max(e["n"] for e in els) + 1, slot=0,
                    flags=0, delay=0, parent=ts.PARENT_NONE,
                    **{"class": ts.CLASS_PREVIEW},
                    x=8, y=8, w=C4_PREVIEW, h=C4_PREVIEW,
                    frame0=len(spr) - 1, n_frames=1))


C4_PREVIEW = 64     # a square at a quarter the panel: legal under the baker's cap


def d39_odd_tail(els, sc, art, spr):
    """A chime on an odd offset: a 1x1 pixel plane (three bytes) at the bank's tail
    when it is even, then the sound after it, with its row — the placement a writer
    that appended instead of putting sounds at the head would produce."""
    if len(art) % 2 == 0:
        spr.append({"off": len(art), "size": 3, "w": 1, "h": 1, "diffx": 0, "diffy": 0})
        art.extend(b"\x00\x00\x00")
    n = 2205
    spr.append({"off": len(art), "size": 2 * n, "w": n, "h": 1, "diffx": 0, "diffy": 0})
    art.extend(b"\x01\x02" * n)
    els.append(ts.sound_row("chime_hour", len(spr) - 1, "none",
                            n=max(int(e["n"]) for e in els) + 1))


def c4_forge_rows(path):
    _h, secs = ob.read_bundle(path)
    return [dict(e) for e in secs[0]["blob"]["elements"]]




# ---------------------------------------------------------------------------
# C4's delta dialect, which v6 carries whole: the delta plate, §37's closing rect,
# the still row, the preview plane, the instance flag, a comp hung on a row — and the
# stamp gate, which takes v6 and nothing older (D42).
# ---------------------------------------------------------------------------
def c4_battery(exe, base):
    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_defaults))[0])[1])
    ck(g["geom"] == {"abi_max": 6, "elem_words": 33, "spr_words": 6},
       "the section stamped %s, not the v6 33/6 the writer packed" % (g["geom"],))
    ck(g["nmat"] == 0 and g["rectrows"] == [], "a flat table materialized %s rects" % (g["rectrows"],))

    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_mk_delta))[0])[1])
    ck(g["nmat"] == 3 and len(g["rectrows"]) == 3,
       "a 4-plane delta plate materialized %s rects, not three" % (g["nmat"],))
    got = [(r["x"], r["y"], r["w"], r["h"]) for r in g["rectrows"]]
    ck(got == [(dx, dy, w, h) for dx, dy, w, h in C4_DELTA_RECTS],
       "the delta rects are %s, not the plate origin plus each diff" % (got,))
    ck(set(g["delay"].values()) == {50}, "the plate's 50 ms tempo did not reach the delays")

    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_mk_delta_wrap))[0])[1])
    got = [(r["x"], r["y"], r["w"], r["h"]) for r in g["rectrows"]]
    ck(g["nmat"] == 4 and got == [C4_WRAP] + [tuple(r) for r in C4_DELTA_RECTS],
       "§37: the closing rect is not frame 0's: %s" % (got,))

    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_mk_delta_still))[0])[1])
    ck(g["nmat"] == 2 and g["rectrows"][0]["w"] == 0 and g["rectrows"][1]["x"] == 200,
       "C5: the still row did not land as a zero rect before the real one: %s" % (g["rectrows"],))

    rc, out, err = run(exe, "--dump3f", forge(base, c4(c4_mk_preview))[0])
    ck(rc == 0, "a shipped-shape preview row was refused: %r" % (out + err)[:280])
    rc, out, err = run(exe, "--refuse", forge(base, c4(c4_mk_preview_lie), raw=True)[0])
    ck(rc == 0 and "that extent is" in out, "a masked preview was not refused: %r" % out[:280])

    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_mk_inst))[0])[1])
    inst = [c for c in g["comps"] if c[3] == C4_MONTH]
    ck(len(inst) == 1 and inst[0][11] == 0,
       "the instanced month window did not project with frame_bytes 0: %s" % (g["comps"],))
    g = parsed(run(exe, "--dump3f", forge(base, c4(c4_mk_comp_on_hand))[0])[1])
    bank = [c for c in g["comps"] if c[3] == C4_MONTH]
    ck(len(bank) == 1 and bank[0][15] != 0, "the hung comp's parent did not resolve: %s" % (g["comps"],))

    for what, mut, needle in (
            ("a delta outside its box", c4_mk_rect_outside, "outside its"),
            ("a closing delta with no row behind it", c4_mk_wrap_no_row,
             "the one row past the plate must BE the closing rect"),
            ("a closing rect overhanging its plate", c4_mk_wrap_outside,
             "must sit inside the plate"),
            ("a delta loop wider than the dial's rect table", c4_mk_delta_wide,
             "invalid-area table holds 20"),
            ("the closing bit on a moving row", c4_mk_wrap_on_hand, "closing-delta bit")):
        rc, out, err = run(exe, "--refuse", forge(base, c4(mut), raw=True)[0])
        ck(rc == 0 and needle in out, "%s was not refused in its own words (%r): %r"
           % (what, needle, out[:220]))

    honest = forge(base, c4(c4_defaults))[0]
    for what, par, needle in (("a plate claiming a parent", 2, "names a parent"),
                              ("a parent of 0", 0, "parent 0")):
        rc, out, err = run(exe, "--refuse", elem_tamper(honest, 0, W["parent"], par))
        ck(rc == 0 and needle in out, "%s was not refused (%r): %r" % (what, needle, out[:240]))
    rc, out, err = run(exe, "--refuse", elem_tamper(honest, 0, W["delay"], 120))
    ck(rc == 0 and "one frame" in out and "tempo" in out,
       "a delay on a single-frame row was not refused: %r" % out[:240])

    # ---- D42: v6 and nothing older ----
    for stamp in (3, 4, 5, 7):
        rc, out, err = run(exe, "--refuse", abi_lie(honest, stamp))
        ck(rc == 0 and "nothing older" in out,
           "a clock section stamped v%d was not refused at the clock's gate: %r" % (stamp, out[:240]))
    for stamp in (4, 5):
        rc, out, err = run(exe, "--refuse", abi_lie(honest, 6, header=stamp))
        ck("refused" in out and "geometry v%d" % stamp in out,
           "a container stamped v%d was not refused at the header: %r" % (stamp, out[:240]))
    print("  C4 at v6: delta plate, closing rect, still, preview, instance, parent; "
          "stamps 3, 4, 5 and 7 refused")


# ---------------------------------------------------------------------------
# D39 — sounds, unchanged by D42 but for the beat they are held against: the tick
# loop's beat must agree with the seconds row's own cadence, which the reader now
# derives rather than reads.
# ---------------------------------------------------------------------------
import orbsound as osnd            # noqa: E402

D39_LOOP_N = ts.SOUND_RATE * 8      # an 8 s loop: a whole number of beats at 1/2/4/8


def d39_pcm(n, v=0x0102):
    return _st.pack("<%dh" % n, *([v] * n))


def d39_forge(base, sounds, pattern="none", tamper=None, check=False):
    """The pair-forged section with `sounds` put in by the writer, then `tamper`
    (els, sc, art, spr) applied. Returns (path, py_verdict): None when
    check_clock_sounds accepts the final table at the beat the rows derive."""
    box = {}

    def mutate(els, sc, spr, art):
        c4_defaults(els, sc, art, spr)
        sec = {"elements": els, "scalars": sc, "sprites": spr, "art": art}
        osnd.add_sounds(sec, sounds, pattern, check=check)
        if tamper:
            tamper(els, sc, art, spr)
        sweep, rate = ts.clock_tempo(els, sc)
        try:
            ts.check_clock_sounds(els, [(int(p["off"]), int(p["size"]), int(p["w"]),
                                         int(p["h"])) for p in spr], sweep, rate)
            box["py"] = None
        except ValueError as e:
            box["py"] = str(e)
    path, _v = forge(base, mutate, raw=True)
    return path, box["py"]


def d39_step(base):
    """The base dial's beat, as its rows derive it."""
    _h, secs = ob.read_bundle(base)
    b = [s for s in secs if s["blob"]["slug"] == "clock"][0]["blob"]
    return ts.clock_tempo(b["elements"], b["scalars"])


def c4_forge_rows(path):
    _h, secs = ob.read_bundle(path)
    return [dict(e) for e in secs[0]["blob"]["elements"]]


def d39_hand_steps(step):
    def t(els, sc, art, spr):
        sec_row(els).update(cadence=ts.CAD_STEP, cad_arg=4 if step != 4 else 2)
    return t


def d39_battery(exe, base):
    sweep, step = d39_step(base)
    beat = 100 if sweep else step * 100
    full = [{"slot": "tick_loop", "pcm": d39_pcm(D39_LOOP_N), "beat_chz": beat,
             "first_beat": 7},
            {"slot": "chime_hour", "pcm": d39_pcm(30001)},
            {"slot": "strike_hour", "pcm": d39_pcm(22050)},
            {"slot": "jump_minute", "pcm": d39_pcm(2205)}]
    plain = parsed(run(exe, "--dump3f", forge(base, c4(c4_defaults))[0])[1])
    path, py = d39_forge(base, full, "count")
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0 and py is None, "an honest four-sound table was refused (C %r, py %r)"
       % (out[:240], py))
    g = parsed(out)
    got = [(x["slot"], x["n"], x["off"], x["beat"], x["first"], x["flags"]) for x in g["sounds"]]
    head = 2 * (D39_LOOP_N + 30001 + 22050 + 2205)
    want = [(0, D39_LOOP_N, 0, beat, 7, 1),
            (4, 30001, 2 * D39_LOOP_N, 0, 0, 0),
            (5, 22050, 2 * (D39_LOOP_N + 30001), 0, 0, 0),
            (6, 2205, 2 * (D39_LOOP_N + 30001 + 22050), 0, 0, 0)]
    ck(got == want, "the projected sounds are %s, not %s" % (got, want))
    ck(g["pattern"] == ts.HOUR_PATTERNS.index("count"), "the pattern projected as %s" % g["pattern"])
    ck(g["rows"][0]["spr"]["color_off"] == plain["rows"][0]["spr"]["color_off"] + head and
       g["sc"]["plate_off"][0] == plain["sc"]["plate_off"][0] + head,
       "the first row / plate did not move by the %d sound bytes put before them" % head)
    hdr, secs = ob.read_bundle(path)
    s0 = secs[0]
    sec = {"elements": [dict(e) for e in s0["blob"]["elements"]],
           "scalars": dict(s0["blob"]["scalars"]),
           "sprites": [dict(p) for p in s0["blob"]["sprites"]], "art": bytearray(s0["art"])}
    osnd.strip_sounds(sec)
    _h0, base_secs = ob.read_bundle(forge(base, c4(c4_defaults))[0])
    b0 = base_secs[0]
    ck(bytes(sec["art"]) == bytes(b0["art"]) and
       [p["off"] for p in sec["sprites"]] == [p["off"] for p in b0["blob"]["sprites"]],
       "strip_sounds did not return the table and bank it started from")
    path, py = d39_forge(base, [{"slot": "morse_dit", "pcm": d39_pcm(1323)},
                                {"slot": "morse_dah", "pcm": d39_pcm(3969)}], "morse")
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0 and py is None and parsed(out)["pattern"] == ts.HOUR_PATTERNS.index("morse"),
       "a morse pair was refused or lost its pattern: %r %r" % (out[:200], py))
    print("  D39 accept: four sounds at the head, pixels shifted by %d B, strip inverts" % head)

    # A compressed chime (the baker's choice for a chime): projected with its encoding,
    # its plane ADPCM-sized; a truncated plane is refused by the device and the schema.
    import math as _m
    chime = _st.pack("<22050h", *[int(14000 * _m.exp(-3 * i / 22050) *
                                      _m.sin(2 * _m.pi * 440 * i / 22050)) for i in range(22050)])
    def forge_c(tamper=None):
        box = {}
        def mutate(els, sc, spr, art):
            c4_defaults(els, sc, art, spr)
            sec = {"elements": els, "scalars": sc, "sprites": spr, "art": art}
            osnd.add_sounds(sec, [{"slot": "chime_hour", "pcm": chime}], "none", check=False,
                            compress=True)
            if tamper:
                tamper(els, sc, art, spr)
            try:
                ts.check_clock_sounds(els, [(int(p["off"]), int(p["size"]), int(p["w"]),
                                             int(p["h"])) for p in spr], *ts.clock_tempo(els, sc))
                box["py"] = None
            except ValueError as e:
                box["py"] = str(e)
        return forge(base, mutate, raw=True)[0], box["py"]
    path, py = forge_c()
    rc, out, err = run(exe, "--dump3f", path)
    snd = parsed(out)["sounds"] if rc == 0 else []
    ck(rc == 0 and py is None and len(snd) == 1 and snd[0]["enc"] == ts.SOUND_ENC_ADPCM and
       snd[0]["n"] == 22050, "a compressed chime was refused or projected wrongly: %r %r" % (out[:200], py))

    def short(els, sc, art, spr):
        sp = spr[0]
        sp["size"] = int(sp["size"]) - ts.ADPCM_BLOCK_BYTES
    path, py = forge_c(short)
    rc, out, err = run(exe, "--refuse", path)
    ck(rc == 0 and ("%d B" % ts.sound_bytes(ts.SOUND_ENC_ADPCM, 22050)) in out and py and
       "in encoding 1 is w samples by 1" in py,
       "a compressed plane a block short was not refused by both: %r %r" % (out[:200], py))
    print("  D39 ADPCM: a compressed chime projects as encoding 1; a short plane is refused")

    def row_of(els, slot):
        return [e for e in els if int(e["class"]) == ts.CLASS_SOUND and
                int(e["slot"]) == ts.CLOCK_SOUND_SLOTS.index(slot)][0]

    def set_row(which, **kw):
        def t(els, sc, art, spr):
            row_of(els, which).update(kw)
        return t

    def plane_h2(els, sc, art, spr):
        sp = spr[int(row_of(els, "strike_hour")["frame0"])]
        sp["w"], sp["h"] = sp["w"] // 2, 2

    no_strike = [x for x in full if x["slot"] != "strike_hour"]
    odd_loop = [dict(full[0], pcm=d39_pcm(D39_LOOP_N + 1))] + full[1:]
    vectors = (
        ("odd offset", [], "count", d39_odd_tail, "starts at odd offset"),
        ("patterns disagree", full, "count",
         set_row("jump_minute", kind=ts.HOUR_PATTERNS.index("morse")), "a theme has one pattern"),
        ("loop not looped", full, "count", set_row("tick_loop", flags=0), "not flagged to loop"),
        ("chime looped", full, "count", set_row("chime_hour", flags=ts.EF_LOOP),
         "only the tick loop runs"),
        ("no beat", full, "count", set_row("tick_loop", vmax=0), "names no beat"),
        ("part beats", odd_loop, "count", None, "not a whole number of beats"),
        ("beat off the hand", full, "count", d39_hand_steps(step),
         "the sound would walk off the hand"),
        ("a sound sized like a sprite", full, "count", set_row("chime_hour", w=4, h=4),
         "a sound places nothing"),
        ("first beat past a beat", full, "count",
         set_row("tick_loop", vmin=ts.SOUND_RATE * 100 // beat), "sits past one beat"),
        ("beat on a chime", full, "count", set_row("chime_hour", vmax=100),
         "only the tick loop has one"),
        ("a plane two rows high", full, "count", plane_h2, "is w samples by 1"),
        ("count with no stroke", no_strike, "count", None, "carries no hour stroke"),
        ("morse with no dah", [{"slot": "morse_dit", "pcm": d39_pcm(1323)}], "morse", None,
         "lacks a dit or a dah"),
        ("repeater short a stroke", full, "repeater", None, "lacks one of its"),
        ("slot past the vocabulary", full, "count", set_row("jump_minute", slot=13),
         "declares 13 sound slots"),
    )
    refused = 0
    for what, sounds, pat, tamper, needle in vectors:
        path, py = d39_forge(base, sounds, pat, tamper=tamper)
        rc, out, err = run(exe, "--refuse", path)
        ck(rc == 0 and needle in out,
           "%s: the device did not refuse in its own words (%r): %r" % (what, needle, out[:240]))
        py_ok = py is not None and needle in py
        if not py_ok:
            try:
                ts.check_table(c4_forge_rows(path))
            except ValueError as e:
                py_ok = needle in str(e)
        ck(py_ok, "%s: theme_schema did not refuse it (%r): %r" % (what, needle, py))
        refused += 1
    print("  D39 refusals: %d vectors, refused by the device and the schema alike" % refused)
    return full


D39_MUTANTS = (
    ("the odd-offset guard", "if (sp.off & 1u)", "if (0 && (sp.off & 1u))", "odd offset"),
    ("the pattern agreement", "if (pattern >= 0 && (int)e.kind != pattern)", "if (0)",
     "patterns disagree"),
    ("the loop flag on the tick", "if (slot == CLK_SND_TICK_LOOP && !loop)", "if (0)",
     "loop not looped"),
    ("the whole-beats guard",
     "if (((uint64_t)sp.w * bchz) % ((uint64_t)CLK_SND_RATE * 100u))", "if (0)", "part beats"),
    ("the beat-against-the-hand guard", "if (!t->second_sweep && bchz != step * 100u)",
     "if (0)", "beat off the hand"),
    ("the stroke the pattern needs",
     "if ((pattern == CLK_PAT_COUNT || pattern == CLK_PAT_SHIPS_BELLS) && !strike)", "if (0)",
     "count with no stroke"),
)


def d39_mutants(exe, base, full):
    sweep, step = d39_step(base)

    def row(els, slot):
        return [e for e in els if int(e["class"]) == ts.CLASS_SOUND and
                int(e["slot"]) == ts.CLOCK_SOUND_SLOTS.index(slot)][0]

    inputs = {
        "odd offset": ([], "none", d39_odd_tail),
        "patterns disagree": (full, "count", lambda e, s, a, p: row(e, "jump_minute").update(
            kind=ts.HOUR_PATTERNS.index("morse"))),
        "loop not looped": (full, "count", lambda e, s, a, p: row(e, "tick_loop").update(flags=0)),
        "part beats": ([dict(full[0], pcm=d39_pcm(D39_LOOP_N + 1))] + full[1:], "count", None),
        "beat off the hand": (full, "count", d39_hand_steps(step)),
        "count with no stroke": ([x for x in full if x["slot"] != "strike_hour"], "count", None),
    }
    for what, old, new, tag in D39_MUTANTS:
        sounds, pat, tamper = inputs[tag]
        path, _py = d39_forge(base, sounds, pat, tamper=tamper)
        hrc, hout, _ = run(exe, "--refuse", path)
        ck("refused" in hout, "%s: the honest reader accepted %s" % (what, tag))
        m = build(mut=(old, new))
        rc, out, err = run(m, "--refuse", path)
        ck(rc != hrc or out != hout, "uncaught D39 mutant: %s — broken, it answered %r"
           % (what, out[:160]))
    print("  D39 mutants: %d guards, each asked about its own lie" % len(D39_MUTANTS))


# ---------------------------------------------------------------------------
# Mutants: each guard broken in a private copy and asked about the lie only it
# refuses. An uncaught mutant is a device-side refusal nobody is watching.
# (guard, anchor, break, REFUSALS name)
# ---------------------------------------------------------------------------
MUTANTS = (
    # radar-plan §2.11's values guards
    ("values' slot", 'if (e.slot != 0u)\n                    return reject("values at slot',
     'if (0)\n                    return reject("values at slot', "values at slot 1"),
    ("values' version", 'if (!e.src)\n                    return reject("values of layout version 0',
     'if (0)\n                    return reject("values of layout version 0', "values of version 0"),
    ("values place nothing", "if (e.w || e.h || e.x || e.y)", "if (0)", "values that place"),
    ("values' bound", "sp.w > VALUES_MAX || sp.size != sp.w", "sp.size != sp.w", "values past 4096"),
    # No mutant for the values plane's h == 1: with the walk's w*h price and the size == w
    # rule, a plane two rows tall is refused either way, so dropping it is equivalent.
    # OS v1.2.17's sparse fonts
    ("a font carries a glyph", "if (first < 0)\n            return reject(\"font %u carries no glyph",
     "if (0)\n            return reject(\"font %u carries no glyph", "a font that carries no glyph"),
    ("a sparse font's one form", "if (gs.size != form || gs.w != fe.w || gs.h != fe.h)",
     "if (gs.w != fe.w || gs.h != fe.h)", "a sparse font of two forms"),
    ("the plate's full-panel rule",
     "if (plate.x || plate.y || plate.w != (uint16_t)scr_w || plate.h != (uint16_t)scr_h)",
     "if (0)", "a moved plate"),
    ("the plate's existence", "if (at_plate[0] < 0)", "if (0 && at_plate[0] < 0)", "no plate row"),
    ("the hand class's refusal", "            case CLS_HAND:\n            case CLS_SPRITE:\n",
     "            case CLS_SPRITE:\n", "a hand row"),
    ("the row cap", "if (e.slot >= CLK_ROW_MAX)", "if (0)", "a row at slot 64"),
    ("the retired scalars", "if (U(sec, RETIRED_W[k]))", "if (0)", "a retired tick_rate"),
    ("the shape bound", "if (e->kind >= CLK_SHAPE__MAX)", "if (0)", "a shape past the vocabulary"),
    ("the drum's strip rule", "if (e->dy != (int16_t)e->h && e->dy != -(int16_t)e->h)", "if (0)", "a drum rolling less than its strip"),
    ("the drum's radius rule", "if (e->radius < 2 || 2u * e->radius > PANEL_PX)", "if (0)", "a drum with no radius"),
    ("the text shape", "if (e->kind == CLK_SHAPE_TEXT)", "if (0)", "a text shape"),
    ("the label's source", "if (e->src != CLK_SRC_TEXT)", "if (0)", "a text shape"),
    ("the label's stillness", "if (e->span || e->zero || e->vmin || e->vmax || e->dx || e->dy || e->cadence ||",
     "if (0 && (e->span || e->zero || e->vmin || e->vmax || e->dx || e->dy || e->cadence) ||",
     "a label that moves"),
    ("the label's alignment", "if (e->digit > 2u || e->end > 2u)", "if (0)", "a label aligned past end"),
    ("the label's tone 0", "if ((e->str & 0xFu) == CLK_FONT_NONE)", "if (0)", "a label with no tone 0"),
    ("the label's one panel", "if (e->n_frames != 1u || (e->flags & AUX_F_SHADOW))\n        return reject(\"row %u is a text row",
     "if (0)\n        return reject(\"row %u is a text row", "a label of two panels"),
    ("the label's padding",
     "if (e->radius > CLK_LABEL_PAD_MAX || 2u * e->radius >= e->w || 2u * e->radius >= e->h)",
     "if (0)", "a label padded shut"),
    ("the label's fonts present", "if (f != CLK_FONT_NONE && (f >= CLK_FONT_SLOTS || at_font[f] < 0))",
     "if (0)", "a label naming an absent font"),
    ("the font slot bound", "if (e.slot >= CLK_FONT_SLOTS)", "if (0)", "a font past the slots"),
    ("the font's glyph count", "if (e.n_frames < CLK_FONT_ASCII || e.n_frames > CLK_FONT_GLYPHS)",
     "if (0)", "a font of one glyph"),
    ("the font's origin", "if (e.x || e.y)\n                    return reject(\"font",
     "if (0)\n                    return reject(\"font", "a font off the origin"),
    ("the font's space", "if (e.pivot_x <= 0 || (int32_t)e.pivot_x > (int32_t)e.w ||",
     "if (0 && (int32_t)e.pivot_x > (int32_t)e.w ||", "a font with no space"),
    ("the str refusal", "if (e->str)\n        return reject(\"row %u names fonts",
     "if (0)\n        return reject(\"row %u names fonts", "a string offset"),
    ("the still row's silence", 'return reject("row %u is still (source none)',
     'if (0) return reject("row %u is still (source none)', "a still row with a motion"),
    ("the moving row's cadence", "        if (!e->cadence)\n", "        if (0)\n",
     "a moving row with no cadence"),
    ("the domain", "if (e->vmax <= e->vmin)", "if (0)", "an empty domain"),
    ("somewhere to go", "if (!e->span && !e->dx && !e->dy)", "if (0)", "a row that goes nowhere"),
    ("the wrap rule", "if (e->end == CLK_END_WRAP &&", "if (0 &&", "a wrap that travels"),
    ("the travel bound", 'return reject("row %u travels (%d,%d)',
     'if (0) return reject("row %u travels (%d,%d)', "a travel off the panel"),
    ("the pick cap", "e->n_frames > CLK_PICK_MAX)", "e->n_frames > 9999u)",
     "a picked row past its frames"),
    ("the twin under a pick", "if (pe.n_frames > 1u && !pe.delay)", "if (0)",
     "a twin under a picked row"),
    ("the twin's silence", "if ((e->flags & AUX_F_SHADOW) && (e->n_frames != 1u || e->src || e->cadence))",
     "if (0)", "a twin with a motion of its own"),
    ("the ease's bound", "if (e->ease > 1000u)", "if (0)", "an ease past a second"),
    ("the ease's instant", "if (!fly && !steps)", "if (0)", "an ease on nothing"),
    ("step 0", "if (e->cadence == CLK_CAD_STEP && !e->cad_arg)", "if (0)", "a step of 0"),
    ("the sweep's rate", "if (e->cadence == CLK_CAD_SWEEP && e->cad_arg > CLK_SWEEP_FPS_MAX)",
     "if (0)", "a sweep faster than the panel"),
    ("the stop-to-go source", "if (e->cadence == CLK_CAD_STOP_TO_GO && e->src != CLK_SRC_SECOND)",
     "if (0)", "stop-to-go on the hour"),
    ("the release", "if (e.cadence == CLK_CAD_STOP_TO_GO && !minute_jumps)", "if (0)",
     "stop-to-go with no jumping minute"),
    ("the one beat", "if (rate && rate != e.cad_arg)", "if (0)", "seconds rows that disagree"),
    ("the one animated row", "if (t->car_row != 0xFFu)", "if (0)", "two animated rows"),
    ("the twin's row", "if (slot == 0 || at_aux[slot - 1] < 0)", "if (0)",
     "a twin with no row before it"),
    ("the twin under a twin", "if (pe.flags & AUX_F_SHADOW)", "if (0)", "a twin under a twin"),
    ("the parent's class", "        if (!pr)\n", "        if (0)\n", "a parent that is not a moving row"),
    ("the parent's arbour", "if (p->span && (p->spr.centerx != r->spr.centerx",
     "if (0 && (p->spr.centerx != r->spr.centerx", "a child off its turning parent's arbour"),
    ("a pose on a plain class", "if (e.cls != CLS_AUX && (e.dx || e.dy || e.end || e.digit || e.str))",
     "if (0)", "a pose on the plate"),
)


def values_battery(exe, base):
    """radar-plan §2.11, the accept side: a values row the writer takes lands where its
    plane is, with its length and the app's version, and its plane is priced a byte a
    byte (a reader that priced it as pixels refuses it)."""
    seat = {}

    def vals(els, sc, spr, art):
        seat["e"] = add_values(els, spr, art, n=40, ver=3)
        seat["off"] = spr[-1]["off"]
    path, verdict = forge(base, vals)
    ck(verdict is None, "values: the writer refused a 40-byte blob: %s" % verdict)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "values: the reader refused a 40-byte blob: %s" % out[:300])
    if rc:
        return
    v = parsed(out)["values"]
    ck(v == (seat["off"], 40, 3), "values: the reader found %s, not (%d, 40, 3)" % (v, seat["off"]))
    plain = parsed(run(exe, "--dump3f", forge(base, None)[0])[1])["values"]
    ck(plain == (0, 0, 0), "values: a theme with none answers %s, not (0, 0, 0)" % (plain,))
    m = build(mut=("on_values ? (uint64_t)sp.w * sp.h ", "on_values ? (uint64_t)sp.w * sp.h * 3 "))
    mrc, mout, _ = run(m, "--dump3f", path)
    ck(mrc != 0 or "refused" in mout, "uncaught mutant: values priced as pixels — it answered %r" % mout[:120])
    print("  values: a 40-byte blob lands at its plane, version 3, a byte a byte")


def pick_battery(exe, base):
    """theme-pool-plan §2.6, the accept side: a row whose frames the app picks lands
    as one row, its frame count in pick_n and nowhere else, beside a carriage that
    keeps its own meaning."""
    seat = {}

    def picks(els, sc, spr, art):
        seat["slot"] = next_slot(els)
        add_pick_row(els, spr, art, seat["slot"], 49, **SWEEP_SEC)
    path, verdict = forge(base, picks)
    ck(verdict is None, "pick: the writer refused a 49-frame picked row: %s" % verdict)
    rc, out, err = run(exe, "--dump3f", path)
    ck(rc == 0, "pick: the picked row was refused: %s" % out[:300])
    if rc:
        return
    p = parsed(out)
    pick = p["sc"]["pick_n"]
    at = [i for i, n in enumerate(pick) if n]
    ck(len(pick) == ts.CLK_ROW_MAX and len(at) == 1 and pick[at[0]] == 49,
       "pick: pick_n is %s, not one row of 49" % pick)
    ck(at and p["rows"][at[0]]["tag"] == seat["slot"],
       "pick: the 49 sit on row %s, not the row at slot %d" % (at, seat["slot"]))
    ck(p["sc"]["car_row"] == [255] and p["sc"]["tourbillon_n"] == [0],
       "pick: a picked row made a carriage (car_row %s, %s frames)"
       % (p["sc"]["car_row"], p["sc"]["tourbillon_n"]))
    print("  pick: a 49-frame picked row lands in pick_n, no carriage")


def mutants(exe, base):
    by_name = {v[0]: v[1] for v in REFUSALS}
    for what, old, new, tag in MUTANTS:
        mut = by_name[tag]

        def mutate(els, sc, spr, art, mut=mut, tag=tag):
            n0 = len(spr)
            mut(els, sc, spr, art)
            if len(spr) > n0 and tag in ("two animated rows",):
                _respread(els, spr, art)
        path, _v = forge(base, mutate, raw=True)
        hrc, hout, _ = run(exe, "--refuse", path)
        ck("refused" in hout, "%s: the honest reader accepted the input only it refuses (%s)"
           % (what, hout[:120]))
        m = build(mut=(old, new))
        rc, out, err = run(m, "--refuse", path)
        ck(rc != hrc or out != hout, "uncaught mutant: %s — broken, it answered %r"
           % (what, out[:120]))
    print("  mutants: %d guards, each asked about its own lie" % len(MUTANTS))


if __name__ == "__main__":
    EXE, PAIR = main()
    refusals(EXE, PAIR)
    rows_battery(EXE, PAIR)
    d32_battery(EXE, PAIR)
    s31_battery(EXE, PAIR)
    c4_battery(EXE, PAIR)
    pick_battery(EXE, PAIR)
    values_battery(EXE, PAIR)
    FULL = d39_battery(EXE, PAIR)
    if "--mutations" in sys.argv:
        mutants(EXE, PAIR)
        d39_mutants(EXE, PAIR, FULL)
    print("%d checks, %d failures" % (checks[0], len(failures)))
    sys.exit(1 if failures else 0)
