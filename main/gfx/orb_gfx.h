#pragma once
/**
 * main/gfx/orb_gfx.c — the OS-owned compositor.
 *
 * Everything in here paints pixels: the rotation-and-blend maths that replaces
 * lv_draw_sw_transform() on the L4 path, the swept-box geometry that decides
 * which rows a layer dirties, the per-row mark set that drives the flush, and
 * the commit band that restores rows from a caller's canvas, paints the layer
 * list over them, and DMA-gates what actually changed.
 *
 * What it does NOT own is WHAT gets painted: the layer order, which sprite is
 * live, the complication and sub-dial job lists and the whole idea of a hand all
 * stay with the app that asks for them. P1 moved this code out of
 * main/stage/orb_stage.c unchanged, so the shipped dial composites exactly the
 * bytes it did before; the renames below are the only diff to the maths.
 *
 * Debt this carries deliberately, all owned by P6:
 *  - it still speaks clk_sprite_t (the clock's sprite record) rather than a
 *    neutral orb_layer_desc_t, and includes the generated clock_assets.h for
 *    CLK_SCR_W/H. The app boundary is drawn at the functions, not yet at the
 *    types.
 *  - the art bank base pointer is threaded in per call (gfx_paint_ctx_init's
 *    `base`) instead of being resolved here, so gfx stays free of art_bank.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"  /* CONFIG_ORB_DEBUG_HTTP: the span bench (absent on the host) */
#endif
#include "clk_types.h"   /* clk_sprite_t, CLK_SCR_W/H — P6 replaces both */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- paint roles -----------------------------------------------------------
 * Which layer a blend count belongs to on the fps line: 0..4 the theme's layer
 * slots (hour, minute, seconds/carriage, dial, counterweight), 5 the glass
 * overlay, 6 the baked complications, 7 the sub-dial pointers. */
#define CLK_PAINT_ROLES 8
#define CLK_ROLE_GLASS 5
#define CLK_ROLE_COMP  6
#define CLK_ROLE_AUX   7

/* Rows staged in internal RAM at once, and the column width the rotated pass
 * tiles across. Both measured, not chosen: see the block comment on s_band. */
#define CLK_COMMIT_BAND 8
#define CLK_PAINT_TILE  32

/* One layer's row-independent blend maths, resolved ONCE per commit. (The tag
 * is load-bearing since P5.5: orb_layers.h forward-declares this struct to
 * declare its job builder without dragging lvgl.h across the app boundary.) */
typedef struct gfx_paint_ctx {

    const uint16_t *col;
    const uint8_t  *alp;
    int32_t  bx1, by1, bx2, by2;      /* the (padded) swept box, screen coords */
    int32_t  posx, posy;
    int32_t  sxx, sxy, syx, syy;      /* rotated path: trig*64 */
    int32_t  x0_ups, y0_ups;          /* rotated path: LVGL's row origins */
    int32_t  xa_lim;                  /* static path: posx + w - 1 */
    uint32_t w, h;
    uint8_t  bmode;
    uint8_t  opa;                     /* the row's opacity over its art's own alpha:
                                       * LVGL's image opa, LV_OPA_MIX2 and all (theme-
                                       * text-plan T2); >= LV_OPA_MAX is the art as is */
    uint8_t  role;                    /* paint bucket for the fps line */
    bool     rot;                     /* false == identity (ang == 0) path */
    bool     smooth;                  /* rotated path samples 2x2 bilinear (EF_SMOOTH) */
    bool     drum;                    /* D43: a drum row (gfx_paint_ctx_drum)           */
    bool     solid;                   /* radar-plan §6.19: an unturned row whose every pixel
                                       * covers (a merged live layer): copied, not blended */
    int32_t  drum_cy, drum_r, drum_phase;   /* window centre row, radius, strip scroll px */
    float    drum_c;                  /* strip px per radian of the drum: h / 2pi       */
    int32_t  ang_10;                  /* the pose, kept for the span bench (forward map) */
    int16_t  pvx, pvy;                /* the pivot in sprite coords, likewise            */
} gfx_paint_ctx_t;

/* D43: turn an initialised (unrotated) ctx into a drum's: the strip rolls through the
 * window spr.w x 2r centred on the sprite's centre, the strip row `phase` under the
 * window's middle line. False when the drum cannot be drawn (no radius). */
