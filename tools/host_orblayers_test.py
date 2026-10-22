#!/usr/bin/env python3
"""host_orblayers_test.py — the P5.5 layer table, tested on the host.

Why this file exists
--------------------
main/gfx/orb_layers.c is the file every card app's ink passes through, and the
P5.5 pass line — every compositor primitive reachable through the gate, refused
or accepted for exactly the documented reason, and counted either way — is not
a property anybody can see on the dial. The three failure shapes this table can
hide are the ones P5.5's design notes recorded as already-disproved assumptions:
a row that bakes into memory it does not own, a stale handle that paints a
stranger's art, and a generation field that means nothing. So the shipped
orb_layers.c is compiled VERBATIM here, beside the shipped orb_assets.c it
bakes through, with only the leaves stubbed — the allocator counts, the font
answers, and the two compositor calls (gfx_mark_rows, gfx_paint_ctx_init) are
recorded rather than simulated.

What is under test
------------------
  the hook        a half-installed allocator is no allocator; with no heap
                  wired, tier 1 lives and tier 2/3 refuses by count
  tier 1          the resolver's box and offset become the sprite the painter
                  reads: col = base + art.off, placement the override recipe
                  (descriptor first, answer second, centre last), generation
                  tags make every handle outlive nothing
  tier 2/3        the plate is baked into row-owned heap (the app's bank is
                  read-only and the table must not write there); the bake's
                  pixels are probed through the ctx the row builds, so the
                  band geometry proves the generator, not just the size
  the unset trap  gen_prim 0xFF means "use the answer's", and an answer with
                  no generator falls to the CLASS default — never to 0, which
                  is RECT. A HAND that silently draws a rectangle is the bug
  handles         stale refuses and counts; del is idempotent for the ORIGINAL
                  handle and refuses the generation after it; re-add of a
                  recycled row mints a new generation
  wipe            planes freed, rows gone, generations kept — a held handle
                  stays dead across an uninstall or a suspend
  jobs            table order IS paint order; show=0 rows stay live and
                  absent; a baked row answers with its own base; a theme row
                  with no payload draws nothing; jmax is a promise, not advice
  the commit hook   orb_sched_commit_layers() is mirrored here (its TU needs
                  lvgl and display): no table, an empty table and a
                  canvas-less pass each reach no commit, and a hand-built
                  table reaches gfx_commit exactly once, with the SHOWN rows'
                  count and the canvas it was handed. The python side lints
                  the shipped body against the mirror.
  the art.off base  the hook hands orb_layers_jobs() the mapped payload, so a
                  tier-1 row draws payload + art.off and nothing else. A
                  forged two-bank payload proves both readings: a folded
                  offset draws the planted plane, the raw catalogue offset
                  draws the OTHER bank. The shipped convention is
                  section-relative — orb_elems.c passes the catalogue row
                  through, orb_clock3.c walks it from zero — so the fold
                  belongs to the resolver, and the lint forbids a `+ blob_off`
                  inside the hook by name.

The two stubs that mirror shipped code (gfx_paint_ctx_init's gate, the swept
box) are linted against orb_gfx.c's own text on the python side — the same
mirror discipline the P5b suites apply to the class vocabulary.

Run:  python3 tools/host_orblayers_test.py             # the vectors
      python3 tools/host_orblayers_test.py --mutations # the mutant battery
"""

import os, re, subprocess, sys, tempfile

