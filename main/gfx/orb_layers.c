/*
 * main/gfx/orb_layers.c — the per-app layer table; see the header for the model.
 *
 * The whole of P5.5's policy is a handful of rules over a fixed row array: a
 * handle outlives its row only as far as its generation does; a row that owns
 * heap gives it back on every path that ends the row; a set that will not bake
 * changes nothing; and the order jobs come out is the order they went in. The
 * registry (orb_assets.c) owns what an art answer MEANS and the bake owns how
 * pixels are produced; this file moves neither — it decides who gets a row.
 *
 * No LVGL include and no ESP include by design: the gate and the NVS ledger are
 * orb_svc.c's, the fonts and the blend maths are orb_gfx.c's, and what is here
 * has to compile on the host for the suite to drive it. The two gfx functions
 * this file calls are declared extern below, exactly as orb_gfx.h prototypes
 * them; the suite stubs them and the target links the real ones.
 */
#include "orb_layers.h"

#include <string.h>

/* The one compositor header. It drags lvgl.h with it, and the host suite
 * shims that with the two typedefs this file's calls touch — cheaper than
 * hand-copying prototypes here, which is exactly the drift §9.7 exists to
 * stop. The externs this file USED to carry (gfx_mark_rows,
 * gfx_paint_ctx_init) now come from the header that prototypes them. */
#include "orb_gfx.h"

/* The allocator hook, installed once by orb_svc_init before any app runs. A
 * board that never installs one takes no heap and bakes nothing: tier 1 still
 * works, and every tier 2/3 add is a counted refusal rather than a crash. */
static orb_layers_alloc_fn s_alloc;
static orb_layers_free_fn  s_free;

void orb_layers_set_alloc(orb_layers_alloc_fn alloc, orb_layers_free_fn free_fn)
{
    s_alloc = alloc;
    s_free  = (alloc) ? free_fn : NULL;          /* half a hook is no hook */
}


/* ---- handles ---------------------------------------------------------------
 * (gen << 8) | (idx + 1). The +1 so a zero byte never means row zero, and the
 * gen in the byte above it so a del followed by an add of the same row hands
 * out a DIFFERENT number: the stale handle stays dead, which is the whole
 * reason the encoding is not just the index. gen 0 is "never issued", so the
 * first add of a fresh row starts at 1. */
static int h_make(uint8_t gen, int idx)  { return ((int)gen << 8) | (idx + 1); }

/* Decode and check a handle against the table. idx is written only on success. */
static bool h_resolve(const orb_layer_tab_t *t, int h, int *idx)
{
    if (h <= 0 || h >> 16) return false;         /* 0 refuses, negatives too,
                                                  * and nothing lives above a
                                                  * generation byte */
    const int   i   = (h & 0xFF) - 1;
    const uint8_t gen = (uint8_t)(h >> 8);
    if (i < 0 || i >= ORB_LAYER_MAX) return false;
    if (!t->used[i] || !gen || t->gen[i] != gen) return false;
    *idx = i;
    return true;
}

/* ---- the descriptor's own rules -------------------------------------------
 * A tier that is not one of the three answers is a resolve that FAILED being
 * handed over as art: pass the number through (the app's own log line will
 * carry it) and refuse. The class byte is checked the same way, against the
 * registry's own bound rather than a re-typed one. */
static bool tier_is_art(uint16_t tier)
{
    return tier == ORB_ASSET_OK || tier == ORB_ASSET_APP || tier == ORB_ASSET_BOARD;
}

/* The generator a row will bake with: the descriptor's override if it set one
 * (0xFF = it did not — 0 is RECT, a real generator, and an unset field must
 * never pick one silently for a HAND), else the answer's own prim if the tier
 * carried one, else the class default from the registry. A TEXT/STATUS row
 * without a string is a caller bug, and the bake below will refuse it exactly
 * as loudly as orb_assets.c does. */