bool gfx_paint_ctx_drum(gfx_paint_ctx_t *pc, const clk_sprite_t *spr, int32_t r, int32_t phase);
/* The screen box a drum's window covers (for marks). */
void gfx_drum_box(const clk_sprite_t *spr, int32_t r, lv_area_t *out);

/* The per-tick row mark set. The commit loop reads these directly through the
 * returned view (no call in its per-row loops); only marking and clearing go
 * through functions, so ownership of the state stays here. */
typedef struct {
    uint8_t  need[CLK_SCR_H];         /* rows to re-composite from the canvas  */
    uint8_t  force[CLK_SCR_H];        /* rows whose pixels never reached the panel */
    int16_t  x0[CLK_SCR_H];           /* the x-extent that must be restored    */
    int16_t  x1[CLK_SCR_H];
    /* §49.15: one interior gap the extent need not restore (h0 > h1: none) — a ring's hollow,
     * or the space between two hands on one row. Marks only ever over-cover, so a reader that
     * ignores the gap stays correct; the commit walk skips it. int16 throughout: screen
     * columns, and the same internal RAM the two int32 extents used. */
    int16_t  h0[CLK_SCR_H];
    int16_t  h1[CLK_SCR_H];
} gfx_rows_t;
/* ---- geometry ------------------------------------------------------------ */

/* LVGL's integer-degree sine, /32768 scale (pair it with >> 15, or scale up
 * yourself). */
int32_t gfx_trigo_sin(int32_t angle);

/* The rotation pair SHARED by paint_row's inverse map and the orbit chord
 * stamps: LVGL's lv_draw_sw_transform interpolation of gfx_trigo_sin at the
 * exact tenth of a degree, quantised the way LVGL quantises it (>> 5), LVGL's
 * /1024 scale (1.0 == 1024 — NOT a 16.16 value: callers that multiply these
 * into a >> 16 product must scale by 64, see orb_stage.c's tourb_stamp_pose).
 * Stamps and painter must consume exactly these numbers: any other pair (the
 * whole-degree table, or a different scale) displaces the stamped pose from the
 * painted one, and rows outside the stamped spans are neither restored nor
 * repainted — ghost and missing ink that no fps counter can see. */
void gfx_rot_sin_cos(int32_t ang_10, int32_t *sinma, int32_t *cosma);

/* Bounding box of `spr` rotated by ang_10 (TENTHS of a degree) about its pivot,
 * clamped to the panel. SWEPT_PAD is part of the contract, not a detail: the
 * software transform blends its edge antialiasing past the geometric box, so an
 * app that marks the rows its ink touches has to widen its own spans by the same
 * pad the compositor's box carries — the tourbillon's orbit chords do exactly
 * that, and a mismatch ghosts the tail outside the restored rows. */
#define SWEPT_PAD 3
void gfx_swept_rect_box(const clk_sprite_t *spr, int32_t ang_10,
                        int32_t rx1, int32_t ry1, int32_t rx2, int32_t ry2,
                        lv_area_t *out);
void gfx_swept_box(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out);
void gfx_area_union(lv_area_t *a, const lv_area_t *b);

/* ---- blending and sprite records ----------------------------------------- */

/* One RGB565 pixel over the mirror's, byte-swapping at the store. bmode: 0
 * normal, 1 multiply (shadows), 2 screen (mapped to additive). */
void gfx_blend_px(uint16_t *d, uint16_t c, uint8_t a, int bmode);

uint32_t gfx_sprite_bytes(const clk_sprite_t *s);
bool     gfx_sprite_drawable(const clk_sprite_t *s);
void     gfx_fill_sprite_dsc(lv_image_dsc_t *d, const clk_sprite_t *s,
                             const uint8_t *base);

/* ---- the commit pass ----------------------------------------------------- */

/* Fill a paint context for `spr` at ang_10 over the art bank at `base`.
 * False = nothing drawable, so the caller drops the job rather than painting
 * garbage. */
bool gfx_paint_ctx_init(gfx_paint_ctx_t *pc, const clk_sprite_t *spr,
                        const uint8_t *base, int32_t ang_10, uint8_t role);

