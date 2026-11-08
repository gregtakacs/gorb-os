#!/usr/bin/env python3
"""Host bench for the stage's live layer (radar-plan.md §2.9, R3c).

Two rules are the layer's contract, and both are lifted VERBATIM and compiled with host
gcc -Werror:

- `over` (main/app/orb_ui.c): an app's indexed draw onto a target. A page canvas is
  opaque, so a pixel blends as the canvas draws always did; the live layer keeps an
  alpha plane, so src over a pixel it only partly covers must add up the coverage and
  weight the colours by what each contributes, or a map under rain darkens to black.
- `live_restack` (main/stage/orb_stage.c): the classic path's z is the page's child
  order, so the layer's image goes in before the first widget the stage built that
  stands deeper than it (each carries its depth key + 1), under the glass and under
  any widget of the app's own (no key). The L4 table gets the same depth from
  depth_key, which is lifted with it.

Run:  python3 tools/host_live_test.py              (rc 0 = pass)
      python3 tools/host_live_test.py --mutations  (proves the checks bite)
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
UI = os.path.join(ROOT, "main", "app", "orb_ui.c")
UIH = os.path.join(ROOT, "main", "app", "orb_ui.h")
STAGE = os.path.join(ROOT, "main", "stage", "orb_stage.c")

# Two edits are equivalent and left out: dropping `|| !da` (over a clear pixel the general
# formula gives ao = a and a weight of 255, the same answer; the branch saves the divide)
# and dropping `!tag ||` (an untagged child's tag - 1 wraps past every key and stops the
# walk anyway; the test says so for readers). The same goes for live_restack's explicit
# stop at the two lifted layers: they carry no tag, so the tag test stops there too.
MUTATIONS = [
    (UI, "const int32_t half = k2 * k2 / 2,", "const int32_t half = 0,"),
    (UI, "const int32_t u = p->sx * k2 + 2 * X - 2 * p->dx + 1 - p->k, x0", "const int32_t u = p->sx * k2 + 2 * X - 2 * p->dx, x0"),
    (UI, "p->pal[p->shift >= 0 ? n >> p->shift : n / (k2 * k2)]", "p->pal[p->shift >= 0 ? n >> (p->shift - 1) : n / (k2 * k2)]"),
    (UI, "for (int x = 0; x < w; x++) p->col[x] = p->prev[x] * (k2 - fy) + p->cur[x] * fy;", "for (int x = 0; x < w; x++) p->col[x] = p->prev[x] * (k2 - fy) + p->cur[x] * (fy ? fy : 1);"),
    (UI, "const int32_t c = p->sy * k2 - 2 * p->dy + 1 - p->k;", "const int32_t c = p->sy * k2 - 2 * p->dy;"),
    (UI, "if (x0 < 0 || x0 + 1 >= w) continue;", "if (x0 < 0 || x0 >= w) continue;"),
    (UI, "static int32_t floordiv(int32_t a, int32_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }", "static int32_t floordiv(int32_t a, int32_t b) { return a / b; }"),
    (UI, "if (floordiv(y * k2 - c + 1, 2) >= p->h) return 1;", "if (0) return 1;"),
    (UI, "a + (uint32_t)da * (255u - a) / 255u", "a + (uint32_t)da * 255u / 255u"),
    (UI, "(uint8_t)(a * 255u / ao)", "a"),
    (UI, "    t->a[i] = (uint8_t)ao;\n", ""),
    (UI, "{ t->px[i] = c; t->a[i] = a; return; }", "{ t->px[i] = c; return; }"),
    (STAGE, "if (!tag || tag - 1u > key[l]) break;", "if (!tag || tag - 1u >= key[l]) break;"),
    (STAGE, "(slot[1] == slot[0] && key[1] < key[0])", "(slot[1] == slot[0])"),
    (STAGE, "slot[hi] + (both ? 1 : 0)", "slot[hi]"),
    (STAGE, "        if (s_live_obj[l]) lv_obj_move_foreground(s_live_obj[l]);\n", ""),
    (STAGE, "(uint16_t)order * 2u + (twin ? 0u : 1u)", "(uint16_t)order * 2u"),
]


def func(src, sig):
    """The function whose definition starts with `sig`, through its closing brace."""
    i = src.index(sig)
    return src[i:src.index("\n}\n", i) + 3]


def lifted(mutation=None):
    ui, stage = open(UI).read(), open(STAGE).read()
    if mutation:
        path, old, new = mutation
        text = ui if path == UI else stage
        if old not in text:
            raise SystemExit("mutation %r does not apply -- rewrite the bench" % (old,))
        text = text.replace(old, new, 1)
        if path == UI:
            ui = text
        else:
            stage = text
    uih = open(UIH).read()
    i = uih.index("typedef struct { uint16_t *px; uint8_t *a;")
    tgt = uih[i:uih.index("\n", i) + 1]
    i = ui.index("typedef struct { uint16_t c; uint8_t a; } pal565_t;")
    pal_t = ui[i:ui.index("\n", i) + 1]
    i = ui.index("typedef struct {\n    const orb_ui_tgt_t *t;\n    const pal565_t *pal;           /* by value */")
    sctx = ui[i:ui.index("} smooth_ctx_t;", i) + len("} smooth_ctx_t;") + 1]
    return "\n".join([
        tgt, "#define ORB_PNG8_MAX_W 1024", pal_t,
        func(ui, "static inline uint16_t blend565("),
        func(ui, "static inline void over("),
        sctx,
        ui[ui.index("static int32_t floordiv("):ui.index("\n", ui.index("static int32_t floordiv(")) + 1],   # one line
        func(ui, "static void smooth_prep("),
        func(ui, "static int smooth_row("),
        func(stage, "static uint16_t depth_key("),
        func(stage, "static void live_restack(void)"),
    ])


PRELUDE = r"""
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* the page, as the walk sees it: children in z order, each with its user data */
typedef struct { void *user_data; int id; } lv_obj_t;
static lv_obj_t *kids[16];
static uint32_t nkids;
static lv_obj_t *s_scr = (lv_obj_t *)1;
#define STAGE_LIVE_N 2
static lv_obj_t *s_live_obj[STAGE_LIVE_N];
static int32_t s_live_order[STAGE_LIVE_N];
static uint32_t lv_obj_get_child_count(lv_obj_t *p) { (void)p; return nkids; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *p, int32_t i) { (void)p; return kids[i]; }
static void *lv_obj_get_user_data(lv_obj_t *o) { return o->user_data; }
static void lv_obj_move_to_index(lv_obj_t *o, int32_t to);
static void lv_obj_move_foreground(lv_obj_t *o) { lv_obj_move_to_index(o, (int32_t)nkids - 1); }
static void lv_obj_move_to_index(lv_obj_t *o, int32_t to)
{
    uint32_t at = 0;
    while (kids[at] != o) at++;
    memmove(&kids[at], &kids[at + 1], (nkids - at - 1) * sizeof kids[0]);
    memmove(&kids[to + 1], &kids[to], (nkids - 1 - to) * sizeof kids[0]);
    kids[to] = o;
}
"""

HARNESS = r"""
static int fails;
#define CK(c, label) do { if (c) printf("ok    %s\n", label); else { printf("FAIL  %s\n", label); fails++; } } while (0)