static orb_prim_t row_gen(const orb_layer_desc_t *d)
{
    if (d->gen_prim != 0xFFu) return (orb_prim_t)d->gen_prim;
    if (d->art.tier >= ORB_ASSET_APP && d->art.prim < ORB_PRIM_N
                                   && d->art.prim != 0)
        return (orb_prim_t)d->art.prim;
    return orb_asset_class_prim((orb_asset_class_t)d->art.cls);
}

/* ---- the row machinery ----------------------------------------------------
 * One validate, one bake-into-plane, one commit. Used by add and by set alike.
 * A row's placement is the same recipe for both art tiers: top-left from the
 * descriptor when it gave one, the resolver's box otherwise; pivot the
 * descriptor's when it gave one, the art's when it had one, plate centre
 * otherwise; centre = top-left + pivot — exactly what gfx_paint_ctx_init
 * expects a record to carry. */
static int16_t pick(int16_t prefer, int16_t fallback)
{
    return (prefer != 0) ? prefer : fallback;
}

/* False when the descriptor cannot become a row. Every reason is the app's
 * mistake; the refusal count is what says so. */
static bool desc_ok(const orb_layer_desc_t *d)
{
    if (!tier_is_art(d->art.tier)) return false;
    if (!orb_asset_class_known(d->art.cls)) return false;
    if (d->art.w <= 0 || d->art.h <= 0) return false;
    if (d->blend > 2) return false;               /* 0..2 is every blend mode   */
    if (d->role >= CLK_PAINT_ROLES) return false; /* counter bound (orb_gfx.h)  */
    if (d->gen_prim != 0xFFu && d->gen_prim >= ORB_PRIM_N) return false;
    if (d->art.tier >= ORB_ASSET_APP) {
        if (!s_alloc) return false;               /* no heap wired: no bakes    */
        if (orb_asset_prim_bytes((uint32_t)d->art.w, (uint32_t)d->art.h)
            > ORB_LAYER_PLANE_MAX) return false;  /* over-cap box               */
    } else if (d->art.off == 0) {
        return false;                             /* color_off 0 is the unset
                                                  * signature: a theme row with
                                                  * it would draw nothing and
                                                  * read as a broken theme      */
    }
    return true;
}

/* Bake `d`'s plate into the row-owned plane and finish the record: the bake
 * owns the pixels and the box; the app's placement and blend claims land on
 * top of it. False = nothing baked, nothing to keep. */
static bool bake_into(const orb_layer_desc_t *d, uint8_t *plane, clk_sprite_t *spr)
{
    orb_prim_arg_t a = {
        .prim   = row_gen(d),
        .w      = (uint16_t)d->art.w,
        .h      = (uint16_t)d->art.h,
        .x      = pick(d->x, d->art.x),
        .y      = pick(d->y, d->art.y),
        .r      = d->r,
        .thick  = d->thick,
        .ang_10 = d->ang_10,
        .color  = d->color,
        .alpha  = d->alpha,
        .text   = d->text,
    };
    /* BAR/RING centre is in PLATE coordinates — the bake applies cx/cy to the
     * pixel loop's x,y, not to the screen placement. The plate centre is the
     * honest default; an explicit descriptor pivot moves it within the plate. */
    a.cx = pick(d->pivotx, (int16_t)(d->art.w / 2));
    a.cy = pick(d->pivoty, (int16_t)(d->art.h / 2));
    if (!orb_asset_prim_bake(&a, plane, spr)) return false;

    spr->blend = d->blend;
    spr->show  = d->show ? 1 : 0;
    if (d->pivotx || d->pivoty) {                 /* the bake centred it; an
                                                  * explicit pivot moves both  */
        spr->pivotx  = pick(d->pivotx, (int16_t)(d->art.w / 2));
        spr->pivoty  = pick(d->pivoty, (int16_t)(d->art.h / 2));
        spr->centerx = (int16_t)(spr->offx + spr->pivotx);
        spr->centery = (int16_t)(spr->offy + spr->pivoty);
    }
    return true;
}

/* The tier-1 record, built from the resolver's answer: geometry only. Pixels
 * belong to the bundle, and the commit caller hands over its base. */
