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
# walk anyway; the test says so for readers).
MUTATIONS = [
    (UI, "a + (uint32_t)da * (255u - a) / 255u", "a + (uint32_t)da * 255u / 255u"),
    (UI, "(uint8_t)(a * 255u / ao)", "a"),
    (UI, "    t->a[i] = (uint8_t)ao;\n", ""),
    (UI, "{ t->px[i] = c; t->a[i] = a; return; }", "{ t->px[i] = c; return; }"),
    (STAGE, "if (!tag || tag - 1u > key) break;", "if (!tag || tag - 1u >= key) break;"),
    (STAGE, "        if (c == s_live_obj) continue;\n", ""),
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
    return "\n".join([
        tgt,
        func(ui, "static inline uint16_t blend565("),
        func(ui, "static inline void over("),
        func(stage, "static uint16_t depth_key("),
        func(stage, "static void live_restack(void)"),
    ])


PRELUDE = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* the page, as the walk sees it: children in z order, each with its user data */
typedef struct { void *user_data; int id; } lv_obj_t;
static lv_obj_t *kids[16];
static uint32_t nkids;
static lv_obj_t *s_scr = (lv_obj_t *)1;
static lv_obj_t *s_live_obj;
static int32_t s_live_order;
static uint32_t lv_obj_get_child_count(lv_obj_t *p) { (void)p; return nkids; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *p, int32_t i) { (void)p; return kids[i]; }
static void *lv_obj_get_user_data(lv_obj_t *o) { return o->user_data; }
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
                live = { NULL, 9 };

static int page(lv_obj_t **list, int n)       /* a page with the layer appended last */
{
    for (int i = 0; i < n; i++) kids[i] = list[i];
    kids[n] = &live;
    nkids = (uint32_t)n + 1;
    s_live_obj = &live;
    return n;
}
static int live_at(void) { for (uint32_t i = 0; i < nkids; i++) if (kids[i] == &live) return (int)i; return -1; }
static int at_order(int32_t order) { s_live_order = order; live_restack(); return live_at(); }

static uint16_t c565(int r, int g, int b) { return (uint16_t)((r & 31) << 11 | (g & 63) << 5 | (b & 31)); }

int main(void)
{
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
    return fails ? 1 : 0;
}
"""


def run(src, quiet=False):
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        open(c, "w").write(PRELUDE + src + HARNESS)
        r = subprocess.run(["gcc", "-std=c11", "-Wall", "-Werror", "-Wno-unused-function", "-o", exe, c],
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