static lv_obj_t plate = { (void *)1, 1 }, row3 = { (void *)4, 2 }, row5 = { (void *)6, 3 },
                comp21 = { (void *)22, 4 }, glass = { (void *)0x10000, 5 }, appw = { NULL, 6 },
                live = { NULL, 9 }, rain = { NULL, 10 };

static int page(lv_obj_t **list, int n)       /* a page with layer 0 appended last */
{
    for (int i = 0; i < n; i++) kids[i] = list[i];
    kids[n] = &live;
    nkids = (uint32_t)n + 1;
    s_live_obj[0] = &live;
    s_live_obj[1] = NULL;
    return n;
}
static int at(lv_obj_t *o) { for (uint32_t i = 0; i < nkids; i++) if (kids[i] == o) return (int)i; return -1; }
static int live_at(void) { return at(&live); }
static int at_order(int32_t order) { s_live_order[0] = order; live_restack(); return live_at(); }
static void both(int32_t o0, int32_t o1)       /* re-stack both, as stage_live_open does */
{
    s_live_order[0] = o0; s_live_order[1] = o1;
    live_restack();
}

static uint16_t c565(int r, int g, int b) { return (uint16_t)((r & 31) << 11 | (g & 63) << 5 | (b & 31)); }

/* the smoothed draw: a source of w x h values through an identity palette (value -> colour),
 * onto an opaque target; each glass pixel then holds the value it sampled */