static void theme_spr(const orb_layer_desc_t *d, clk_sprite_t *spr)
{
    memset(spr, 0, sizeof *spr);
    spr->w         = (uint16_t)d->art.w;
    spr->h         = (uint16_t)d->art.h;
    spr->offx      = pick(d->x, d->art.x);
    spr->offy      = pick(d->y, d->art.y);
    spr->pivotx    = pick(d->pivotx, pick(d->art.pivotx, (int16_t)(d->art.w / 2)));
    spr->pivoty    = pick(d->pivoty, pick(d->art.pivoty, (int16_t)(d->art.h / 2)));
    spr->centerx   = (int16_t)(spr->offx + spr->pivotx);
    spr->centery   = (int16_t)(spr->offy + spr->pivoty);
    spr->blend     = d->blend;
    spr->show      = d->show ? 1 : 0;
    spr->color_off = d->art.off;
}

/* ---- the four app calls -------------------------------------------------- */

int orb_layers_add(orb_layer_tab_t *t, const orb_layer_desc_t *d)
{
    if (!t || !d) return -1;
    int idx = -1;
    for (int i = 0; i < ORB_LAYER_MAX; i++) if (!t->used[i]) { idx = i; break; }
    if (idx < 0 || !desc_ok(d)) {                 /* full or not-a-layer: one
                                                  * count, one answer, no row  */
        t->refuses++;
        return -1;
    }

    void *plane = NULL;
    clk_sprite_t spr;
    if (d->art.tier >= ORB_ASSET_APP) {
        const uint32_t need = orb_asset_prim_bytes((uint32_t)d->art.w,
                                                   (uint32_t)d->art.h);
        plane = s_alloc(need);
        if (!plane || !bake_into(d, (uint8_t *)plane, &spr)) {
            if (plane) s_free(plane);             /* OOM or an empty plate: the
                                                  * row never happened, and no
                                                  * byte of it stands around.  */
            t->refuses++;
            return -1;
        }
    } else {
        theme_spr(d, &spr);
    }

    t->row[idx]  = *d;
    t->spr[idx]  = spr;
    t->plane[idx] = plane;
    t->base[idx] = (const uint8_t *)plane;        /* NULL for a theme row: the
                                                  * commit's payload answers   */
    t->used[idx] = 1;
    t->n_live++;
    if (++t->gen[idx] == 0) t->gen[idx] = 1;      /* a wrap must not revive a
                                                  * handle the app let go of   */
    return h_make(t->gen[idx], idx);
}

bool orb_layers_set(orb_layer_tab_t *t, int h, const orb_layer_desc_t *d)
{
    int idx;
    if (!t || !d || !h_resolve(t, h, &idx)) { if (t) t->refuses++; return false; }
    if (!desc_ok(d)) { t->refuses++; return false; }

    /* Bake first, into memory the row does not own yet: a set that fails
     * mid-pixels would otherwise leave half a plate under a live handle, and
     * "half an update" is precisely the stale-art failure this whole table is
     * a response to. Only the finished row replaces the old one. */
    void *plane = NULL;
    clk_sprite_t spr;
    if (d->art.tier >= ORB_ASSET_APP) {
        const uint32_t need = orb_asset_prim_bytes((uint32_t)d->art.w,
                                                   (uint32_t)d->art.h);
        plane = s_alloc(need);
        if (!plane || !bake_into(d, (uint8_t *)plane, &spr)) {
            if (plane) s_free(plane);
            t->refuses++;
            return false;
        }
    } else {
        theme_spr(d, &spr);
    }

    if (t->plane[idx]) s_free(t->plane[idx]);     /* the old plate had its
                                                  * last commit at this line   */
    t->row[idx]   = *d;
    t->spr[idx]   = spr;
    t->plane[idx] = plane;
    t->base[idx]  = (const uint8_t *)plane;
    return true;
}

bool orb_layers_get(const orb_layer_tab_t *t, int h, orb_layer_desc_t *out)
{
    int idx;
    if (!t || !out || !h_resolve(t, h, &idx)) return false;
    *out = t->row[idx];
    return true;
}

