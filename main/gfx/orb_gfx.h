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
 * main/clock/clock_view.c unchanged, so the shipped dial composites exactly the
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
    uint8_t  role;                    /* paint bucket for the fps line */
    bool     rot;                     /* false == identity (ang == 0) path */
} gfx_paint_ctx_t;

/* The per-tick row mark set. The commit loop reads these directly through the
 * returned view (no call in its per-row loops); only marking and clearing go
 * through functions, so ownership of the state stays here. */
typedef struct {
    uint8_t  need[CLK_SCR_H];         /* rows to re-composite from the canvas  */
    uint8_t  force[CLK_SCR_H];        /* rows whose pixels never reached the panel */
    int32_t  x0[CLK_SCR_H];           /* the x-extent that must be restored    */
    int32_t  x1[CLK_SCR_H];
} gfx_rows_t;
/* ---- geometry ------------------------------------------------------------ */

/* LVGL's integer-degree sine, /32768 scale (pair it with >> 15, or scale up
 * yourself). */
int32_t gfx_trigo_sin(int32_t angle);

/* The rotation pair SHARED by paint_row's inverse map and the orbit chord
 * stamps: LVGL's lv_draw_sw_transform interpolation of gfx_trigo_sin at the
 * exact tenth of a degree, quantised the way LVGL quantises it (>> 5), LVGL's
 * /1024 scale (1.0 == 1024 — NOT a 16.16 value: callers that multiply these
 * into a >> 16 product must scale by 64, see clock_view.c's tourb_stamp_pose).
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

/* The chrome keep-out. A rect this pass names is the glass's topmost chrome —
 * the carousel chip today — and the direct-push compositor treats its rows as
 * not its to paint: LVGL draws the chrome on its own layer, and a hand that
 * swept through the band would otherwise re-composite over the chip's pixels
 * the same tick (bench, orb-A, 2026-10-29: "MENU" under the second hand shows
 * only in parts). Set it where the chrome is shown; clear it where the chrome
 * hides, and the clear re-marks the kept-out rows so the next commit repaints
 * them whole. One rect; a second chrome would make it a list, not a patch. */
void gfx_keepout_set(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
void gfx_keepout_clear(void);

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
void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas);

#ifdef __cplusplus
}
#endif