static uint16_t G[64 * 64];
static int run_smooth(const uint8_t *src, int sw, int shh, int sx, int sy, int dx, int dy, int k, int gw, int gh)
{
    static pal565_t pal[256];
    static uint8_t vmap[256];
    for (int i = 0; i < 256; i++) { pal[i].c = (uint16_t)i; pal[i].a = 255; vmap[i] = (uint8_t)i; }
    for (int i = 0; i < 64 * 64; i++) G[i] = 0xFFFF;
    orb_ui_tgt_t t = { G, NULL, 64, 64, 64 };
    static smooth_ctx_t c;
    c = (smooth_ctx_t){ .t = &t, .pal = pal, .vmap = vmap, .sx = sx, .sy = sy, .dx = dx, .dy = dy, .w = gw, .h = gh, .k = k, .prev_y = -2 };
    smooth_prep(&c);
    int rows = 0;
    for (int y = 0; y < shh; y++) { rows++; if (smooth_row(&c, y, src + y * sw, sw)) break; }
    return rows;
}
static double ref(const uint8_t *src, int sw, int shh, int sx, int sy, int dx, int dy, int k, int X, int Y)
{
    const double u = sx + (X - dx + 0.5) / k - 0.5, v = sy + (Y - dy + 0.5) / k - 0.5;
    const int x0 = (int)floor(u), y0 = (int)floor(v);
    if (x0 < 0 || y0 < 0 || x0 + 1 >= sw || y0 + 1 >= shh) return -1;
    const double fx = u - x0, fy = v - y0;
    const double a = src[y0 * sw + x0], b = src[y0 * sw + x0 + 1], cc = src[(y0 + 1) * sw + x0], d = src[(y0 + 1) * sw + x0 + 1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy;
}
static int check_smooth(const uint8_t *src, int sw, int shh, int sx, int sy, int dx, int dy, int k, int gw, int gh)
{
    run_smooth(src, sw, shh, sx, sy, dx, dy, k, gw, gh);
    int bad = 0;
    for (int Y = 0; Y < gh; Y++) for (int X = 0; X < gw; X++) {
        const double r = ref(src, sw, shh, sx, sy, dx, dy, k, X, Y);
        const int want = r < 0 ? 0xFFFF : (int)floor(r + 0.5);
        if (G[Y * 64 + X] != want) bad++;
    }
    return bad;
}

int main(void)
{
    /* ---- the smoothed, magnified PNG draw (radar-plan §6.9) ---- */
    static uint8_t S[16 * 16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) S[y * 16 + x] = (uint8_t)(x * 13 + y * 7 + ((x * y) % 5) * 3);
    CK(check_smooth(S, 16, 16, 0, 0, 0, 0, 1, 15, 15) == 0, "smoothed at 1x: each glass pixel is its source pixel's value");
    CK(check_smooth(S, 16, 16, 2, 3, 0, 0, 2, 26, 24) == 0, "at 2x, every glass pixel is the bilinear blend of the values at its centre");
    CK(check_smooth(S, 16, 16, 4, 2, -3, -1, 4, 40, 44) == 0, "at 4x with the source placed a fraction of a pixel off the glass's edge");
    CK(check_smooth(S, 16, 16, 1, 1, -5, -6, 8, 64, 64) == 0, "at 8x, Close's magnification");
    CK(check_smooth(S, 16, 16, 10, 0, 0, 0, 2, 20, 10) == 0, "past the source's right edge the glass is left alone, never read beyond the row");
    CK(check_smooth(S, 16, 16, 0, 0, 7, 9, 4, 40, 40) == 0, "a source placed right of and below the glass's corner: negative positions floor, not truncate");
    { static uint8_t R[8 * 2] = { 0, 100, 0, 100, 0, 100, 0, 100,  0, 100, 0, 100, 0, 100, 0, 100 };
      run_smooth(R, 8, 2, 0, 0, 0, 0, 2, 12, 2);
      CK(G[64 + 1] == 25 && G[64 + 2] == 75, "values blend before the palette: halfway from 0 to 100 a quarter on is 25, not a colour mix"); }
    CK(run_smooth(S, 16, 16, 0, 0, 0, 0, 4, 8, 8) < 16, "it stops the decode once the glass window is drawn");

    /* ---- over: onto a page canvas (no alpha plane) ---- */
    uint16_t px[4];
    uint8_t al[4];
    orb_ui_tgt_t cv = { px, NULL, 4, 1, 4 };
    px[0] = c565(0, 0, 0);
    over(&cv, 0, c565(31, 63, 31), 255);
    CK(px[0] == c565(31, 63, 31), "a canvas: full alpha replaces the pixel");
    px[0] = c565(0, 0, 0);
    over(&cv, 0, c565(31, 63, 31), 128);
    CK(px[0] == blend565(c565(0, 0, 0), c565(31, 63, 31), 128), "a canvas: part alpha blends as it always did");

    /* ---- over: onto the live layer ---- */
    orb_ui_tgt_t lv = { px, al, 4, 1, 4 };
    px[0] = c565(9, 9, 9); al[0] = 0;
    over(&lv, 0, c565(31, 0, 0), 100);
    CK(px[0] == c565(31, 0, 0) && al[0] == 100,
       "a clear pixel takes the source's colour whole and its alpha: no dark fringe from the fill colour");
    px[1] = c565(0, 0, 31); al[1] = 180;
    over(&lv, 1, c565(31, 0, 0), 255);
    CK(px[1] == c565(31, 0, 0) && al[1] == 255, "full alpha replaces colour and alpha");
    px[2] = c565(0, 0, 31); al[2] = 255;
    over(&lv, 2, c565(31, 0, 0), 128);
    CK(px[2] == blend565(c565(0, 0, 31), c565(31, 0, 0), 128) && al[2] == 255,
       "over an opaque pixel: the canvas's blend, and it stays opaque");
    px[3] = c565(0, 0, 31); al[3] = 128;
    over(&lv, 3, c565(31, 0, 0), 128);
    {
        const uint32_t ao = 128 + 128u * 127u / 255u;              /* 191 */
        CK(al[3] == ao, "half over half covers 191 of 255, not 128 and not 255");
        CK(px[3] == blend565(c565(0, 0, 31), c565(31, 0, 0), (uint8_t)(128u * 255u / ao)),
           "and its colour leans to the source by 128/191, what each layer contributes");
    }
    uint8_t a = 0;
    uint16_t p = 0;
    orb_ui_tgt_t one = { &p, &a, 1, 1, 1 };
    for (int k = 0; k < 40; k++) over(&one, 0, c565(0, 63, 0), 40);
    CK(a >= 240 && p == c565(0, 63, 0), "forty faint strokes of one colour build to near-opaque in that colour");

    /* ---- depth_key: the scale the rows and the layer share ---- */
    CK(depth_key(2, false) == 5 && depth_key(2, true) == 4 && depth_key(0, false) == 1,
       "depth_key: a row at order 2 is 5, its twin 4; order 0 is 1, above the plate's 0");

    /* ---- live_restack: the classic path's z ---- */
    lv_obj_t *dial[] = { &plate, &row3, &row5, &comp21, &glass, &appw };
    page(dial, 6);
    CK(at_order(2) == 3, "order 2 (key 5) goes above the row at key 5 and under the comp at 21");
    CK(at_order(0) == 1, "order 0 goes right above the plate");
    CK(at_order(1) == 2, "re-stacked downward: order 1 (key 3) above the row at 3, under the row at 5");
    CK(at_order(63) == 4, "order 63 goes under the glass");
    CK(at_order(10) == 4 && kids[0] == &plate && kids[1] == &row3 && kids[2] == &row5 && kids[3] == &comp21 &&
       kids[5] == &glass && kids[6] == &appw, "re-stacking keeps every other widget in its order");
    lv_obj_t *bare[] = { &plate, &row3, &appw };
    page(bare, 3);
    CK(at_order(63) == 2, "a theme with no glass: under the app's own widget, never over it");

    /* ---- two layers: a map and the rain ---- */
    page(dial, 6);
    kids[nkids++] = &rain;
    s_live_obj[1] = &rain;
    both(0, 10);
    CK(at(&live) == 1 && at(&rain) == 5, "map at 0 above the plate; rain at 10 above the comp at 10 (key 21)");
    both(10, 0);
    CK(at(&rain) == 1 && at(&live) == 5, "swapped orders swap them");
    both(2, 9);
    CK(at(&live) == 3 && at(&rain) == 4, "map at 2 above the row at 2, rain at 9 next above it, under the comp");
    both(5, 5);
    CK(at(&live) + 1 == at(&rain) && at(&live) == 3,
       "at one depth layer 0 goes under layer 1, both above the rows at that depth");
    both(9, 2);
    CK(at(&rain) == 3 && at(&live) == 4, "and with the orders the other way round");
    both(63, 63);
    CK(at(&rain) == at(&glass) - 1 && at(&live) == at(&glass) - 2, "both at 63: under the glass, 0 below 1");
    return fails ? 1 : 0;
}
"""


def run(src, quiet=False):
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        open(c, "w").write(PRELUDE + src + HARNESS)
        r = subprocess.run(["gcc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-o", exe, c, "-lm"],
                           capture_output=True, text=True)
        if r.returncode:
            if not quiet:
                print(r.stderr)
            return None
        r = subprocess.run([exe], capture_output=True, text=True)
        if not quiet:
            print(r.stdout, end="")
        return r.returncode


def main():
    if "--mutations" in sys.argv:
        caught = 0
        for m in MUTATIONS:
            rc = run(lifted(m), quiet=True)
            ok = rc != 0
            caught += ok
            print("%s  %s: %r -> %r" % ("caught" if ok else "MISSED", os.path.basename(m[0]), m[1], m[2]))
        print("%d/%d mutants caught" % (caught, len(MUTATIONS)))
        return 0 if caught == len(MUTATIONS) else 1
    rc = run(lifted())
    return 1 if rc is None else rc


if __name__ == "__main__":
    sys.exit(main())