ROOT = (sys.argv[sys.argv.index("--root") + 1] if "--root" in sys.argv
        else os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
P = lambda *a: os.path.join(ROOT, "main", *a)
LAYERS_C, LAYERS_H = P("gfx", "orb_layers.c"), P("gfx", "orb_layers.h")
ASSETS_C, ASSETS_H = P("assets", "orb_assets.c"), P("assets", "orb_assets.h")
GFX_C, GFX_H = P("gfx", "orb_gfx.c"), P("gfx", "orb_gfx.h")
SVC_H = P("app", "orb_svc.h")
NL = "\n"

# ---- the shims: the two headers the shipped TUs reach but the host lacks ----
SHIM_ESP_ERR = """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_ALLOWED 0x10C
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x11D
"""

# lvgl.h arrives as the two typedefs orb_gfx.h's prototypes need. clk_sprite_t
# and the panel size come from the REAL generated header; SWEPT_PAD and
# CLK_PAINT_ROLES from the real orb_gfx.h. Nothing here redeclares a bound.
SHIM_LVGL = """#pragma once
#include <stdint.h>
typedef int16_t lv_coord_t;
typedef struct _lv_area_t { lv_coord_t x1, y1, x2, y2; } lv_area_t;
typedef struct lv_image_dsc_t { uint32_t magic; const void *data;
    uint32_t w, h, stride; uint32_t cf; } lv_image_dsc_t;
"""

# ---- the harness: leaves recorded, shipped table verbatim -------------------
HARNESS_C = r'''
/* host_orblayers_test harness: orb_layers.c (SHIPPED) + orb_assets.c (SHIPPED)
 * against recorded leaves. gfx_mark_rows / gfx_paint_ctx_init / gfx_swept_box
 * are implemented here, not in orb_gfx.c — the python side lints that this
 * file's ctx maths still mirrors the shipped gfx_paint_ctx_init, and the
 * shipped swept formula, rather than trusting the copy. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "orb_gfx.h"      /* the shims' lvgl.h supplies lv_area_t et al. */
#include "orb_layers.h"

static int g_fail;
static void ck(const char *nm, int cond, int detail)
{
    printf("%-6s %s%s%d\n", cond ? "ok" : "FAIL", nm, " |", detail);
    if (!cond) g_fail++;
}

/* ---- the recorded leaves -------------------------------------------------- */
static int      g_mark_calls;
static int32_t  g_my0, g_my1, g_mx0, g_mx1;
static int      g_mark_force;
void gfx_mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1, bool force)
{
    g_mark_calls++;
    g_my0 = y0; g_my1 = y1; g_mx0 = x0; g_mx1 = x1; g_mark_force = force ? 1 : 0;
}

/* Mirrors gfx_swept_rect_box: the four plate corners about the pivot, plus
 * SWEPT_PAD, clipped to the panel. */
void gfx_swept_box(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out)
{
    const double rad = (double)ang_10 * (M_PI / 1800.0);
    const double c = cos(rad), s = sin(rad);
    const int32_t rx1 = 0, ry1 = 0, rx2 = (int32_t)spr->w - 1, ry2 = (int32_t)spr->h - 1;
    int32_t x1 = INT32_MAX, y1 = INT32_MAX, x2 = INT32_MIN, y2 = INT32_MIN;
    for (int i = 0; i < 4; i++) {
        const double px0 = (i & 1) ? (double)rx2 : (double)rx1;
        const double py0 = (i & 2) ? (double)ry2 : (double)ry1;
        const double dx = px0 - (double)spr->pivotx, dy = py0 - (double)spr->pivoty;
        const double fx = (double)spr->centerx + (c * dx - s * dy);
        const double fy = (double)spr->centery + (s * dx + c * dy);
        const int32_t ix = (int32_t)floor(fx), iy = (int32_t)floor(fy);
        if (ix < x1) x1 = ix;
        if (iy < y1) y1 = iy;
        if (ix > x2) x2 = ix;
        if (iy > y2) y2 = iy;
    }
    x1 -= SWEPT_PAD; y1 -= SWEPT_PAD; x2 += SWEPT_PAD; y2 += SWEPT_PAD;
    out->x1 = (lv_coord_t)(x1 < 0 ? 0 : x1);
    out->y1 = (lv_coord_t)(y1 < 0 ? 0 : y1);
    out->x2 = (lv_coord_t)(x2 > CLK_SCR_W - 1 ? CLK_SCR_W - 1 : x2);
    out->y2 = (lv_coord_t)(y2 > CLK_SCR_H - 1 ? CLK_SCR_H - 1 : y2);
}

/* Mirrors gfx_paint_ctx_init's drawable gate and field arithmetic (the real
 * one is gfx_commit's private maths; the mirror is linted against it). */
static const uint8_t *g_last_base;   /* the vectors read the ctx array itself */
bool gfx_sprite_drawable(const clk_sprite_t *s)
{ return s->show && s->w > 0 && s->h > 0 && s->color_off != 0; }
bool gfx_paint_ctx_init(gfx_paint_ctx_t *pc, const clk_sprite_t *spr,
                        const uint8_t *base, int32_t ang_10, uint8_t role)
{
    if (!gfx_sprite_drawable(spr)) return false;
    memset(pc, 0, sizeof *pc);
    pc->role = role;
    pc->col = (const uint16_t *)(base + spr->color_off);
    pc->alp = base + spr->color_off + (uint32_t)spr->w * (uint32_t)spr->h * 2u;
    pc->w = spr->w; pc->h = spr->h;
    pc->posx = (int32_t)spr->centerx - (int32_t)spr->pivotx;
    pc->posy = (int32_t)spr->centery - (int32_t)spr->pivoty;
    pc->bmode = spr->blend;
    lv_area_t box; gfx_swept_box(spr, ang_10, &box);
    pc->bx1 = box.x1; pc->by1 = box.y1; pc->bx2 = box.x2; pc->by2 = box.y2;
    pc->rot = (ang_10 % 3600 != 0);
    g_last_base = base;
    return true;
}

/* ---- the recorded commit, and the hook it records -------------------------
 * Migration 2 (P8, Commits 2-4): orb_sched_commit_layers() lives in a TU the
 * host cannot compile (lvgl.h, display.h, esp_timer.h), so the shipped body is
 * mirrored here exactly as host_sched_rung_test.py mirrors the rung pick, and
 * the python side lints the shipped text for every branch this copy depends on.
 * Commit 4 did not move the body; it moved the CALL to sched_flush_commit,
 * which the section-14 lints below pin in the call site's old place.
 * gfx_commit is recorded, not simulated: the vector's whole claim is the job
 * count the commit was handed. */
static int g_commit_calls, g_commit_nj;
static const gfx_paint_ctx_t *g_commit_jc;
static const uint16_t *g_commit_canvas;
void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas)
{
    g_commit_calls++; g_commit_nj = nj; g_commit_jc = jc; g_commit_canvas = canvas;
}

/* The shipped hook's job array lives at file scope for the same reason: the
 * ceiling is now ORB_LAYER_JOB_MAX deep, and the LVGL task's stack is not ours
 * to spend a kilobyte of per tick. One caller, one task — mirrored here as the
 * function-static it also is on the target. */
static int hook_mirror(const orb_layer_tab_t *t, const uint8_t *payload,
                       const uint16_t *canvas)
{
    static gfx_paint_ctx_t jc[ORB_LAYER_JOB_MAX];
    if (!t || orb_layers_count(t) == 0) return 0;
    if (!canvas) return 0;
    int nj = orb_layers_jobs(t, payload, jc, ORB_LAYER_JOB_MAX);
    if (nj) gfx_commit(nj, jc, canvas);
    return nj;
}

/* ---- the allocator, counted ---------------------------------------------- */
static int g_alloc_n, g_free_n, g_alloc_fail_next;
static long g_live_bytes;
static void *g_live[24]; static uint32_t g_live_sz[24];
static void *rec_alloc(uint32_t b)
{
    if (g_alloc_fail_next) { g_alloc_fail_next = 0; return NULL; }
    g_alloc_n++; g_live_bytes += b;
    void *p = malloc(b);
    for (int i = 0; i < 24; i++) if (!g_live[i]) { g_live[i] = p; g_live_sz[i] = b; break; }
    return p;
}
static void rec_free(void *p)
{
    if (!p) return;
    for (int i = 0; i < 24; i++)
        if (g_live[i] == p) { g_live[i] = NULL; g_live_bytes -= g_live_sz[i]; break; }
    g_free_n++;
    free(p);
}

/* ---- the font backend: one fixed glyph run, ARGB8888 ---------------------- */
static int g_text_calls;
static bool text_stub(const char *t, uint8_t **plane, uint16_t *w, uint16_t *h,
                      uint32_t color, uint8_t alpha)
{
    (void)color; (void)alpha;
    if (!t || !*t) return false;
    const size_t n = strlen(t);
    *w = (uint16_t)(4 * n); *h = 7;
    uint8_t *p = malloc((size_t)*w * *h * 4);
    if (!p) return false;
    for (size_t i = 0; i < (size_t)*w * *h; i++) { p[i * 4] = 10; p[i * 4 + 1] = 20;
        p[i * 4 + 2] = 30; p[i * 4 + 3] = 200; }
    *plane = p;
    g_text_calls++;
    return true;
}

/* ---- descriptors ---------------------------------------------------------- */
static orb_layer_desc_t mk(uint16_t cls, uint16_t tier, int16_t x, int16_t y,
                           int16_t w, int16_t h)
{
    orb_layer_desc_t d;
    memset(&d, 0, sizeof d);
    d.art.cls = cls; d.art.tier = tier; d.art.w = w; d.art.h = h;
    d.art.x = x; d.art.y = y;
    d.gen_prim = 0xFF;                    /* unset: the field-zero trap door  */
    d.show = 1; d.alpha = 255;
    return d;
}

static uint8_t theme_mem[1 << 17];                              /* the payload */
#define THEME ((const uint8_t *)theme_mem)
#define T1_OFF  0x120u

/* ---- the base-convention rig (Migration 2, Commit 2) ----------------------
 * A forged payload holding TWO section banks, so a tier-1 row's offset has an
 * unambiguous answer to the question "payload-relative or section-relative?".
 * The pixels live at BANK_B_OFF + PL_SPR_OFF inside the payload; a row that
 * carries the folded offset draws them, and a row that carries the raw
 * catalogue offset draws bank A's bytes at the same distance into the payload.
 * Both facts are asserted below, because the one that is merely assumed is the
 * one Commit 3 gets wrong. */
#define BANK_B_OFF   0x20000u
#define PL_SPR_OFF   0x120u
#define PL_COLOR     0x0F0Fu                    /* the planted RGB565 word     */
#define FAKE_COLOR   0xF0F0u                    /* what the wrong base draws   */

static void forge_payload(void)
{
    for (int i = 0; i < (int)sizeof theme_mem; i++) theme_mem[i] = 0;
    uint16_t *a = (uint16_t *)(theme_mem + PL_SPR_OFF);          /* bank A */
    uint16_t *b = (uint16_t *)(theme_mem + BANK_B_OFF + PL_SPR_OFF);
    *a = FAKE_COLOR; *b = PL_COLOR;
}

static orb_layer_tab_t T;   /* zero-init: every row free, every gen 0 */

int main(void)
{
    orb_asset_set_text(text_stub);

    /* 1 — a half-installed hook is no hook: no bake, no crash */
    orb_layers_set_alloc(rec_alloc, NULL);
    orb_layers_set_alloc(NULL, rec_free);
    orb_layer_desc_t bar = mk(ORB_ASSET_HAND, ORB_ASSET_BOARD, 10, 20, 40, 40);
    bar.thick = 8; bar.color = 0xF81F;
    ck("no-hook refuses bake", orb_layers_add(&T, &bar) == -1,
       (int)orb_layers_refuses(&T));

    /* 2 — tier 1 needs no heap and lands the resolver's box */
    orb_layers_set_alloc(rec_alloc, rec_free);
    orb_layer_desc_t t1 = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 100, 100, 20, 30);
    t1.art.off = T1_OFF; t1.role = 2; t1.blend = 1;
    int h1 = orb_layers_add(&T, &t1);
    ck("tier1 added", h1 > 0, h1);
    ck("tier1 took no heap", g_alloc_n == 0, g_alloc_n);
    ck("count one", orb_layers_count(&T) == 1, orb_layers_count(&T));
    orb_layer_desc_t back;
    ck("get round-trips", orb_layers_get(&T, h1, &back)
       && back.art.off == T1_OFF && back.role == 2 && back.blend == 1, h1);
    gfx_paint_ctx_t jc[8];
    int n = orb_layers_jobs(&T, THEME, jc, 8);
    ck("tier1 job built", n == 1 && g_last_base == THEME, n);
    ck("tier1 col = payload + off", jc[0].col == (const uint16_t *)(THEME + T1_OFF), 0);
    ck("tier1 role and blend", jc[0].role == 2 && jc[0].bmode == 1, jc[0].bmode);
    ck("tier1 not rotated", !jc[0].rot, jc[0].rot);
    ck("tier1 pos is the box", jc[0].posx == 100 && jc[0].posy == 100, jc[0].posx);

    /* 3 — the HAND that turns: class default picks BAR, not RECT */
    int hb = orb_layers_add(&T, &bar);          /* gen unset, art.prim unset  */
    ck("hand added", hb > 0, hb);
    ck("bake took one plane", g_alloc_n == 1, g_alloc_n);
    n = orb_layers_jobs(&T, THEME, jc, 8);
    ck("two jobs, table order", n == 2 && g_last_base != THEME, n);
    /* job[1] is the bar: probe the plane through the ctx's own pointers. A BAR
     * leaves the row-0 alpha clear; a silent RECT would fill the box. */
    ck("bar plate is a bar", jc[1].alp[0] == 0 && jc[1].alp[18 * 40 + 20] != 0, 0);
    ck("bar color is the claim", jc[1].col[18 * 40 + 20] == 0xF81F, jc[1].col[18 * 40 + 20]);
    ck("baked row carries its base", (const uint8_t *)jc[1].col > (const uint8_t *)0x1000, 0);

    /* 4 — the override: same HAND, explicit RING, turns into a ring */
    orb_layer_desc_t ring = mk(ORB_ASSET_HAND, ORB_ASSET_APP, 200, 200, 30, 30);
    ring.gen_prim = ORB_PRIM_RING; ring.r = 14; ring.thick = 3; ring.role = 4;
    int hr = orb_layers_add(&T, &ring);
    n = orb_layers_jobs(&T, THEME, jc, 8);
    ck("override bakes a ring", hr > 0 && n == 3
       && jc[2].alp[0] == 0 && jc[2].alp[1 * 30 + 15] != 0, n);

    /* 5 — stale handles refuse and count, and del is generation, not index */
    uint32_t rf = orb_layers_refuses(&T);
    ck("del of a live row is true", orb_layers_del(&T, hb), 0);
    ck("del of the SAME handle is false", !orb_layers_del(&T, hb), 0);
    ck("stale set refuses", !orb_layers_set(&T, hb, &t1), 0);
    ck("stale get refuses", !orb_layers_get(&T, hb, &back), 0);
    ck("stale touch refuses", !orb_layers_touch(&T, hb), 0);
    ck("refusals counted", orb_layers_refuses(&T) >= rf + 3,
       (int)(orb_layers_refuses(&T) - rf));
    int hb2 = orb_layers_add(&T, &bar);
    ck("re-add mints a NEW handle", hb2 != hb && hb2 > 0, hb2);
    ck("old handle still dead", !orb_layers_get(&T, hb, &back), 0);
    ck("handle gen is the row's counter", (unsigned)(hb2 >> 8) == 3u &&
       (unsigned)(hb2 & 0xFF) == (unsigned)(hb & 0xFF), hb2 >> 8);

    /* 6 — touch marks the OS's box: y 20..27 at x 10..49, minus SWEPT_PAD */
    g_mark_calls = 0;
    ck("touch accepted", orb_layers_touch(&T, hb2), 0);
    ck("touch marked once", g_mark_calls == 1, g_mark_calls);
    ck("mark is the swept box", g_my0 == 17 && g_my1 == 62 && g_mx0 == 7 && g_mx1 == 52,
       g_my0 * 1000 + g_my1);
    ck("mark not forced", g_mark_force == 0, g_mark_force);

    /* 7 — an OOM add is a refusal, and the row stays untouched */
    g_alloc_fail_next = 1;
    orb_layer_desc_t card = mk(ORB_ASSET_SPRITE, ORB_ASSET_BOARD, 5, 5, 10, 10);
    int ho = orb_layers_add(&T, &card);
    ck("OOM refuses", ho == -1, ho);
    uint32_t rf2 = orb_layers_refuses(&T);
    ck("OOM counted", rf2 > 0, (int)rf2);
    int cnt_before = orb_layers_count(&T);
    g_alloc_fail_next = 1;
    ck("OOM on set refuses too", !orb_layers_set(&T, hr, &card), 0);
    ck("OOM left the count alone", orb_layers_count(&T) == cnt_before,
       orb_layers_count(&T));
    orb_layers_get(&T, hr, &back);
    ck("refused set left the old art", back.r == 14 && back.art.tier == ORB_ASSET_APP,
       back.r);

    /* 8 — set swaps a plate and frees the old one exactly */
    int planes_before = g_alloc_n - g_free_n;
    orb_layer_desc_t ring2 = ring; ring2.art.y = 210; ring2.r = 10; ring2.thick = 4;
    ck("set accepted", orb_layers_set(&T, hr, &ring2), 0);
    ck("set moved exactly one plane", g_alloc_n - g_free_n == planes_before, planes_before);
    orb_layers_get(&T, hr, &back);
    ck("set replaced the art", back.art.y == 210 && back.r == 10, back.art.y);
    n = orb_layers_jobs(&T, THEME, jc, 8);
    ck("set re-mints the sprite record", n == 3 && jc[2].posy == 210, jc[2].posy);
    ck("handle survived its own set", orb_layers_touch(&T, hr), 0);
    int live_before = orb_layers_count(&T);
    long bytes_before = 0; /* planes are freed by wipe below; counted there */
    (void)bytes_before;

    /* 9 — wipe: rows gone, planes freed, generations kept */
    orb_layers_wipe(&T);
    ck("wipe empties the table", orb_layers_count(&T) == 0 && live_before > 0,
       orb_layers_count(&T));
    ck("wipe freed every plane", g_alloc_n == g_free_n, g_free_n);
    ck("wipe kept every generation", T.gen[0] == 1u && T.gen[1] == 3u && T.gen[2] == 1u,
       (int)T.gen[1]);
    orb_layers_add(&T, &bar);
    int hr3 = orb_layers_add(&T, &ring);   /* lands row 1: gen 3 -> 4 */
    ck("post-wipe handle differs from pre-wipe", hr3 != hr, hr3);
    ck("post-wipe handle carries the kept gen", hr3 == ((4 << 8) | 2), hr3);
    ck("pre-wipe handle stays dead", !orb_layers_touch(&T, hr), 0);
    orb_layers_del(&T, hr3);
    int hr4 = orb_layers_add(&T, &ring);
    ck("post-wipe del/add mints a new generation", hr4 != hr3, hr4);

/* 10 — table order, the show=0 row, and jmax */
    orb_layers_wipe(&T);
    orb_layer_desc_t a1 = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 10, 10, 8, 8);
    a1.art.off = 0x10; a1.role = 1;
    orb_layer_desc_t a2 = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 20, 20, 8, 8);
    a2.art.off = 0x20; a2.role = 2; a2.show = 0;
    orb_layer_desc_t a3 = mk(ORB_ASSET_RING, ORB_ASSET_BOARD, 30, 30, 20, 20);
    a3.art.prim = ORB_PRIM_RING; a3.r = 9; a3.thick = 2; a3.role = 3;
    int hA = orb_layers_add(&T, &a1), hB = orb_layers_add(&T, &a2);
    int hC = orb_layers_add(&T, &a3);
    n = orb_layers_jobs(&T, THEME, jc, 8);
    ck("hidden row kept live, out of the jobs", n == 2 && orb_layers_count(&T) == 3, n);
    ck("order is table order", jc[0].role == 1 && jc[1].role == 3, jc[0].role);
    ck("baked row answers with its own base", g_last_base != THEME, 0);
    const unsigned rf_j = orb_layers_refuses(&T);
    n = orb_layers_jobs(&T, THEME, jc, 1);
    ck("jmax is a promise", n == 1, n);
    /* The capacity refusal belongs to the compositor, not the ledger: the app
     * stayed inside ORB_LAYER_MAX and the caller's array ran out. Pinning this
     * is what keeps orb_layers.h's "dropped WITHOUT touching the refusal
     * counter" from rotting back into the claim it used to make. */
    ck("jmax exhaustion is not billed to the app",
       orb_layers_refuses(&T) == rf_j, (int)orb_layers_refuses(&T));
    n = orb_layers_jobs(&T, THEME, jc, 0);
    ck("jmax 0 builds nothing, still uncounted",
       n == 0 && orb_layers_refuses(&T) == rf_j, n);
    n = orb_layers_jobs(&T, NULL, jc, 8);
    ck("no payload: theme rows skip, baked stays", n == 1 && jc[0].role == 3, n);
    (void)hA; (void)hB;
    ck("del of a shown row frees its plane", orb_layers_del(&T, hC)
       && g_alloc_n == g_free_n + 0, g_free_n);

    /* 11 — every refusal counts */
    int before = (int)orb_layers_refuses(&T);
    orb_layer_desc_t bad = mk(ORB_ASSET_SPRITE, ORB_ASSET_NO_SLOT, 0, 0, 8, 8);
    ck("a failed resolve is not art", orb_layers_add(&T, &bad) == -1, (int)bad.art.tier);
    bad.art.tier = ORB_ASSET_OK;
    ck("empty box refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.art.w = -3; bad.art.tier = ORB_ASSET_BOARD;
    ck("negative box refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.art.w = 200; bad.art.h = 200;   /* 200*200*3+4 over the cap */
    ck("over-cap box refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.art.w = 8; bad.art.h = 8; bad.blend = 3;
    ck("blend out of range refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.blend = 0; bad.role = CLK_PAINT_ROLES;
    ck("role out of range refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.role = 0; bad.art.cls = 99;
    ck("class out of range refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.art.cls = ORB_ASSET_SPRITE; bad.gen_prim = ORB_PRIM_N;
    ck("generator out of range refuses", orb_layers_add(&T, &bad) == -1, 0);
    bad.gen_prim = 0xFF; bad.art.tier = ORB_ASSET_OK; bad.art.off = 0;
    ck("unset offset refuses", orb_layers_add(&T, &bad) == -1, 0);
    ck("refusals counted", (int)orb_layers_refuses(&T) >= before + 9,
       (int)orb_layers_refuses(&T) - before);
    ck("NULL table is quiet", orb_layers_count(NULL) == 0
       && orb_layers_refuses(NULL) == 0 && !orb_layers_touch(NULL, 1), 0);

    /* 12 — the promised registry-to-compositor seam, driven directly */
    uint8_t pbuf[4 + 20u * 20u * 3u];
    orb_prim_arg_t pa = { .prim = ORB_PRIM_RING, .w = 20, .h = 20,
                          .x = 5, .y = 5, .cx = 10, .cy = 10,
                          .r = 9, .thick = 3, .ang_10 = 0,
                          .color = 0x001F, .alpha = 255 };
    gfx_paint_ctx_t pxc;
    ck("prim ctx bakes and wraps", gfx_prim_ctx_init(&pxc, &pa, pbuf,
                                                     sizeof pbuf, 6)
       && pxc.w == 20 && pxc.col == (const uint16_t *)(pbuf + 4)
       && pxc.role == 6, (int)pxc.w);
    ck("prim ctx clamps a rogue role", gfx_prim_ctx_init(&pxc, &pa, pbuf,
                                                         sizeof pbuf, 200)
       && pxc.role == CLK_ROLE_AUX, pxc.role);
    ck("prim ctx refuses a short plane", !gfx_prim_ctx_init(&pxc, &pa, pbuf,
                                                            64u, 0), 0);
    pa.w = 0;
    ck("prim ctx refuses an empty box", !gfx_prim_ctx_init(&pxc, &pa, pbuf,
                                                           sizeof pbuf, 0), 0);

    /* alpha 0 bakes nothing: the plane was taken and must come straight back */
    bad.art.tier = ORB_ASSET_BOARD; bad.alpha = 0;
    long bal = g_live_bytes;
    ck("empty-coverage bake refuses", orb_layers_add(&T, &bad) == -1, 0);
    ck("failed bake leaked nothing", g_live_bytes == bal, (int)(g_live_bytes - bal));

    /* 13 — the commit hook's own contract, mirrored and counted. The shipped
     * function lives in orb_sched.c, which the host cannot compile; the python
     * lint pins that this body still matches the shipped one, so what these
     * vectors assert is the commit the app's rows actually reach. */
    const uint16_t *CANVAS = (const uint16_t *)0x1u;      /* recorded, never read */
    g_commit_calls = 0;
    ck("hook: no table, no commit",
       hook_mirror(NULL, THEME, CANVAS) == 0 && g_commit_calls == 0, g_commit_calls);
    orb_layer_tab_t H;
    memset(&H, 0, sizeof H);
    ck("hook: empty table, no commit",
       hook_mirror(&H, THEME, CANVAS) == 0 && g_commit_calls == 0, g_commit_calls);

    forge_payload();
    orb_layer_desc_t f1 = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 20, 20, 8, 8);
    f1.art.off = BANK_B_OFF + PL_SPR_OFF;             /* folded: payload-relative */
    int hh = orb_layers_add(&H, &f1);
    orb_layer_desc_t hid = f1; hid.show = 0;
    int h2 = orb_layers_add(&H, &hid);
    orb_layer_desc_t bar2 = mk(ORB_ASSET_HAND, ORB_ASSET_BOARD, 30, 30, 40, 40);
    bar2.thick = 8; bar2.color = 0xF81F;
    int h3 = orb_layers_add(&H, &bar2);
    ck("hook: three rows live", hh > 0 && h2 > 0 && h3 > 0
       && orb_layers_count(&H) == 3, orb_layers_count(&H));
    /* The canvas guard, with rows on the table so the count guard cannot be
     * what answered: no canvas means no marked row has anything to be restored
     * from, and the commit pass that paints into the mirror anyway is the crash
     * this guard exists to refuse. */
    ck("hook: no canvas, no commit",
       hook_mirror(&H, THEME, NULL) == 0 && g_commit_calls == 0, g_commit_calls);
    int nj = hook_mirror(&H, THEME, CANVAS);
    ck("hook: ONE commit, shown rows only", g_commit_calls == 1 && nj == 2, nj);
    ck("hook: the commit was handed the canvas", g_commit_canvas == CANVAS, 0);
    ck("hook: the array is ORB_LAYER_JOB_MAX deep",
       ORB_LAYER_JOB_MAX == ORB_LAYER_MAX, ORB_LAYER_JOB_MAX);
    int nj2 = hook_mirror(&H, THEME, CANVAS);
    ck("hook: one commit per pass", g_commit_calls == 2 && nj2 == nj, g_commit_calls);
    ck("hook: the row count never moved", orb_layers_count(&H) == 3,
       orb_layers_count(&H));

    /* 14 — the art.off base convention, measured rather than assumed. The hook
     * hands orb_layers_jobs() the MAPPED PAYLOAD; a tier-1 row therefore draws
     * from payload + art.off, with nothing added. So a bundle element's
     * catalogue offset — which orb_elems.c passes through verbatim, and which
     * orb_clock3.c proves is contiguous from ZERO inside its own section's bank
     * — must arrive folded. The second vector is the same row unfolded: it draws
     * the OTHER bank's planted word, which is the wrong-pixels failure Commit 3
     * owes, and the reason the fix is the resolver's and not a + blob_off in
     * the hook (a hook that added the clock's blob_off would break every
     * non-clock app's rows, which are already payload-relative). */
    gfx_paint_ctx_t jc2[ORB_LAYER_JOB_MAX];
    int n2 = orb_layers_jobs(&H, THEME, jc2, ORB_LAYER_JOB_MAX);
    ck("base: a folded off draws the planted plane",
       n2 == 2 && jc2[0].col == (const uint16_t *)(THEME + BANK_B_OFF + PL_SPR_OFF)
       && *jc2[0].col == PL_COLOR, n2);
    orb_layer_desc_t raw = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 20, 20, 8, 8);
    raw.art.off = PL_SPR_OFF;                        /* the catalogue's own number */
    orb_layer_tab_t R;
    memset(&R, 0, sizeof R);
    int rh = orb_layers_add(&R, &raw);
    int n3 = orb_layers_jobs(&R, THEME, jc2, ORB_LAYER_JOB_MAX);
    ck("base: the SAME off unfolded draws another bank's word",
       rh > 0 && n3 == 1 && jc2[0].col == (const uint16_t *)(THEME + PL_SPR_OFF)
       && *jc2[0].col == FAKE_COLOR, n3);
    ck("base: the payload base is used verbatim", g_last_base == THEME, 0);
    orb_layers_wipe(&H);
    orb_layers_wipe(&R);

    /* 15 — Migration 2, Commit 3: the raised ceiling and the inert table. The
     * shipped clock's bake claims every composite element it names — nine on
     * the analog bake the board carries (measured at boot) — so the table must
     * reach 16 and the hook must carry all of them in one commit. And the
     * build enters every row HIDDEN: Commit 4's tick pass wakes them from the
     * wall state, so hidden rows are the build instant, a suspend wipe's
     * aftermath, and every pose the gates refuse (an unposed hand, a comp with
     * no tile). A full table of hidden rows is live bookkeeping that builds
     * zero jobs — the exact shape the wake marks and the resume rebuild rest
     * on, mirrored here. */
    orb_layer_tab_t K;
    memset(&K, 0, sizeof K);
    for (int i = 0; i < ORB_LAYER_MAX; i++) {
        orb_layer_desc_t r = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 8 * i, 8 * i, 8, 8);
        r.art.off = BANK_B_OFF + PL_SPR_OFF;
        r.role = (uint8_t)(i % CLK_PAINT_ROLES);
        int hk = orb_layers_add(&K, &r);
        ck("cap: every row of the raised table claims", hk > 0, hk);
    }
    ck("cap: the whole table is live", orb_layers_count(&K) == ORB_LAYER_MAX,
       orb_layers_count(&K));
    ck("cap: the job ceiling is the table", ORB_LAYER_JOB_MAX == 16, ORB_LAYER_JOB_MAX);
    g_commit_calls = 0;
    int nk = hook_mirror(&K, THEME, CANVAS);
    ck("cap: one commit carries all sixteen rows",
       nk == ORB_LAYER_MAX && g_commit_calls == 1, nk);
    orb_layer_desc_t over = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 0, 0, 8, 8);
    over.art.off = 0x30;
    ck("cap: the row past the ceiling refuses, counted",
       orb_layers_add(&K, &over) == -1 && orb_layers_refuses(&K) == 1,
       (int)orb_layers_refuses(&K));
    orb_layers_wipe(&K);

    g_commit_calls = 0;
    orb_layer_tab_t V;
    memset(&V, 0, sizeof V);
    for (int i = 0; i < 3; i++) {
        orb_layer_desc_t r = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 8 * i, 8 * i, 8, 8);
        r.art.off = BANK_B_OFF + PL_SPR_OFF;
        r.show = 0;                                   /* exactly clock_view.c's add */
        ck("inert: a hidden row claims", orb_layers_add(&V, &r) > 0, 0);
    }
    int nv = hook_mirror(&V, THEME, CANVAS);
    ck("inert: three live hidden rows commit nothing",
       nv == 0 && g_commit_calls == 0 && orb_layers_count(&V) == 3, nv);

    /* The suspended-app wipe (orb_svc_app_suspended) empties the table but
     * keeps it and the generations; the dial's re-show re-claims without
     * deleting first. Mirrored here: wipe, then re-add the same three rows —
     * the ledger must land back at three live, zero refusals. */
    const uint32_t rf_v = orb_layers_refuses(&V);
    orb_layers_wipe(&V);
    ck("resume: the wipe emptied without refusing",
       orb_layers_count(&V) == 0 && orb_layers_refuses(&V) == rf_v,
       (int)orb_layers_refuses(&V));
    for (int i = 0; i < 3; i++) {
        orb_layer_desc_t r = mk(ORB_ASSET_SPRITE, ORB_ASSET_OK, 8 * i, 8 * i, 8, 8);
        r.art.off = BANK_B_OFF + PL_SPR_OFF;
        r.show = 0;
        ck("resume: the row re-claims after the wipe", orb_layers_add(&V, &r) > 0, 0);
    }
    ck("resume: the ledger is back to the bake's count",
       orb_layers_count(&V) == 3 && orb_layers_refuses(&V) == rf_v,
       orb_layers_count(&V));
    orb_layers_wipe(&V);

    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail ? 1 : 0;
}
'''

# ---- build ------------------------------------------------------------------
def build(tmp, layers_text=None, assets_text=None):
    os.makedirs(tmp, exist_ok=True)
    shim = os.path.join(tmp, "shim")
    os.makedirs(shim, exist_ok=True)
    open(os.path.join(shim, "esp_err.h"), "w").write(SHIM_ESP_ERR)
    open(os.path.join(shim, "lvgl.h"), "w").write(SHIM_LVGL)
    lsrc = LAYERS_C
    if layers_text is not None:
        lsrc = os.path.join(tmp, "orb_layers.c")
        open(lsrc, "w").write(layers_text)     # mutant lands first on -I path
    asrc = ASSETS_C
    if assets_text is not None:
        asrc = os.path.join(tmp, "orb_assets.c")
        open(asrc, "w").write(assets_text)
    hac = os.path.join(tmp, "harness.c")
    open(hac, "w").write(HARNESS_C)
    exe = os.path.join(tmp, "harness")
    cc = subprocess.run(
        ["cc", "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Wno-unused-parameter",
         "-Werror", "-I", shim, "-I", tmp, "-I", P("gfx"), "-I", P("assets"),
         "-I", P("app"), "-I", ROOT, "-I", P(""), "-I", os.path.join(ROOT, "assets"),
"-I", os.path.join(ROOT, "main", "art"),
         "-o", exe, hac, lsrc, asrc, "-lm"],
        capture_output=True, text=True)
    if cc.returncode:
        raise SystemExit("harness build failed:" + NL + cc.stderr[-3000:])
    return exe


def run(exe):
    r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    if r.returncode < 0:
        return r.returncode, "FAIL harness died (signal)\n"
    return r.returncode, r.stdout


# ---- the mirror lints: the stubs must still mirror what ships ---------------
def lint(tmp, out):
    lay = open(LAYERS_C, encoding="utf-8").read()
    layh = open(LAYERS_H, encoding="utf-8").read()
    gfx = open(GFX_C, encoding="utf-8").read()
    gfxh = open(GFX_H, encoding="utf-8").read()
    svch = open(SVC_H, encoding="utf-8").read()
    svc = open(P("app", "orb_svc.c"), encoding="utf-8").read()
    fails = []
    def pin(cond, why):
        if not cond:
            fails.append(why)
    # The harness's ctx stub mirrors the shipped gate and plane arithmetic.
    pin("if (!gfx_sprite_drawable(spr)) return false;" in gfx,
        "gfx_paint_ctx_init no longer gates on gfx_sprite_drawable — the "
        "harness stub mirrors a rule that left the building")
    pin("(const uint16_t *)(base + spr->color_off)" in gfx
        and "spr->color_off" in HARNESS_C,
        "the shipped colour-plane arithmetic moved; the stub still copies the old")
    pin("(double)ang_10 * (M_PI / 1800.0)" in gfx and "1800.0" in HARNESS_C,
        "the swept box's angle scale moved in orb_gfx.c; the stub still "
        "copies 1800")
    # The table header stays the boundary it claims to be.
    pin('#include "lvgl.h"' not in layh and '#include "orb_gfx.h"' not in layh,
        "orb_layers.h now includes a compositor header — the app boundary leaks")
    pin("typedef struct orb_layer_desc " in layh
        and "typedef struct orb_layer_desc      orb_layer_desc_t;" in svch,
        "orb_svc.h's forward typedef and orb_layers.h's definition no longer "
        "name the same struct tag")
    pin("typedef struct orb_layer_tab " in layh
        and "typedef struct orb_layer_tab       orb_layer_tab_t;" in svch,
        "the table's tag disagrees between the boundary and the module")
    # The frozen four keep the signature the ABI table calls with.
    for sig in ("int  (*layer_add)(orb_app_t *, const orb_layer_desc_t *);",
                "void (*layer_set)(orb_app_t *, int h, const orb_layer_desc_t *);",
                "void (*layer_touch)(orb_app_t *, int h, const struct _lv_area_t *);",
                "void (*layer_del)(orb_app_t *, int h);"):
        pin(sig in svch, "orb_svc_t's frozen layer signature moved: " + sig[:40])
    # The unset trap is a rule about a byte, and the byte is linted here.
    pin("d->gen_prim != 0xFFu" in lay and "ORB_PRIM_N" in lay,
        "the gen_prim sentinel or its bound left orb_layers.c — the field-zero "
        "trap (0 is RECT) is only closed while both are here")
    pin("orb_asset_class_prim" in lay,
        "row_gen no longer falls through to the registry's class default: the "
        "HAND-becomes-RECT bug is back")
    # ---- Migration 2, Commit 2: the commit hook, its getter, its constant ----
    # The hook itself cannot compile here (orb_sched.c drags lvgl/display/esp
    # headers), so the harness mirrors its body and the mirror is pinned to the
    # shipped text the same way the ctx stub above is.
    layh_def = "#define ORB_LAYER_JOB_MAX ORB_LAYER_MAX"
    pin(layh_def in layh,
        "ORB_LAYER_JOB_MAX is gone or no longer the table's own row count: the "
        "commit array is sized by a number the table cannot reach")
    pin("const orb_layer_tab_t *orb_svc_layer_table(const orb_app_t *a);" in svch,
        "orb_svc.h no longer declares the read door the commit hook calls")
    gm = re.search(r"const orb_layer_tab_t \*orb_svc_layer_table\(const orb_app_t \*a\)\n"
                   r"\{(.*?)\n\}", svc, re.S)
    pin(gm is not None, "orb_svc_layer_table is gone from orb_svc.c")
    if gm:
        gb = gm.group(1)
        pin("row_of(a)" in gb and "->layers" in gb,
            "orb_svc_layer_table no longer answers through row_of(): the getter "
            "left the registry's own slot arithmetic and can now name a stranger")
        pin("calloc" not in gb and "heap_caps" not in gb,
            "orb_svc_layer_table allocates: a scheduler read is now the thing "
            "that spends an app's PSRAM table — Commit 2's whole no-create rule")
    sc = open(P("gfx", "orb_sched.c"), encoding="utf-8").read()
    pin("static gfx_paint_ctx_t s_lay_jc[ORB_LAYER_JOB_MAX];" in sc,
        "the commit hook's job array left file scope — at ORB_LAYER_JOB_MAX (16) "
        "deep it is a kilobyte of the LVGL task's stack per flush, which is the "
        "one thing clock_view.c's retired hook never spent")
    hm = re.search(r"void orb_sched_commit_layers\(const orb_app_t \*active\)\n"
                   r"\{(.*?)\n\}", sc, re.S)
    pin(hm is not None, "orb_sched_commit_layers is gone from orb_sched.c")
    if hm:
        hb = hm.group(1)
        for stmt in ("const orb_layer_tab_t *t = orb_svc_layer_table(active);",
                     "if (!t || orb_layers_count(t) == 0) return;",
                     "if (!s_canvas) return;",
                     "int nj = orb_layers_jobs(t, art_bank_ptr(), s_lay_jc, ORB_LAYER_JOB_MAX);",
                     "if (nj) gfx_commit(nj, s_lay_jc, s_canvas);"):
            pin(stmt in hb,
                "the shipped commit hook no longer reads `" + stmt + "` — the "
                "harness's hook_mirror mirrors a body that moved")
        pin("gfx_paint_ctx_t jc[" not in hb,
            "the commit hook allocates its job array on the stack again — the "
            "array belongs beside the function (see the file-scope pin above)")
        pin("blob_off" not in hb and "orb_theme_geo" not in hb,
            "the commit hook grew a blob_off term: the base fix belongs at the "
            "resolver, and a hook-side add breaks every non-clock app's rows")
    # Migration 2, Commit 4 moved the call site from the tick to the display
    # flush hook THIS TU registers — the weld point where every mark of the
    # frame is in the bank and no DMA has read a row yet. The tick-side call
    # this pin replaced was Commit 2's interim; the orphan rule is the same
    # one, and the hook must carry the glass gate the retired clock hook
    # bequeathed (without it, boot's first commit welds the dial's pending
    # marks over the menu — the ghost that hook's header comment documents).
    mhook = re.search(r"static void sched_flush_commit\(void\)\n\{(.*?)\n\}",
                      sc, re.S)
    pin(mhook is not None,
        "the flush hook Commit 4 registered is gone from orb_sched.c")
    if mhook:
        pin("if (!orb_sched_running()) return;" in mhook.group(1) and
            "orb_sched_commit_layers(orb_app_current());" in mhook.group(1),
            "sched_flush_commit lost its glass gate or the commit call: the "
            "hook is landed and orphaned, which is the consumer count "
            "orb_layers.h records")
    pin("display_set_commit_hook(sched_flush_commit);" in sc,
        "orb_sched_start no longer registers the layer-table flush hook")
    tick_region = ""
    if "static void tick_timer_cb" in sc:
        tick_region = sc.split("static void tick_timer_cb")[1].split(
            "report_fps_if_due")[0]
    pin("orb_sched_commit_layers(orb_app_current());" not in tick_region,
        "the tick calls the layer-table commit again — the interim Commit 2 "
        "call site is back beside the flush hook, and two welds cannot share "
        "one mark set")
    pin("void orb_sched_commit_layers(const orb_app_t *active);"
        in open(P("gfx", "orb_sched.h"), encoding="utf-8").read(),
        "orb_sched.h no longer declares the commit pass the flush hook calls")
    # And the four shipped facts the folded-base vectors above rest on. A move
    # here does not break a vector; it invalidates what the vectors MEAN, which
    # is why Commit 3 is not allowed to trust them unread.
    conv = [("art/orb_elems.c",  "out->off      = sp.off;",
             "orb_elems.c no longer hands the catalogue's off back verbatim — the "
             "fold may already live there, and Commit 2's finding needs a re-read"),
            ("art/orb_clock3.c", "uint64_t at = 0;",
             "orb_clock3.c's catalogue walk no longer starts at zero — section-bank "
             "relative is no longer the shipped reading"),
            ("art/orb_clock3.c", "if (sp.off != at)",
             "orb_clock3.c no longer requires a contiguous catalogue from the bank's "
             "start, so 'off is inside the section's bank' is an assumption again"),
            ("art/orb_theme.c",  "s_geo.blob_off = io.out_art_off - SLOT_ART_OFF;",
             "orb_theme.c's blob_off is no longer the section bank's offset INTO the "
             "mapped payload — the disagreement this commit reported may have moved"),
            ("clock/clock_view.c", "s_art = art_bank_ptr() + orb_theme_geo()->blob_off;",
             "clock_view.c's blob() is no longer payload+blob_off — the classic "
             "path still routes its LVGL descriptors through it, and the fold "
             "proved here is what lets the table path not need it"),
            ("assets/orb_assets.h", "uint32_t off;        /* tier 1: offset into the theme's art payload",
             "orb_asset_t.off is no longer documented as payload-relative — the "
             "convention the commit hook depends on has changed its own mind")]
    for f, anchor, why in conv:
        pin(anchor in open(P(f), encoding="utf-8").read(), why)
    # The broker wipes at exactly the two doors a free cap has, and no third.
    for where in ("orb_svc_forget_row", "orb_svc_app_suspended"):
        m2 = re.search(r"(?:esp_err_t|void|static \w+|int|bool|uint32_t) +" + where + r"\(", svc)
        body = svc[m2.start():].split("\n}")[0] if m2 else ""
        pin("orb_layers_wipe" in body, where + " no longer wipes the layer table")
    m2 = re.search(r"(?:esp_err_t|void|static \w+|int|bool|uint32_t) +orb_svc_set_grant\(", svc)
    body = svc[m2.start():].split("\n}")[0] if m2 else ""
    pin("orb_layers_wipe" not in body,
        "orb_svc_set_grant wipes layers again: CAP_UI is free and revoking a "
        "free cap is refused above, so the wipe would be code for a door that "
        "does not exist")
    pin("orb_layers_set_alloc(svc_layers_alloc, heap_caps_free)" in svc,
        "orb_svc_init no longer installs the PSRAM allocator")
    pin('"gfx/orb_layers.c"' in open(os.path.join(P(""), "CMakeLists.txt")).read(),
        "orb_layers.c fell out of the component's SRCS")
    pin("gfx_prim_ctx_init" in lay and "gfx_prim_ctx_init" in layh,
        "the promised bake+ctx entry is gone — orb_assets.h still documents it")
    return fails

# ---- the mutant battery: rewrite the SHIPPED text, rebuild, rerun -----------
# Each entry is (name, [(old, new)]) over orb_layers.c's own words. A mutant
# that escapes means a vector that does not mean what it says.
MUTATIONS = [
    ("handle row index off by one",
     [("const int   i   = (h & 0xFF) - 1;", "const int   i   = (h & 0xFF);")]),
    ("add never bumps the generation",
     [("if (++t->gen[idx] == 0) t->gen[idx] = 1;", "if (t->gen[idx] == 0) t->gen[idx] = 1;")]),
    ("class default replaced by RING",
     [("return orb_asset_class_prim((orb_asset_class_t)d->art.cls);",
       "return ORB_PRIM_RING;")]),
    ("failed bake keeps the plane",
     [("            if (plane) s_free(plane);             /* OOM or an empty plate: the",
       "            if (0) s_free(plane);                 /* OOM or an empty plate: the")]),
    ("wipe resets the generations",
     [("        t->used[i]  = 0;", "        t->used[i]  = 0; t->gen[i] = 0;")]),
    ("jobs emit in reverse",
     [("for (int i = 0; i < ORB_LAYER_MAX && n < jmax; i++)",
       "for (int i = ORB_LAYER_MAX - 1; i >= 0 && n < jmax; i--)")]),
    ("baked rows painted from the caller's payload",
     [("const uint8_t *base = t->base[i] ? t->base[i] : theme_base;",
       "const uint8_t *base = theme_base;")]),
    ("set frees the old plane before baking",
     [("    if (t->plane[idx]) s_free(t->plane[idx]);     /* the old plate had its",
       "    if (d->art.tier < ORB_ASSET_APP && t->plane[idx]) s_free(t->plane[idx]); /*")]),
    ("table-full refuses uncounted",
     [("        t->refuses++;\n        return -1;", "        return -1;")]),
    ("set does not re-mint the sprite record",
     [("    t->row[idx]   = *d;\n    t->spr[idx]   = spr;",
       "    t->row[idx]   = *d;")]),
]



def main():
    tmp = tempfile.mkdtemp(prefix="orblayers")
    print("== mirror lints ==")
    lfails = lint(tmp, "")
    for f in lfails:
        print("FAIL " + f)
    exe = build(tmp)
    rc, out = run(exe)
    print(out.rstrip())
    vecs = out.count("ok ") + out.count("FAIL")
    print("== %d vectors, rc=%d, %d lint failures ==" % (vecs, rc, len(lfails)))
    if "--mutations" not in sys.argv:
        return 1 if (rc or lfails) else 0
    base_out_lines = set(l.split(" ", 1)[1] for l in out.splitlines() if l.startswith("FAIL"))
    bad = 0
    src = open(LAYERS_C, encoding="utf-8").read()
    for nm, pairs in MUTATIONS:
        m2 = src
        for old, new in pairs:
            assert m2.count(old) == 1, nm + ": anchor lost"
            m2 = m2.replace(old, new)
        try:
            mb = build(os.path.join(tmp, "mut_" + nm.replace(" ", "_")), m2)
        except SystemExit as e:
            print("caught: %-38s (build refused it)" % nm)
            continue
        mrc, mout = run(mb)
        mfails = set(l.split(" ", 1)[1] for l in mout.splitlines() if l.startswith("FAIL"))
        fresh = mfails - base_out_lines
        if mrc >= 0 and not fresh:
            print("MUTATION ESCAPED: %s — no new vector failure" % nm)
            bad += 1
        else:
            print("caught: %-38s (%s)" % (nm, sorted(fresh)[0][:60]))
    print("%d mutants, %d escaped" % (len(MUTATIONS), bad))
    return 1 if (bad or rc or lfails) else 0


if __name__ == "__main__":
    sys.exit(main())