bool orb_layers_touch(orb_layer_tab_t *t, int h)
{
    int idx;
    if (!t || !h_resolve(t, h, &idx)) { if (t) t->refuses++; return false; }
    lv_area_t box;
    gfx_swept_box(&t->spr[idx], t->row[idx].ang_10, &box);
    gfx_mark_rows(box.y1, box.y2, box.x1, box.x2, false);
    return true;
}

bool orb_layers_del(orb_layer_tab_t *t, int h)
{
    int idx;
    if (!t || !h_resolve(t, h, &idx)) { if (t) t->refuses++; return false; }
    if (t->plane[idx]) s_free(t->plane[idx]);
    t->plane[idx] = NULL;
    t->base[idx]  = NULL;
    t->used[idx]  = 0;
    t->n_live--;
    ++t->gen[idx];                                /* the handle dies HERE, so a
                                                  * re-add of this row cannot
                                                  * be reached by the caller
                                                  * that just let it go        */
    return true;
}

/* ---- the OS side --------------------------------------------------------- */

void orb_layers_wipe(orb_layer_tab_t *t)
{
    if (!t) return;
    for (int i = 0; i < ORB_LAYER_MAX; i++) {
        if (t->plane[i]) s_free(t->plane[i]);
        t->plane[i] = NULL;
        t->base[i]  = NULL;
        t->used[i]  = 0;
    }
    t->n_live = 0;
    /* gen[] stands: the point of a wipe is that the app's held handles stay
     * dead, and resetting the counters is how a dead handle wakes up holding
     * somebody else's row. */
}

int orb_layers_count(const orb_layer_tab_t *t) { return t ? t->n_live : 0; }

uint32_t orb_layers_refuses(const orb_layer_tab_t *t)
{
    return t ? t->refuses : 0;
}

int orb_layers_jobs(const orb_layer_tab_t *t, const uint8_t *theme_base,
                    struct gfx_paint_ctx *jc, int jmax)
{
    if (!t || !jc || jmax <= 0) return 0;
    int n = 0;
    for (int i = 0; i < ORB_LAYER_MAX && n < jmax; i++) {
        if (!t->used[i] || !t->spr[i].show) continue;
        const uint8_t *base = t->base[i] ? t->base[i] : theme_base;
        if (!base) continue;                      /* a theme row with no bundle
                                                  * mapped is a dial with no
                                                  * theme: draw nothing, and
                                                  * nothing is what it draws.  */
        if (gfx_paint_ctx_init(&jc[n], &t->spr[i], base, t->row[i].ang_10,
                               t->row[i].role)) {
            jc[n].smooth = t->row[i].smooth != 0;
            n++;                                  /* undrawable is skipped, never
                                                  * guessed at: the job list is
                                                  * the frame's contract.       */
        }
    }
    return n;
}

/* The compositor-side fallback entry the registry header has promised: bake
 * one primitive into the caller's plane, then wrap it in a ctx exactly as a
 * theme sprite would be wrapped — the one code path D22 said existed, so a
 * generated plate is counted by the same role buckets the dial's hands are.
 * No new geometry lives here: the bake owns the pixels (orb_assets.c), the
 * ctx owns the blend maths (orb_gfx.c), and this is the seam between them. */
bool gfx_prim_ctx_init(gfx_paint_ctx_t *pc, const orb_prim_arg_t *a,
                       uint8_t *plane, uint32_t plane_cap, uint8_t role)
{
    if (!a || !plane || !pc) return false;
    if (a->prim >= ORB_PRIM_N) return false;
    const uint32_t need = orb_asset_prim_bytes(a->w, a->h);
    if (need == 0 || need > plane_cap) return false;

    clk_sprite_t spr;
    if (!orb_asset_prim_bake(a, plane, &spr)) return false;
    /* The bake lands the plate at a->x/a->y with a centre pivot; the caller's
     * role claim is what joins it to the fps line. Clamped, never trusted: the
     * counter arrays are CLK_PAINT_ROLES wide and an app's byte cannot widen
     * them. */
    return gfx_paint_ctx_init(pc, &spr, plane, a->ang_10,
                              role < CLK_PAINT_ROLES ? role : CLK_ROLE_AUX);
}