/* Composite one layer into ONE already-staged device row over [xa..xb],
 * intersected with the layer's swept box and never outside it: those pixels
 * already hold a correct composite and re-blending them would double-darken. */
void gfx_paint_row_ctx(const gfx_paint_ctx_t *pc, int32_t y,
                       int32_t xa, int32_t xb, uint16_t *row);

/* The staging band, in INTERNAL RAM (plain .bss — PSRAM is exactly what this
 * pass must stop touching so often). */
uint16_t *gfx_band(int r);

/* Mark rows dirty for the next commit, growing each row's restored x-extent.
 * force=1 says the pixels reached the mirror but never the panel, so the row
 * DMAes regardless of the byte-compare. */
void gfx_mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1, bool force);
const gfx_rows_t *gfx_rows(void);
void gfx_rows_clear(void);
bool gfx_commit_needed(void);
void gfx_commit_consumed(void);

/* The chrome keep-out. A rect named here is the glass's topmost chrome — the
 * carousel chip, the map credit — and the direct-push compositor treats its
 * pixels as not its to paint: LVGL draws the chrome on its own layer, and a hand
 * that swept through the band would otherwise re-composite over the chip's
 * pixels the same tick (bench, orb-A, 2026-10-29: "MENU" under the second hand
 * shows only in parts). Set it where the chrome is shown; clear it where the
 * chrome hides, and the clear re-marks the kept-out rect so the next commit
 * repaints it whole. One rect a chrome, by its slot. */
enum { GFX_KO_CHIP, GFX_KO_CREDIT, GFX_KO_N };
void gfx_keepout_set(int slot, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void gfx_keepout_clear(int slot);

/* Hook phase timings and per-role blend counts, for the fps line. Totals are
 * monotonic; gfx_role_snapshot() returns one second's deltas and keeps its own
 * baselines, so the reporter owns no paint state. */
void gfx_hook_phases(uint64_t *rest_us, uint64_t *paint_us, uint64_t *cmp_us);
void gfx_role_snapshot(uint32_t *drb, uint64_t *dru);

/* A synthetic layer (the DISPLAY_BENCH ink) that must paint inside the timed
 * paint window so the CAD hook/px fields carry its cost, not hide it. */
typedef void (*gfx_band_hook_fn)(int32_t y0, int nr);
void gfx_set_band_hook(gfx_band_hook_fn fn);

/* Re-composite every marked row: restore the row's span from `canvas` (the
 * plate's own pixels, byte-swapped into mirror order), paint the `nj` layers
 * over it in the order given, then write back and DMA only the rows that came
 * out different. Consumes the mark set. */
/* radar-plan §6.27: the next commit carries a live layer's swap; its blip (the commit's
 * time and its push, us) is what the last one cost, 0 before the first. */
void gfx_swap_note(void);
uint32_t gfx_swap_blip_us(void);
void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas);

#if CONFIG_ORB_DEBUG_HTTP
/* ---- the ink-span bench (/api/v1/debug/dial) -------------------------------
 *
 * A rotated layer is marked and painted by its swept BOX today: every pixel of
 * the box is walked, inverse-mapped and tested, and 3/4 of them land on
 * transparent texels. The bench replaces the box with the layer's INK per screen
 * row: each 8-row strip of the sprite's alpha plane is reduced to its ink extent
 * once per sprite, forward-mapped to a quad at the pose, and the quad cut into one
 * [x0, x1] per screen row, padded so the painter's nearest/bilinear sample can
 * never fall outside it.
 *
 *   mode 0  off: the shipped compositor, byte for byte
 *   mode 1  marks use the spans and the paint clips to them
 *   mode 2  marks use the spans, the paint clips, AND the clipped-away pixels are
 *           painted too and every blend there is counted as an escape. Output is
 *           the shipped output; escapes > 0 means the spans are not conservative.
 *
 * no_glass drops the CLK_ROLE_GLASS job: what the commit would cost if the bake
 * folded the glass into the background frames. Visibly wrong while it is set. */
typedef struct {
    int16_t y0, y1;                   /* rows with ink; y0 > y1 = none          */
    int16_t x0[CLK_SCR_H], x1[CLK_SCR_H];
    int16_t h0[CLK_SCR_H], h1[CLK_SCR_H];  /* §49.15: an interior gap with no ink (h0 > h1: none) */
} gfx_spans_t;
/* nearest_shadow samples the still/shadow rows (role 3) nearest instead of the
 * bilinear the bake granted them. */
void gfx_bench_set(int spans_mode, bool no_glass, bool nearest_shadow, int aa_seconds);
int  gfx_bench_spans(void);
/* The spans of `pc`'s pose. False when the job is not span-able (static, drum). */
bool gfx_spans_of(const gfx_paint_ctx_t *pc, const gfx_spans_t **out);
/* Mark `pc`'s pose by its spans instead of its box. False = mark the box. */
bool gfx_mark_spans(const gfx_paint_ctx_t *pc);
typedef struct {
    uint64_t commits, restored_px, iter_box_px, iter_span_px, escapes, span_us;
    uint64_t rest_us, paint_us, cmp_us;
    uint64_t role_blend[CLK_PAINT_ROLES], role_us[CLK_PAINT_ROLES];
    uint64_t hist[6];                 /* commits by cost: <10, <20, <40, <60, <90, >=90 ms */
    uint32_t max_us;                  /* dearest commit since the last read (read resets) */
} gfx_bench_totals_t;
void gfx_bench_totals(gfx_bench_totals_t *o);

/* ---- pre-paint (plan §49.8) -------------------------------------------------
 *
 * Two prepared planes stand in for the canvas: PF holds every SLOW job composited over
 * the background (glass included), PB the jobs below the lowest FAST job (the seconds
 * row and its shadow twin). A commit then copies PF and repaints live only inside the
 * fast jobs' spans, from PB upward, so a tick costs the fast hand and nothing else. A gear
 * frame's PF/PB rows are built AHEAD, from its rects, in the slack a paced present waits
 * out (gfx_prep_idle), and swapped in when the frame lands. Mode 2 also re-composites
 * every row the shipped way and counts the pixels that differ (must stay 0). */
typedef struct { int16_t x, y, w, h; const uint16_t *src; } gfx_prep_rect_t;   /* src: LE, stride w */
void gfx_prep_mode(int mode);                     /* 0 off, 1 on, 2 on + exactness check */
/* radar-plan §6.27: or'd into the mode, the two front planes alone (868 KB, not 1.7 MB): a
 * plan that builds no gear frame ahead (an app's sweep over its live layers). */
#define GFX_PREP_LEAN 4
int  gfx_prep_on(void);                           /* the mode, GFX_PREP_LEAN included */
/* radar-plan §6.27: a slow job's pixels were replaced (a live layer's new picture, its
 * alpha now at `alp`) under the same pose, row y's changes in [x0[y], x1[y]] (none where
 * x0 > x1; the whole glass when x0 is NULL). The rows go stale together: the next commit
 * composes them all from the new picture, one blip, never a slice at a time; the slack
 * rebuilds the planes after. False when pre-paint is not holding the job's planes (the
 * caller damages the rows as before). */
bool gfx_prep_swap(const int16_t *x0, const int16_t *x1, const uint8_t *alp);
/* radar-plan §2.9: a slow job's pixels changed under a pose that did not (the live layer):
 * rebuild the box from them and repaint it this tick. A no-op while pre-paint is off. */
void gfx_prep_stale(int32_t y0, int32_t y1, int32_t x0, int32_t x1);
/* The canvas just took frame `frame`'s rects; `next` are the rects of the frame after. */
void gfx_prep_landed(int frame, const gfx_prep_rect_t *cur, int ncur,
                     int next, const gfx_prep_rect_t *nxt, int nnext);
/* Build ahead until `deadline_us`; false when there is nothing left to build. */
bool gfx_prep_idle(int64_t deadline_us);
typedef struct {
    uint64_t rows_ahead, rows_sync, swaps_ready, swaps_late, rebuilds, mismatch_px, checked_px;
    uint64_t live_px, copy_px;
    uint64_t swaps, promoted;                         /* §6.27: live swaps, jobs made fast */
} gfx_prep_stats_t;
void gfx_prep_stats(gfx_prep_stats_t *o);
#endif

#ifdef __cplusplus
}
#endif
