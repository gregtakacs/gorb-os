/* main/gfx/orb_gfx.c — the OS-owned compositor: the rotation and blend maths
 * that replaces lv_draw_sw_transform() on the L4 path, the swept-box geometry,
 * the per-row mark set, and the commit band that restores, paints and DMA-gates.
 *
 * Extracted from main/stage/orb_stage.c in P1 with the maths unchanged: the
 * renames and the row-state consolidation are the only diff, so the shipped dial
 * composites the same bytes it did before the move.
 *
 * The hook lives in this TU for a reason: gfx_paint_row_ctx is called per layer
 * per row (~1500 times a commit on the tourbillon face), and a cross-TU call
 * there is measurable. The app builds the job list; gfx_commit paints it.
 */

#include "orb_gfx.h"
#include "orb_live_work.h"    /* radar-plan §7.7: the build-ahead is the live worker's job */

#include <math.h>
#include "esp_attr.h"           /* EXT_RAM_BSS_ATTR: pre-paint's zone tables */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "orb_glass.h"

/* The rotation pair SHARED by paint_row's inverse map and the orbit chord
 * stamps: LVGL's lv_draw_sw_transform interpolation of trigo_sin at the exact
 * tenth of a degree, quantised the way LVGL quantises it (>> 5), LVGL's /1024
 * scale (1.0 == 1024 — NOT a 16.16 value: callers that multiply these into a
 * >> 16 product must scale by 64, see tourb_stamp_pose). Stamps and painter
 * must consume exactly these numbers: any other pair (the whole-degree table,
 * or a different scale) displaces the stamped pose from the painted one, and
 * rows outside the stamped spans are neither restored nor repainted — ghost
 * and missing ink that no fps counter can see. */
void gfx_rot_sin_cos(int32_t ang_10, int32_t *sinma, int32_t *cosma)
{
    const int32_t tr_ang = -(ang_10 % 3600);
    if (tr_ang == 0) { *sinma = 0; *cosma = 1024; return; }   /* LVGL's bit-exact case */
    const int32_t low = tr_ang / 10, rem = tr_ang - low * 10;
    const int32_t s1 = gfx_trigo_sin(low),      s2 = gfx_trigo_sin(low + 1);
    const int32_t c1 = gfx_trigo_sin(low + 90), c2 = gfx_trigo_sin(low + 91);
    *sinma = ((s1 * (10 - rem) + s2 * rem) / 10) >> 5;
    *cosma = ((c1 * (10 - rem) + c2 * rem) / 10) >> 5;
}
uint32_t gfx_sprite_bytes(const clk_sprite_t *s)
{
    return (uint32_t)s->w * (uint32_t)s->h * 3u;   /* colour w*h*2 + alpha w*h */
}

bool gfx_sprite_drawable(const clk_sprite_t *s)
{
    return s->show && s->w > 0 && s->h > 0 && s->color_off != 0;
}

/* Build an LVGL RGB565A8 descriptor for a cropped sprite: planar colour plane
 * (w*h*2, stride w*2) with the alpha plane (w*h, stride w) appended in the blob. */
void gfx_fill_sprite_dsc(lv_image_dsc_t *d, const clk_sprite_t *s,
                              const uint8_t *base)
{
    memset(d, 0, sizeof(*d));
    d->header.magic      = LV_IMAGE_HEADER_MAGIC;
    d->header.cf         = LV_COLOR_FORMAT_RGB565A8;
    d->header.flags      = 0;
    d->header.w          = s->w;
    d->header.h          = s->h;
    d->header.stride     = (uint32_t)s->w * 2u;
    d->data_size         = gfx_sprite_bytes(s);
    d->data              = base + s->color_off;
}
/* ---- rotated-layer invalidation ------------------------------------------
 * A hand rotates about a pivot that sits far outside its own box (the second
 * hand's ink is a slim sliver whose box is nowhere near the sub-dial centre it
 * swings around), so the pixels it paints this tick and uncovers next tick form
 * a wedge, and that wedge's bounding box walks tens of pixels outside the
 * widget's box as it sweeps.
 *
 * lv_image_set_rotation() does invalidate both the old and the new transformed
 * box, but every one of those calls routes through lv_obj_invalidate_area(),
 * which clips the area to the widget's own coords expanded by ext_draw_size --
 * and ext_draw_size is refreshed for the NEW angle only. The outgoing wedge's
 * anti-aliased fringe therefore lands outside the clip and is never repainted
 * from the plate. On the glass that is exactly the two artifacts we had:
 * sparkle pixels stranded where the hand used to be, and a hairline running
 * along the edge of the hand's rectangle.
 *
 * Pushing the union of both wedges in through the PARENT sidesteps the child
 * clip (the screen's box is the whole panel), so LVGL re-renders the plate under
 * the entire swept region before drawing the hand back over it. Padded by a few
 * pixels because the software transform blends its edge AA past the geometric
 * box, which is precisely the band the clip was dropping. SWEPT_PAD itself is in
 * the header, because the app's chord marking has to use the same value. */

void gfx_swept_rect_box(const clk_sprite_t *spr, int32_t ang_10,
                           int32_t rx1, int32_t ry1, int32_t rx2, int32_t ry2,
                           lv_area_t *out)
{
    /* ang_10 is TENTHS of a degree: deg = ang_10/10, rad = ang_10 * PI/1800.
     * (The divisor here was 3600, i.e. the box was built for HALF the angle:
     * right at 12 o'clock, progressively wrong as the hand swept, which is why
     * the hour hand lost its tip and the second hand looked clipped at a straight
     * edge wherever the box stopped short of the ink paint_layer maps onto.) */
    const double rad = (double)ang_10 * (M_PI / 1800.0);
    const double c = cos(rad), s = sin(rad);
    int32_t x1 = 0x7fffffff, y1 = 0x7fffffff;
    int32_t x2 = -0x7fffffff, y2 = -0x7fffffff;

    for (int i = 0; i < 4; i++) {
        const double cx = (i & 1) ? (double)rx2 : (double)rx1;
        const double cy = (i & 2) ? (double)ry2 : (double)ry1;
        const double dx = cx - (double)spr->pivotx;
        const double dy = cy - (double)spr->pivoty;
        /* lv_point_transform()'s convention, with the pivot pinned to the
         * sprite's screen centre (obj->coords = center - pivot). */
        const double px = (double)spr->centerx + (c * dx - s * dy);
        const double py = (double)spr->centery + (s * dx + c * dy);
        const int32_t ix = (int32_t)floor(px), iy = (int32_t)floor(py);
        if (ix < x1) x1 = ix;
        if (iy < y1) y1 = iy;
        if (ix > x2) x2 = ix;
        if (iy > y2) y2 = iy;
    }

    x1 -= SWEPT_PAD; y1 -= SWEPT_PAD;
    x2 += SWEPT_PAD; y2 += SWEPT_PAD;
    out->x1 = (lv_coord_t)(x1 < 0 ? 0 : x1);
    out->y1 = (lv_coord_t)(y1 < 0 ? 0 : y1);
    out->x2 = (lv_coord_t)(x2 > CLK_SCR_W - 1 ? CLK_SCR_W - 1 : x2);
    out->y2 = (lv_coord_t)(y2 > CLK_SCR_H - 1 ? CLK_SCR_H - 1 : y2);
}

void gfx_swept_box(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out)
{
    gfx_swept_rect_box(spr, ang_10, 0, 0, (int32_t)spr->w - 1, (int32_t)spr->h - 1, out);
}


void gfx_area_union(lv_area_t *a, const lv_area_t *b)
{
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
}
/* Rows this tick has dirtied and that the commit hook must re-composite from
 * scratch: background restored out of s_canvas, then every layer painted over
 * it in z-order.
 *
 * Two things are essential and were the source of the "hands missing /
 * trailing" bug:
 *  - the rows must ALSO be marked in the display's band set
 *    (display_mark_rows_clocked), or the mirror we repaint is never DMA'd and
 *    the glass keeps the old rows;
 *  - each marked row's whole painted span must be restored from the canvas
 *    first, or a layer that moved out of a pixel leaves it behind (ghost) and
 *    a layer that is re-painted blends over its own previous copy.
 * Restoring first also makes the hook idempotent, so the marks stay set for
 * the duration of the tick and are cleared at tick start, not at commit.
 *
 * Each row remembers the x-extent that must be restored: the union of every
 * pushed rect and every layer box that touched it. Narrower than the row, and
 * it can only grow within a tick, so a later mark never loses coverage.
 *
 * FORCE rows had their mirror content replaced outside the hook (the GIF
 * delta push writes mirror+canvas directly between commits), so the panel has
 * never seen those pixels: they always join the flush. The other marked rows
 * are candidates for the hook's byte-compare flush gate -- the orbit disc
 * re-composites ~450 rows a tick but only the wedge under the carriage comes
 * out different, and re-DMA-ing 130 KB of unchanged pixels every tick is what
 * capped sweep at 5 fps once the blend work itself was gone. */
static gfx_rows_t rowstate;
static bool      s_commit;      /* something marked rows since last tick  */

/* §49.15: one row's covered set is [*x0, *x1] less the gap [*h0, *h1] (h0 > h1: none). Add
 * [a, b]: merge the pieces, keep the widest gap that is left, and cover any other (a mark may
 * only over-cover). A ring's two sides, or two hands apart on one row, keep their gap. */
static inline void seg_add(int16_t *x0, int16_t *x1, int16_t *h0, int16_t *h1, int32_t a, int32_t b)
{
    int32_t lo[3], hi[3], n = 0;
    if (*h0 <= *h1) { lo[n] = *x0; hi[n++] = *h0 - 1; lo[n] = *h1 + 1; hi[n++] = *x1; }
    else            { lo[n] = *x0; hi[n++] = *x1; }
    int32_t ins = n;                                   /* keep the pieces sorted by lo */
    while (ins > 0 && lo[ins - 1] > a) { lo[ins] = lo[ins - 1]; hi[ins] = hi[ins - 1]; ins--; }
    lo[ins] = a; hi[ins] = b; n++;
    int32_t m = 0;
    for (int32_t i = 1; i < n; i++) {
        if (lo[i] <= hi[m] + 1) { if (hi[i] > hi[m]) hi[m] = hi[i]; }
        else { m++; lo[m] = lo[i]; hi[m] = hi[i]; }
    }
    *x0 = (int16_t)lo[0]; *x1 = (int16_t)hi[m];
    int32_t g0 = 1, g1 = 0;
    for (int32_t i = 0; i < m; i++)
        if (lo[i + 1] - hi[i] - 1 > g1 - g0 + 1) { g0 = hi[i] + 1; g1 = lo[i + 1] - 1; }
    *h0 = (int16_t)g0; *h1 = (int16_t)g1;
}

/* The pieces of a row's marked extent: one, or two either side of its gap. */
static inline int row_segs(int32_t y, int32_t *sa, int32_t *sb)
{
    sa[0] = rowstate.x0[y];
    if (rowstate.h0[y] <= rowstate.h1[y]) {
        sb[0] = rowstate.h0[y] - 1; sa[1] = rowstate.h1[y] + 1; sb[1] = rowstate.x1[y];
        return 2;
    }
    sb[0] = rowstate.x1[y];
    return 1;
}

static inline void row_add(int32_t y, int32_t a, int32_t b)
{
    if (!rowstate.need[y]) {
        rowstate.need[y] = 1;
        rowstate.x0[y] = (int16_t)a; rowstate.x1[y] = (int16_t)b;
        rowstate.h0[y] = 1; rowstate.h1[y] = 0;
        return;
    }
    seg_add(&rowstate.x0[y], &rowstate.x1[y], &rowstate.h0[y], &rowstate.h1[y], a, b);
}

void gfx_mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1,
                             bool force)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= CLK_SCR_H) y1 = CLK_SCR_H - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= CLK_SCR_W) x1 = CLK_SCR_W - 1;
    if (y1 < y0 || x1 < x0) return;

    bool any = false;
    for (int32_t y = y0; y <= y1; y++) {
        row_add(y, x0, x1);
        if (force) rowstate.force[y] = 1;
        any = true;
    }
    if (any) {
        s_commit = true;
        display_mark_rows_clocked(y0, y1);
    }
}
/* lv_trigo_sin(), copied from lv_math.c: bit-exact table + 32767 clamp. */
static const uint16_t s_sin0_90[91] = {
    0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
    5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580,  10126, 10668,
    11207, 11743, 12275, 12803, 13328, 13848, 14365, 14876, 15384, 15886,
    16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174, 20622,
    21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730,
    25102, 25466, 25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088,
    28378, 28660, 28932, 29197, 29452, 29698, 29935, 30163, 30382, 30592,
    30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928, 32052, 32166,
    32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763,
    32768
};

int32_t gfx_trigo_sin(int32_t angle)
{
    while (angle < 0)    angle += 360;
    while (angle >= 360) angle -= 360;
    int32_t ret;
    if (angle < 90)        ret =  s_sin0_90[angle];
    else if (angle < 180)  ret =  s_sin0_90[180 - angle];
    else if (angle < 270)  ret = -s_sin0_90[angle - 180];
    else                   ret = -s_sin0_90[360 - angle];
    return (ret == 32767) ? 32768 : (ret == -32767) ? -32768 : ret;
}

/* lv_color_16_16_mix_inlined(), copied from lv_draw_sw_blend_private.h, with one
 * change (D43 §5.4): 0x2008010 adds half a step (16) to each of the three packed
 * fields before the >> 5, so every field ROUNDS where LVGL's truncates. Truncation
 * pulled every blended texel about half a level down — the glass sat ~3 luma under
 * the preview — and left the crystal overlay's soft edges banded. Checked equal to a
 * per-field round on 200,000 inputs with no field overflowing. LVGL's own image
 * blends take the same rounding through main/gfx/orb_lv_blend.c (the custom-ASM
 * hooks), so the two paint paths still agree texel for texel. */
static inline uint16_t mix_16_16(uint16_t c1, uint16_t c2, uint8_t mix)
{
    if (mix == 255) return c1;
    if (mix == 0)   return c2;
    if (c1 == c2)   return c1;
    uint32_t mix5 = ((uint32_t)mix + 4) >> 3;
    uint32_t bg = (uint32_t)(c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    uint32_t fg = (uint32_t)(c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    uint32_t res = ((((fg - bg) * mix5 + 0x2008010u) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((res >> 16) | res);
}

/* Blend one sampled texel onto the mirror. Shared by the rotated and the
 * static loop in paint_row so the two can never drift apart. */
/* A row's opacity over a texel's coverage, as LVGL's image blend takes it
 * (orb_lv_blend.c's mix_mask_opa: LV_OPA_MIX2(mask, opa)); an opacity at or above
 * LV_OPA_MAX is LVGL's cover, the texel as it is. */
static inline uint8_t opa_mix(uint8_t a, uint8_t opa)
{
    return opa >= LV_OPA_MAX ? a : (uint8_t)(((uint32_t)a * opa) >> 8);
}

static inline void blend_px(uint16_t *d, uint16_t c, uint8_t a, int bmode)
{
    const uint16_t dest = __builtin_bswap16(*d);
    uint16_t out;
    if (bmode == 1) {                      /* MULTIPLY (shadow layers) */
        if (c == 0xffff) return;
        const uint16_t res =
            (uint16_t)((((dest >> 11) * (c >> 11) >> 5) << 11) |
                       ((((dest >> 5) & 0x3F) * ((c >> 5) & 0x3F) >> 6) << 5) |
                       (((dest & 0x1F) * (c & 0x1F)) >> 5));
        out = mix_16_16(res, dest, a);
    }
    else if (bmode == 2) {                 /* SCREEN (mapped to ADDITIVE) */
        if (c == 0x0000) return;
        const uint32_t dr = dest >> 11, dg = (dest >> 5) & 0x3F, db = dest & 0x1F;
        const uint32_t sr = c >> 11,    sg = (c >> 5) & 0x3F,    sb = c & 0x1F;
        const uint32_t r = (dr + sr < 31 ? dr + sr : 31) << 11;
        const uint32_t g = (dg + sg < 63 ? dg + sg : 63) << 5;
        const uint32_t b = db + sb < 31 ? db + sb : 31;
        out = mix_16_16((uint16_t)(r | g | b), dest, a);
    }
    else {                                 /* NORMAL */
        out = (a >= 255) ? c : mix_16_16(c, dest, a);
    }
    *d = __builtin_bswap16(out);
}
static uint32_t s_hk_role_blend[CLK_PAINT_ROLES];
static uint32_t s_hk_role_blend0[CLK_PAINT_ROLES];
static uint64_t s_hk_role_us[CLK_PAINT_ROLES];
static uint64_t s_hk_role_us0[CLK_PAINT_ROLES];
bool gfx_paint_ctx_init(gfx_paint_ctx_t *pc, const clk_sprite_t *spr,
                        const uint8_t *base, int32_t ang_10, uint8_t role)
{
    if (!gfx_sprite_drawable(spr)) return false;
    pc->role = role;
    pc->opa = LV_OPA_COVER;           /* the art as it is unless the job builder dims it */
    pc->smooth = false;               /* nearest unless the job builder says the row smooths */
    pc->drum = false;                 /* a drum only when the job builder makes it one */
    pc->solid = false;                /* solid only when the job builder says the row covers */

    pc->col = (const uint16_t *)(base + spr->color_off);
    pc->alp = base + spr->color_off
              + (uint32_t)spr->w * (uint32_t)spr->h * 2u;
    pc->w = (uint32_t)spr->w;
    pc->h = (uint32_t)spr->h;
    pc->posx = (int32_t)spr->centerx - (int32_t)spr->pivotx;
    pc->posy = (int32_t)spr->centery - (int32_t)spr->pivoty;
    pc->bmode = (uint8_t)spr->blend;

    /* The (padded) swept box is a superset of LVGL's transformed area;
     * out-of-image samples fall through the bounds test in paint_row_ctx. */
    lv_area_t box;
    gfx_swept_box(spr, ang_10, &box);
    pc->bx1 = box.x1; pc->by1 = box.y1;
    pc->bx2 = box.x2; pc->by2 = box.y2;

    pc->ang_10 = ang_10;
    pc->pvx = spr->pivotx;
    pc->pvy = spr->pivoty;
    pc->rot = (ang_10 % 3600 != 0);
    pc->sxx = pc->sxy = pc->syx = pc->syy = 0;
    pc->x0_ups = pc->y0_ups = 0;
    pc->xa_lim = pc->posx + (int32_t)spr->w - 1;
    if (!pc->rot) return true;

    /* Inverse-transform setup, copied from lv_draw_sw_transform(): scale is
     * fixed 256, so sxx..syy reduce to trig*64; x0_ups/y0_ups keep LVGL's
     * exact operation order (int32 products, truncating /256). The pair comes
     * from the same helper the orbit chord stamps use — see rot_sin_cos. */
    int32_t sinma, cosma;
    gfx_rot_sin_cos(ang_10, &sinma, &cosma);
    pc->sxx =  cosma * 64; pc->sxy = -(sinma * 64);
    pc->syx =  sinma * 64; pc->syy =  cosma * 64;
    const int32_t xin0 = -(int32_t)spr->pivotx, yin0 = -(int32_t)spr->pivoty;
    pc->x0_ups =
        (int32_t)(((cosma * xin0 - sinma * yin0) * 64) / 256)
        + (int32_t)spr->pivotx * 256 + 0x80;
    pc->y0_ups =
        (int32_t)(((sinma * xin0 + cosma * yin0) * 64) / 256)
        + (int32_t)spr->pivoty * 256 + 0x80;
    return true;
}

/* D43: a drum. The strip (spr.w x spr.h, one turn of the drum) rolls past a window
 * spr.w x 2r whose middle line sits on the sprite's centre. A screen line yy from that
 * middle sees the drum's surface at angle asin(yy / r), so it shows the strip row
 * phase + (h / 2pi) * asin(yy / r), wrapped: the figure under the middle reads flat and
 * the ones rolling over the top and under the bottom foreshorten exactly as a barrel
 * does. The shading over the curve is not here: it is screen-locked, so the bake folds
 * it into the still cover above the drum, and the drum itself costs one copy per pixel. */
void gfx_drum_box(const clk_sprite_t *spr, int32_t r, lv_area_t *out)
{
    out->x1 = (int32_t)spr->centerx - (int32_t)spr->pivotx;
    out->x2 = out->x1 + (int32_t)spr->w - 1;
    out->y1 = (int32_t)spr->centery - r;
    out->y2 = (int32_t)spr->centery + r - 1;
}

bool gfx_paint_ctx_drum(gfx_paint_ctx_t *pc, const clk_sprite_t *spr, int32_t r, int32_t phase)
{
    if (r < 2 || !pc->h) return false;
    lv_area_t box;
    gfx_drum_box(spr, r, &box);
    pc->drum = true;
    pc->rot = false;
    pc->smooth = false;
    pc->drum_cy = (int32_t)spr->centery;
    pc->drum_r = r;
    pc->drum_phase = phase;
    pc->drum_c = (float)pc->h / 6.28318531f;
    pc->posx = box.x1;
    pc->xa_lim = box.x2;
    pc->bx1 = box.x1; pc->by1 = box.y1;
    pc->bx2 = box.x2; pc->by2 = box.y2;
    return true;
}

static void paint_drum_row(const gfx_paint_ctx_t *pc, int32_t y, int32_t xa, int32_t xb,
                           uint16_t *row)
{
    const float yy = ((float)(y - pc->drum_cy) + 0.5f) / (float)pc->drum_r;
    if (yy <= -1.0f || yy >= 1.0f) return;
    const int32_t h = (int32_t)pc->h;
    int32_t sy = pc->drum_phase + (int32_t)floorf(pc->drum_c * asinf(yy) + 0.5f);
    sy %= h;
    if (sy < 0) sy += h;
    if (xa < pc->posx) xa = pc->posx;
    if (xb > pc->xa_lim) xb = pc->xa_lim;
    if (xa > xb) return;
    uint32_t n = 0, si = (uint32_t)sy * pc->w + (uint32_t)(xa - pc->posx);
    for (int32_t x = xa; x <= xb; x++, si++) {
        const uint8_t a = opa_mix(pc->alp[si], pc->opa);
        if (!a) continue;
        blend_px(&row[x], pc->col[si], a, pc->bmode);
        n++;
    }
    if (pc->role < CLK_PAINT_ROLES) s_hk_role_blend[pc->role] += n;
}

/* Composite one layer into ONE already-staged device row, over the caller's
 * [xa..xb] intersected with the layer's swept box (never outside it: those
 * pixels already hold a correct composite and re-blending them would
 * double-darken). Pure integer maths on internal-RAM pixels — everything that
 * does not depend on the row lives in paint_ctx_init. Blends are counted per
 * role (see s_hk_role_blend): one total cannot say WHICH layer owns the cost,
 * and that is the difference between staging the carriage's texels and
 * memoizing the static dial+glass pair. */
void gfx_paint_row_ctx(const gfx_paint_ctx_t *pc, int32_t y,
                          int32_t xa, int32_t xb, uint16_t *row)
{
    uint32_t n = 0;
    if (y < pc->by1 || y > pc->by2) return;
    if (xa < pc->bx1) xa = pc->bx1;
    if (xb > pc->bx2) xb = pc->bx2;
    if (xa > xb) return;
    if (pc->drum) { paint_drum_row(pc, y, xa, xb, row); return; }

    if (!pc->rot) {
        /* ---- static path (ang == 0): dial, glass, hub ----
         * A layer that never turns maps screen px to texel by a constant
         * offset (the identity case of LVGL's transform is exact: xs_int =
         * x - posx, ys_int = y - posy — verified by reducing the general
         * maths at sinma=0/cosma=1024). Per-pixel multiply chains, shifts and
         * bounds tests are pure waste here; clip the span up front instead.
         * A full-coverage semi-transparent "glass" overlay makes this the
         * single biggest line item in the commit (77% of its texels carry
         * real alpha), so it has to be exact AND cheap: ~1 us/px of real
         * blending otherwise caps the whole dial at 4 fps once the art is
         * full-coverage rather than a hub cap. */
        const int32_t sy = y - pc->posy;
        if (sy < 0 || sy >= (int32_t)pc->h) return;
        if (xa < pc->posx) xa = pc->posx;
        if (xb > pc->xa_lim) xb = pc->xa_lim;
        if (xa > xb) return;
        const uint16_t *col = pc->col;
        const uint8_t *alp = pc->alp;
        uint32_t si = (uint32_t)sy * pc->w + (uint32_t)(xa - pc->posx);
        if (pc->solid && pc->opa >= LV_OPA_MAX && pc->bmode == 0) {
            /* radar-plan §6.19: a row that covers is a copy into mirror order, the restore's
             * own cost, not a blend (the radar's map and rain, merged: 125 ms a full glass
             * as two blended layers) */
            const uint16_t *src = col + si;
            for (int32_t x = xa; x <= xb; x++) { const uint16_t v = *src++; row[x] = (uint16_t)((v >> 8) | (v << 8)); }
            n = (uint32_t)(xb - xa + 1);
            if (pc->role < CLK_PAINT_ROLES) s_hk_role_blend[pc->role] += n;
            return;
        }
        for (int32_t x = xa; x <= xb; x++, si++) {
            const uint8_t a = opa_mix(alp[si], pc->opa);
            if (!a) continue;
            blend_px(&row[x], col[si], a, pc->bmode);
            n++;
        }
        if (pc->role < CLK_PAINT_ROLES) s_hk_role_blend[pc->role] += n;
        return;
    }

    const uint16_t *col = pc->col;
    const uint8_t *alp = pc->alp;
    const int32_t y_abs = y - pc->posy;
    const int32_t xs_r = pc->x0_ups + (int32_t)(((int64_t)pc->sxy * y_abs) >> 8);
    const int32_t ys_r = pc->y0_ups + (int32_t)(((int64_t)pc->syy * y_abs) >> 8);

    if (pc->smooth) {
        /* Bilinear, for a row the bake granted EF_SMOOTH (the unified render path's
         * slice 5). The inverse map already carries 8 fractional bits; the nearest
         * path rounds them away through LVGL's +0x80 bias, so subtracting it back
         * gives the sample's true position, and its fraction weights the 2x2
         * neighbourhood. Coverage weights the colour (premultiplied), so a fringe
         * texel's colour cannot bleed past its own alpha, and texels outside the
         * plane count as transparent: the edge fades instead of clipping. Same
         * blend_px, same role counter, so the fps line prices the filter on the
         * layer that pays it. */
        const int32_t w = (int32_t)pc->w, h = (int32_t)pc->h;
        for (int32_t x = xa; x <= xb; x++) {
            const int32_t x_abs = x - pc->posx;
            const int32_t xf = xs_r + ((pc->sxx * x_abs) >> 8) - 0x80;
            const int32_t yf = ys_r + ((pc->syx * x_abs) >> 8) - 0x80;
            const int32_t ix = xf >> 8, iy = yf >> 8;
            if (ix < -1 || ix >= w || iy < -1 || iy >= h) continue;
            const uint32_t fx = (uint32_t)xf & 0xFFu, fy = (uint32_t)yf & 0xFFu;
            const uint32_t wt[4] = { (256u - fx) * (256u - fy), fx * (256u - fy),
                                     (256u - fx) * fy,          fx * fy };
            uint32_t asum = 0, rs = 0, gs = 0, bs = 0;
            for (int k = 0; k < 4; k++) {
                const int32_t tx = ix + (k & 1), ty = iy + (k >> 1);
                if (tx < 0 || tx >= w || ty < 0 || ty >= h) continue;
                const uint32_t si = (uint32_t)ty * pc->w + (uint32_t)tx;
                const uint32_t aw = wt[k] * alp[si];          /* <= 65536 * 255 */
                if (!aw) continue;
                const uint16_t c = col[si];
                asum += aw;
                rs += (aw >> 8) * (c >> 11);
                gs += (aw >> 8) * ((c >> 5) & 0x3Fu);
                bs += (aw >> 8) * (c & 0x1Fu);
            }
            const uint8_t a = opa_mix((uint8_t)((asum + 32768u) >> 16), pc->opa);
            if (!a) continue;
            const uint32_t d = asum >> 8;
            const uint16_t c = (uint16_t)(((rs / d) << 11) | ((gs / d) << 5) | (bs / d));
            blend_px(&row[x], c, a, pc->bmode);
            n++;
        }
        if (pc->role < CLK_PAINT_ROLES) s_hk_role_blend[pc->role] += n;
        return;
    }

    for (int32_t x = xa; x <= xb; x++) {
        const int32_t x_abs = x - pc->posx;
        const int32_t xs_int = (xs_r + ((pc->sxx * x_abs) >> 8)) >> 8;
        const int32_t ys_int = (ys_r + ((pc->syx * x_abs) >> 8)) >> 8;
        if (xs_int < 0 || xs_int >= (int32_t)pc->w ||
            ys_int < 0 || ys_int >= (int32_t)pc->h) continue;

        const uint32_t sidx = (uint32_t)ys_int * pc->w + (uint32_t)xs_int;
        const uint8_t a = opa_mix(alp[sidx], pc->opa);
        if (!a) {
            continue;                   /* 3/4 of the box: skip  */
        }
        blend_px(&row[x], col[sidx], a, pc->bmode);
        n++;
    }
    if (pc->role < CLK_PAINT_ROLES) s_hk_role_blend[pc->role] += n;
}
/* The commit's staging band, in INTERNAL RAM (plain .bss — PSRAM is exactly
 * what this pass must stop touching so often). The mirror is the panel's
 * source of truth: every byte that ever reached the glass was mirrored there
 * first, so the row still in the mirror IS a faithful "what the glass shows"
 * and the flush gate compares the staged composite against it in place — the
 * whole-page PSRAM snapshot buffer this replaces cost a read AND a write per
 * marked row on top of the compare, and 200 KB of the heap.
 *
 * It is a BAND of rows, not one, because of how the rotated pass reads its
 * texels. Measured cycles per blend_px on the tourbillon face (the "blend/s by
 * layer" line): the static glass holds at 82 cycles at every orbit angle, the
 * static dial at ~95, but the rotating carriage swings 111 -> 317 depending
 * only on where it is in the orbit. One device row of a rotated layer samples
 * a DIAGONAL through the colour and alpha planes (stride w*2 and w), so it
 * touches a fresh 32-byte PSRAM cache line per texel and never revisits one.
 * Walking a band of rows a few columns at a time keeps the source footprint
 * (tile_w + band) x (tile_w + band) small enough to live in the 32 KB cache
 * across the whole band, which is the difference between the carriage costing
 * ~55 ms and ~25 ms of a ~145 ms commit. */
/* Tile/band sizes measured on the tourbillon face, cycles per blend_px of the
 * rotating carriage across a whole orbit (32-px / 8-row): untiled 111/174/317
 * min/avg/max, tile 32 = 114/157/193, tile 16 = 121/161/194 — 16 is dearer
 * (per-tile call overhead starts to dominate) and 32 is the flattest, which is
 * the point: the angle-dependent cliff is gone. Static layers were unaffected
 * at 82 (glass) / 95 (dial) in every configuration — they read sequentially
 * and are already at the blend's ALU floor, so the tiling is rotated-only. */
static uint16_t s_band[CLK_COMMIT_BAND][CLK_SCR_W];

/* ---- the mark set, the band, the counters and the commit pass ------------
 * The commit hook body is here, not in the app, so the per-row layer calls stay
 * intra-TU and the app only ever decides WHAT to paint. */

const gfx_rows_t *gfx_rows(void) { return &rowstate; }

void gfx_rows_clear(void)
{
    memset(rowstate.need, 0, sizeof(rowstate.need));
    memset(rowstate.force, 0, sizeof(rowstate.force));
}

bool     gfx_commit_needed(void)   { return s_commit; }
void     gfx_commit_consumed(void) { s_commit = false; }
uint16_t *gfx_band(int r)          { return s_band[r]; }

/* Commit-hook phase breakdown (per-second deltas on the fps line, ms): rest =
 * take the mirror row + wipe the staging row back to plain canvas, paint = every
 * layer's blend traffic, cmp = the flush gate. The caller keeps its own
 * baselines; these are monotonic totals. */
static uint64_t s_hk_rest_us, s_hk_paint_us, s_hk_cmp_us;

/* ---- the chrome keep-out (see orb_gfx.h) -----------------------------------
 * The topmost chrome — today the carousel chip — is LVGL's to draw, and the
 * direct-push compositor knows nothing about its layer. A commit that restored
 * or painted through the chip's box would re-composite bare dial pixels over
 * the chip's, and the flush gate would DMA them, so the chrome vanished under
 * any hand that swept its band (bench, orb-A, 2026-10-29). The gate below is
 * the whole mechanism: rows outside the rect's y-range behave exactly as
 * before; a row inside it has its x-span cut around the rect, and the chip's
 * own pixels stay in the mirror untouched. */
static int32_t s_ko[GFX_KO_N][4];            /* x0, y0, x1, y1 */
static bool    s_ko_up[GFX_KO_N];
static bool    s_ko_on;                      /* any slot up */

void gfx_keepout_set(int slot, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (slot < 0 || slot >= GFX_KO_N) return;
    s_ko[slot][0] = x0; s_ko[slot][1] = y0; s_ko[slot][2] = x1; s_ko[slot][3] = y1;
    s_ko_up[slot] = true;
    s_ko_on = true;
}

void gfx_keepout_clear(int slot)
{
    if (slot < 0 || slot >= GFX_KO_N || !s_ko_up[slot]) return;
    s_ko_up[slot] = false;
    s_ko_on = false;
    for (int k = 0; k < GFX_KO_N; k++) s_ko_on = s_ko_on || s_ko_up[k];
    /* Whatever the commit skipped while the chrome stood has to come back: a
     * hand that moved across the rect during the peek still owns those pixels,
     * so re-mark the rect whole and the next commit repaints it. */
    gfx_mark_rows(s_ko[slot][1], s_ko[slot][3], s_ko[slot][0], s_ko[slot][2], false);
}

/* Is (x, y) under a standing chrome? Asked only for a row some rect crosses. */
static inline bool ko_hit(int32_t x, int32_t y)
{
    for (int k = 0; k < GFX_KO_N; k++)
        if (s_ko_up[k] && y >= s_ko[k][1] && y <= s_ko[k][3] && x >= s_ko[k][0] && x <= s_ko[k][2])
            return true;
    return false;
}
static inline bool ko_row(int32_t y)
{
    if (!s_ko_on) return false;
    for (int k = 0; k < GFX_KO_N; k++)
        if (s_ko_up[k] && y >= s_ko[k][1] && y <= s_ko[k][3]) return true;
    return false;
}
void gfx_hook_phases(uint64_t *rest_us, uint64_t *paint_us, uint64_t *cmp_us)
{
    *rest_us = s_hk_rest_us; *paint_us = s_hk_paint_us; *cmp_us = s_hk_cmp_us;
}

/* One second's worth of per-role blends and paint time, as deltas: the caller
 * prints them straight onto the fps line and owns no state of its own. */
void gfx_role_snapshot(uint32_t *drb, uint64_t *dru)
{
    for (int i = 0; i < CLK_PAINT_ROLES; i++) {
        drb[i] = s_hk_role_blend[i] - s_hk_role_blend0[i];
        dru[i] = s_hk_role_us[i] - s_hk_role_us0[i];
        s_hk_role_blend0[i] = s_hk_role_blend[i];
        s_hk_role_us0[i] = s_hk_role_us[i];
    }
}

static gfx_band_hook_fn s_band_hook;
void gfx_set_band_hook(gfx_band_hook_fn fn) { s_band_hook = fn; }

/* blend_px/mix_16_16 stay file-static on purpose: they are the per-pixel hot
 * loop gfx_paint_row_ctx inlines (the measured 185-232 ns/px path), and a real
 * call there would show up on the fps line. gfx_blend_px exists only for the
 * DISPLAY_BENCH synthetic ink, which pays the call and does not care. */
void gfx_blend_px(uint16_t *d, uint16_t c, uint8_t a, int bmode)
{
    blend_px(d, c, a, bmode);
}


#if CONFIG_ORB_DEBUG_HTTP
/* ---- the ink-span bench (orb_gfx.h) ----------------------------------------
 *
 * Two caches, both PSRAM, both keyed by the alpha plane's address (one plane
 * per sprite frame, and the bank is mapped for the dial's whole life):
 *   ink   the sprite's alpha plane reduced to [umin, umax] per 8-row strip, read
 *         once per sprite (the hands here are <= 56 x 272: ~15 KB of alpha);
 *   pose  the screen spans of one (plane, angle, placement), so a gear frame that
 *         re-composites under a hand standing still never recomputes them. */
#define INK_STRIP   8
#define INK_STRIPS  64                  /* h <= 512 */
#define INK_N       48                  /* the tracker's 15 plane rows pick among 49 frames */
/* A commit resolves every rotated job's spans first and paints after, holding a pointer
 * into this cache per job (gfx_commit's bsp[]). Twelve entries let the tracker's ~20 jobs
 * (its arm, 15 planes, the markers) evict entries already handed out, and the arm, resolved
 * first, painted through a plane's spans: gone, or slivers where the two crossed (Bronze,
 * 2026-11-06, 399,291 escapes a commit in spans=2). Room for a commit's 32 jobs and the
 * marks between commits, and a commit's own entries are pinned (spc_t.gen). */
#define SPC_N       72   /* plan §53: a commit's 64 jobs, and the marks between commits */

typedef struct {
    const uint8_t *alp;
    uint32_t w, h;
    int16_t  umin[INK_STRIPS], umax[INK_STRIPS];
    int16_t  gmin[INK_STRIPS], gmax[INK_STRIPS];   /* §49.15: the strip's widest inkless gap (gmin > gmax: none) */
} ink_t;
/* A gap narrower than this keeps the strip one quad: each side's quad grows by the pad and a
 * pixel, so a thin gap would only buy two quads that overlap again. */
#define INK_GAP_MIN 24
typedef struct {
    const uint8_t *alp;
    int32_t  ang, posx, posy;
    bool     smooth;
    uint32_t stamp;
    uint32_t gen;                       /* the commit that last used it: never evicted within it */
    gfx_spans_t sp;
} spc_t;

static volatile int  s_bx_mode = 1;     /* §49.15: ink spans are the shipped compositor; the bench may turn them off */
static volatile bool s_bx_noglass;
static ink_t   *s_ink;
static spc_t   *s_spc;
static uint32_t s_ink_next, s_spc_clock, s_spc_gen = 1;
static uint64_t s_bx_commits, s_bx_restored, s_bx_iter_box, s_bx_iter_span,
                s_bx_escape, s_bx_span_us;

static volatile bool s_bx_nearest;
static volatile int  s_bx_aa_sec;      /* §49.15: 1 the seconds row smoothed (the bake keeps a hot one nearest), 2 every shadow too */
static uint64_t s_bx_hist[6];
static uint32_t s_bx_max_us;

void gfx_bench_set(int spans_mode, bool no_glass, bool nearest_shadow, int aa_seconds)
{
    s_bx_mode = spans_mode < 0 ? 0 : (spans_mode > 2 ? 2 : spans_mode);
    s_bx_noglass = no_glass;
    s_bx_nearest = nearest_shadow;
    s_bx_aa_sec = aa_seconds;
}
int gfx_bench_spans(void) { return s_bx_mode; }

void gfx_bench_totals(gfx_bench_totals_t *o)
{
    o->commits = s_bx_commits;   o->restored_px = s_bx_restored;
    o->iter_box_px = s_bx_iter_box; o->iter_span_px = s_bx_iter_span;
    o->escapes = s_bx_escape;    o->span_us = s_bx_span_us;
    o->rest_us = s_hk_rest_us;   o->paint_us = s_hk_paint_us; o->cmp_us = s_hk_cmp_us;
    for (int i = 0; i < CLK_PAINT_ROLES; i++) {
        o->role_blend[i] = s_hk_role_blend[i];
        o->role_us[i]    = s_hk_role_us[i];
    }
    memcpy(o->hist, s_bx_hist, sizeof o->hist);
    o->max_us = s_bx_max_us;
    s_bx_max_us = 0;
}

static bool bx_alloc(void)
{
    if (s_ink && s_spc) return true;
    if (!s_ink) s_ink = heap_caps_calloc(INK_N, sizeof *s_ink, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_spc) s_spc = heap_caps_calloc(SPC_N, sizeof *s_spc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_ink && s_spc;
}

static const ink_t *ink_of(const gfx_paint_ctx_t *pc)
{
    for (int i = 0; i < INK_N; i++)
        if (s_ink[i].alp == pc->alp && s_ink[i].w == pc->w && s_ink[i].h == pc->h) return &s_ink[i];
    if (pc->h > INK_STRIP * INK_STRIPS) return NULL;
    ink_t *k = &s_ink[s_ink_next++ % INK_N];
    k->alp = pc->alp; k->w = pc->w; k->h = pc->h;
    const uint32_t ns = (pc->h + INK_STRIP - 1) / INK_STRIP;
    static uint8_t col[512];                          /* one strip's inked columns (w <= 512) */
    for (uint32_t st = 0; st < ns; st++) {
        int16_t lo = INT16_MAX, hi = -1;
        const uint32_t cw = pc->w < sizeof col ? pc->w : sizeof col;
        memset(col, 0, cw);
        for (uint32_t v = st * INK_STRIP; v < (st + 1) * INK_STRIP && v < pc->h; v++) {
            const uint8_t *a = pc->alp + (size_t)v * pc->w;
            for (uint32_t u = 0; u < pc->w; u++)
                if (a[u]) { if ((int16_t)u < lo) lo = (int16_t)u; if ((int16_t)u > hi) hi = (int16_t)u; if (u < cw) col[u] = 1; }
        }
        k->umin[st] = lo; k->umax[st] = hi;
        /* the widest run of inkless columns strictly inside [lo, hi]: a ring's hollow */
        int16_t g0 = 1, g1 = 0;
        if (hi >= lo && pc->w <= sizeof col) {
            for (int32_t u = lo + 1; u < hi; ) {
                if (col[u]) { u++; continue; }
                int32_t e = u;
                while (e + 1 < hi && !col[e + 1]) e++;
                if (e - u > g1 - g0) { g0 = (int16_t)u; g1 = (int16_t)e; }
                u = e + 1;
            }
        }
        if (g1 - g0 + 1 < INK_GAP_MIN) { g0 = 1; g1 = 0; }
        k->gmin[st] = g0; k->gmax[st] = g1;
    }
    return k;
}

/* One convex quad, cut into screen rows: for each row band [y, y+1) the x-range
 * the quad covers, taken from where its edges cross the band. Grows `o`. */
static inline float fmn(float a, float b) { return a < b ? a : b; }
static inline float fmx(float a, float b) { return a > b ? a : b; }
static inline int   flo(float v) { const int i = (int)v; return (v < (float)i) ? i - 1 : i; }

static void quad_rows(const float qx[4], const float qy[4], gfx_spans_t *o)
{
    /* Each edge once: its y-range and dx/dy, so a row costs multiplies only (the
     * S3's FPU has no divide, and a divide per edge per row was the whole cost). */
    float elo[4], ehi[4], ex0[4], ey0[4], ek[4];
    float ymn = qy[0], ymx = qy[0];
    for (int e = 0; e < 4; e++) {
        const int q = (e + 1) & 3;
        elo[e] = fmn(qy[e], qy[q]); ehi[e] = fmx(qy[e], qy[q]);
        ex0[e] = qx[e]; ey0[e] = qy[e];
        const float dy = qy[q] - qy[e];
        ek[e] = (dy > 1e-4f || dy < -1e-4f) ? (qx[q] - qx[e]) / dy : 0.0f;
        if (qy[e] < ymn) ymn = qy[e];
        if (qy[e] > ymx) ymx = qy[e];
    }
    int ya = flo(ymn), yb = flo(ymx);
    if (ya < 0) ya = 0;
    if (yb > CLK_SCR_H - 1) yb = CLK_SCR_H - 1;
    for (int y = ya; y <= yb; y++) {
        const float b0 = (float)y, b1 = (float)y + 1.0f;
        float mn = 1e9f, mx = -1e9f;
        for (int e = 0; e < 4; e++) {
            const float lo = fmx(elo[e], b0), hi = fmn(ehi[e], b1);
            if (lo > hi) continue;
            const float xa = ex0[e] + (lo - ey0[e]) * ek[e];
            const float xb = ex0[e] + (hi - ey0[e]) * ek[e];
            mn = fmn(mn, fmn(xa, xb));
            mx = fmx(mx, fmx(xa, xb));
        }
        if (mn > mx) continue;
        int ix0 = flo(mn) - 1, ix1 = flo(mx) + 2;
        if (ix0 < 0) ix0 = 0;
        if (ix1 > CLK_SCR_W - 1) ix1 = CLK_SCR_W - 1;
        if (ix0 > ix1) continue;
        if (o->y0 > o->y1) {                          /* first row of ink */
            o->y0 = o->y1 = (int16_t)y;
            o->x0[y] = (int16_t)ix0; o->x1[y] = (int16_t)ix1; o->h0[y] = 1; o->h1[y] = 0;
            continue;
        }
        if (y < o->y0) { for (int r = y; r < o->y0; r++) { o->x0[r] = INT16_MAX; o->x1[r] = -1; o->h0[r] = 1; o->h1[r] = 0; } o->y0 = (int16_t)y; }
        if (y > o->y1) { for (int r = o->y1 + 1; r <= y; r++) { o->x0[r] = INT16_MAX; o->x1[r] = -1; o->h0[r] = 1; o->h1[r] = 0; } o->y1 = (int16_t)y; }
        if (o->x0[y] > o->x1[y]) { o->x0[y] = (int16_t)ix0; o->x1[y] = (int16_t)ix1; o->h0[y] = 1; o->h1[y] = 0; }
        else seg_add(&o->x0[y], &o->x1[y], &o->h0[y], &o->h1[y], ix0, ix1);
    }
}

bool gfx_spans_of(const gfx_paint_ctx_t *pc, const gfx_spans_t **out)
{
    if (!pc->rot || pc->drum || !bx_alloc()) return false;
    s_spc_clock++;
    for (int i = 0; i < SPC_N; i++) {
        spc_t *c = &s_spc[i];
        if (c->alp == pc->alp && c->ang == pc->ang_10 && c->posx == pc->posx &&
            c->posy == pc->posy) {
            c->stamp = s_spc_clock;
            c->gen = s_spc_gen;
            *out = &c->sp;
            return true;
        }
    }
    const int64_t t0 = esp_timer_get_time();
    const ink_t *k = ink_of(pc);
    if (!k) return false;
    /* least recently used among the entries this commit has not handed out: one it has
     * is still a job's spans (gfx_commit's bsp[]); with none free the job paints its box */
    spc_t *c = NULL;
    for (int i = 0; i < SPC_N; i++)
        if (s_spc[i].gen != s_spc_gen && (!c || s_spc[i].stamp < c->stamp)) c = &s_spc[i];
    if (!c) return false;
    c->alp = pc->alp; c->ang = pc->ang_10; c->posx = pc->posx; c->posy = pc->posy;
    c->smooth = pc->smooth; c->stamp = s_spc_clock; c->gen = s_spc_gen;
    c->sp.y0 = 1; c->sp.y1 = 0;

    int32_t sinma, cosma;
    gfx_rot_sin_cos(pc->ang_10, &sinma, &cosma);
    const float cs = (float)cosma / 1024.0f, sn = (float)sinma / 1024.0f;
    const float cx = (float)(pc->posx + pc->pvx), cy = (float)(pc->posy + pc->pvy);
    const float pad = 3.0f;           /* covers bilinear too: one entry serves mark and paint */
    const uint32_t ns = (pc->h + INK_STRIP - 1) / INK_STRIP;
    for (uint32_t st = 0; st < ns; st++) {
        if (k->umin[st] > k->umax[st]) continue;
        const uint32_t ve = (st + 1) * INK_STRIP < pc->h ? (st + 1) * INK_STRIP : pc->h;
        const float v0 = (float)(st * INK_STRIP) - pad - pc->pvy, v1 = (float)ve + pad - pc->pvy;
        /* §49.15: a strip with an inkless gap is two quads, so a ring's hollow is no ink */
        const bool gap = k->gmin[st] <= k->gmax[st];
        const int16_t r0[2] = { k->umin[st], gap ? (int16_t)(k->gmax[st] + 1) : 0 };
        const int16_t r1[2] = { gap ? (int16_t)(k->gmin[st] - 1) : k->umax[st], k->umax[st] };
        for (int run = 0; run < (gap ? 2 : 1); run++) {
            const float u0 = r0[run] - pad - pc->pvx, u1 = r1[run] + 1 + pad - pc->pvx;
            const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
            float qx[4], qy[4];
            for (int q = 0; q < 4; q++) {             /* the painter's inverse, inverted */
                qx[q] = cx + cs * us[q] + sn * vs[q];
                qy[q] = cy - sn * us[q] + cs * vs[q];
            }
            quad_rows(qx, qy, &c->sp);
        }
    }
    s_bx_span_us += (uint64_t)(esp_timer_get_time() - t0);
    *out = &c->sp;
    return true;
}

bool gfx_mark_spans(const gfx_paint_ctx_t *pc)
{
    const gfx_spans_t *sp;
    if (!s_bx_mode || !gfx_spans_of(pc, &sp)) return false;
    for (int y = sp->y0; y <= sp->y1; y++) {
        if (sp->x0[y] > sp->x1[y]) continue;
        if (sp->h0[y] <= sp->h1[y]) {
            gfx_mark_rows(y, y, sp->x0[y], sp->h0[y] - 1, false);
            gfx_mark_rows(y, y, sp->h1[y] + 1, sp->x1[y], false);
        } else gfx_mark_rows(y, y, sp->x0[y], sp->x1[y], false);
    }
    return true;
}

/* A rotated job's row, painted only where its ink can be. Mode 2 paints the rest
 * too and counts what blended there: the proof the spans lose nothing. */
static void paint_spanned(const gfx_paint_ctx_t *pc, int32_t y, int32_t xa, int32_t xb,
                          uint16_t *row, const gfx_spans_t *sp)
{
    if (y >= pc->by1 && y <= pc->by2) {
        const int32_t a = xa > pc->bx1 ? xa : pc->bx1, b = xb < pc->bx2 ? xb : pc->bx2;
        if (b >= a) s_bx_iter_box += (uint64_t)(b - a + 1);
    }
    /* the job's ink on this row: one piece, or two either side of its gap (§49.15) */
    int32_t ja[2], jb[2], jn = 0;
    if (y >= sp->y0 && y <= sp->y1 && sp->x0[y] <= sp->x1[y]) {
        if (sp->h0[y] <= sp->h1[y]) { ja[0] = sp->x0[y]; jb[0] = sp->h0[y] - 1; ja[1] = sp->h1[y] + 1; jb[1] = sp->x1[y]; jn = 2; }
        else { ja[0] = sp->x0[y]; jb[0] = sp->x1[y]; jn = 1; }
    }
    int32_t ca[2], cb[2], cn = 0;
    for (int i = 0; i < jn; i++) {
        const int32_t a = xa > ja[i] ? xa : ja[i], b = xb < jb[i] ? xb : jb[i];
        if (a <= b) { ca[cn] = a; cb[cn] = b; cn++; }
    }
    for (int i = 0; i < cn; i++) {
        s_bx_iter_span += (uint64_t)(cb[i] - ca[i] + 1);
        gfx_paint_row_ctx(pc, y, ca[i], cb[i], row);
    }
    if (s_bx_mode == 2 && pc->role < CLK_PAINT_ROLES) {
        /* the check arm paints the rest of [xa, xb] too, and counts any blend that lands there */
        const uint32_t before = s_hk_role_blend[pc->role];
        int32_t at = xa;
        for (int i = 0; i < cn; i++) { if (at < ca[i]) gfx_paint_row_ctx(pc, y, at, ca[i] - 1, row); at = cb[i] + 1; }
        if (at <= xb) gfx_paint_row_ctx(pc, y, at, xb, row);
        s_bx_escape += s_hk_role_blend[pc->role] - before;
    }
}
#endif /* CONFIG_ORB_DEBUG_HTTP */


#if CONFIG_ORB_DEBUG_HTTP
/* ---- pre-paint (orb_gfx.h, plan §49.8) ------------------------------------- */
#define PP_JOBS 64   /* ORB_LAYER_JOB_MAX: a commit's jobs (plan §53) */
#define PP_RECTS 8
#define PP_FAST_MAX 8                 /* separate live spans a row: the seconds hands and their twins */
#define PP_FAST_GAP 8                 /* px: closer spans merge into one live pass */
typedef struct {
    const uint8_t *alp;
    int32_t ang, posx, posy, bx1, by1, bx2, by2;
    uint32_t w, h;
    uint8_t opa, smooth, bmode, rot;
} ppkey_t;

static volatile int s_pp_mode;
static uint16_t *s_pf, *s_pb, *s_pfn, *s_pbn;       /* wire order, stride CLK_SCR_W */
static bool      s_pp_valid;
static uint32_t  s_pp_gen;                            /* bumps on every invalidation  */
/* PSRAM .bss with the row ceiling past 32 (plan §53): one key a job, read once a commit. */
EXT_RAM_BSS_ATTR static ppkey_t s_pp_key[PP_JOBS];
static int       s_pp_nj = -1;
EXT_RAM_BSS_ATTR static bool    s_pp_fast[PP_JOBS];
static int       s_pp_low;                            /* the lowest fast job            */
/* §49.15: each fast job's zone, the disc its ink sweeps (pivot + farthest corner). The planes
 * cut at the lowest fast job whose zone covers a pixel, not the lowest anywhere: Modern's
 * sub-dial seconds (order 2) under its hour and minute hands had dropped them out of PB over
 * the whole dial, so every frame repainted them live wherever the centre seconds crossed. */
EXT_RAM_BSS_ATTR static int16_t s_pz_cx[PP_JOBS], s_pz_cy[PP_JOBS], s_pz_r[PP_JOBS];
EXT_RAM_BSS_ATTR static bool    s_pz_full[PP_JOBS];   /* a fast job whose pivot moves  */
static const gfx_paint_ctx_t *s_pp_jc;                /* the last commit's job list    */
static int       s_pp_jn;
static const uint16_t *s_pp_canvas;
static uint16_t  s_pp_row[CLK_SCR_W];
/* radar-plan §7.7: a live layer's next swap, built ahead (owner, 2026-11-09: the arm hitched
 * ~120 ms at every rain step over a storm). While the present is paced, the live worker, on
 * the other core, builds a whole PB (the slow layers under the fast jobs' cut, the rain among
 * them) for the picture waiting in the layer's back copy: its changed rows composed from a
 * snapshot of the job list, the rest copied from the front PB. The swap exchanges the PB
 * pointers and makes each changed row's PF from its PB, the slow jobs over the cut painted
 * on (the time, the cursor, the glass: pp_build_row's own second half), so the glass still
 * changes in one commit. One plane, not a pair: 434 KB (owner, 2026-11-09: PSRAM under
 * radar would not hold two). A row the LVGL task wrote or marked stale after
 * the snapshot (s_ah_bad: the time's label, the track's cursor) is rebuilt at the swap; a
 * rebuild of every plane (s_pp_gen) voids it all. */
enum { AH_IDLE, AH_RUN, AH_DONE };
#define AH_PSRAM_FLOOR (1024u * 1024u)                /* general PSRAM a spare pair leaves free */
static volatile int  s_ah_state = AH_IDLE;
static volatile bool s_ah_cancel;
static uint16_t *s_ah_pb;                             /* the spare PB (lean mode's own) */
EXT_RAM_BSS_ATTR static volatile uint8_t s_ah_bad[CLK_SCR_H];
EXT_RAM_BSS_ATTR static int16_t s_ah_x0[CLK_SCR_H], s_ah_x1[CLK_SCR_H];
/* the snapshot the worker builds from */
EXT_RAM_BSS_ATTR static gfx_paint_ctx_t s_ah_jc[PP_JOBS];
EXT_RAM_BSS_ATTR static bool    s_ah_fast[PP_JOBS];
EXT_RAM_BSS_ATTR static int16_t s_ah_pzcx[PP_JOBS], s_ah_pzcy[PP_JOBS], s_ah_pzr[PP_JOBS];
static int       s_ah_jn, s_ah_low;
static const uint16_t *s_ah_canvas;
static const uint16_t *s_ah_src_pb;
static const uint8_t *s_ah_alp;
static uint32_t  s_ah_gen;
static uint32_t  s_ah_used, s_ah_mism;                /* the bench's account */
/* the frame being built ahead */
static gfx_prep_rect_t s_pp_nx[PP_RECTS];
static int       s_pp_nnx, s_pp_nx_frame = -1, s_pp_prep_y;
static bool      s_pp_prep_done;
static uint32_t  s_pp_prep_gen;
/* the frame that landed, to swap or rebuild at the next commit */
static gfx_prep_rect_t s_pp_land[PP_RECTS];
static int       s_pp_nland;
static bool      s_pp_land_ready, s_pp_land_pending;
static gfx_prep_stats_t s_pps;
/* Stale rows: where a slow job moved, PF/PB are wrong until rebuilt. Such a row segment is
 * composited the shipped way (canvas + every job) and rebuilt in the present's slack, so a
 * slow move never stalls a tick. s_ps0/1 keep what went stale since the build-ahead began:
 * those rows of a frame built ahead were built against the old pose, and stay stale after
 * the swap. One interval per row, as the marks keep it; empty when x0 > x1. */
static int16_t   s_st0[CLK_SCR_H], s_st1[CLK_SCR_H], s_ps0[CLK_SCR_H], s_ps1[CLK_SCR_H];
static int       s_st_rows;                           /* rows with a stale segment     */
/* Back-sourced rows: a frame built ahead lands by reading its rows straight from the back
 * planes; the copy into the front planes runs in the slack (gfx_prep_idle), before any
 * stale rebuild or the next build-ahead may touch either plane. */
static int16_t   s_bk0[CLK_SCR_H], s_bk1[CLK_SCR_H];
static int       s_bk_rows;
/* A swap (radar-plan §6.27, gfx_prep_swap): a job's new pixels under the same pose, its
 * changed rows stale, so the next commit composes them all at once from the new picture.
 * s_lg_alp is the job's new alpha: the commit takes its key's change as no move (else the
 * whole box would go stale, not the rows that changed). s_lg_wait: the last commit's job list still
 * reads the old pixels (which the app may now draw over), so the slack waits for a commit. */
static const uint8_t *s_lg_alp;
EXT_RAM_BSS_ATTR static int16_t s_sw0[CLK_SCR_H], s_sw1[CLK_SCR_H];   /* the swap's rows */
static bool      s_sw_rows;
static bool      s_lg_wait;
static bool      s_pp_lean;                           /* GFX_PREP_LEAN: no back planes */
/* Fast by motion (§6.27): a slow job that moved on PP_MOVE_RUN commits in a row is a
 * fast one from then on (an app's sweep, the tracker's arm), known by its art. The
 * seconds rows are fast by role. */
#define PP_MOVE_RUN 4
#define PP_MOVED_MAX 4
static const uint16_t *s_pp_moved[PP_MOVED_MAX];
static int       s_pp_nmoved;
EXT_RAM_BSS_ATTR static uint8_t s_pp_run[PP_JOBS];

static void pp_stale_clear_all(void)
{
    for (int y = 0; y < CLK_SCR_H; y++) {
        s_st0[y] = s_ps0[y] = s_bk0[y] = INT16_MAX;
        s_st1[y] = s_ps1[y] = s_bk1[y] = -1;
    }
    s_st_rows = s_bk_rows = 0;
    s_lg_alp = NULL;
    s_sw_rows = false;
}
/* Settle one back-sourced row into the front planes. */
static void pp_bk_settle(int32_t y)
{
    if (s_bk0[y] > s_bk1[y]) return;
    const size_t off = (size_t)y * CLK_SCR_W + s_bk0[y], n = (size_t)(s_bk1[y] - s_bk0[y] + 1) * 2;
    memcpy(s_pf + off, s_pfn + off, n);
    memcpy(s_pb + off, s_pbn + off, n);
    s_bk0[y] = INT16_MAX; s_bk1[y] = -1; s_bk_rows--;
}
/* A plane row segment [a, b] into dst: front, except where the row is back-sourced. */
static void pp_row_src(uint16_t *dst, const uint16_t *front, const uint16_t *back, int32_t y, int32_t a, int32_t b)
{
    const size_t row = (size_t)y * CLK_SCR_W;
    const int32_t k0 = s_bk0[y], k1 = s_bk1[y];
    if (k0 > k1 || k1 < a || k0 > b) { memcpy(dst + a, front + row + a, (size_t)(b - a + 1) * 2); return; }
    const int32_t m0 = k0 > a ? k0 : a, m1 = k1 < b ? k1 : b;
    if (a < m0) memcpy(dst + a, front + row + a, (size_t)(m0 - a) * 2);
    memcpy(dst + m0, back + row + m0, (size_t)(m1 - m0 + 1) * 2);
    if (m1 < b) memcpy(dst + m1 + 1, front + row + m1 + 1, (size_t)(b - m1) * 2);
}
static void pp_bk_settle(int32_t y);
static void pp_stale_add(int32_t y, int32_t a, int32_t b)
{
    if (y < 0 || y >= CLK_SCR_H) return;
    if (s_ah_state != AH_IDLE) s_ah_bad[y] = 1;                    /* §7.7: rebuilt at the swap */
    pp_bk_settle(y);
    if (a < 0) a = 0;
    if (b > CLK_SCR_W - 1) b = CLK_SCR_W - 1;
    if (a > b) return;
    if (s_st0[y] > s_st1[y]) s_st_rows++;
    if (a < s_st0[y]) s_st0[y] = (int16_t)a;
    if (b > s_st1[y]) s_st1[y] = (int16_t)b;
    if (a < s_ps0[y]) s_ps0[y] = (int16_t)a;
    if (b > s_ps1[y]) s_ps1[y] = (int16_t)b;
    /* and repaint it this tick: the mirror holds the old pose there */
    row_add(y, a, b);
    display_mark_rows_clocked(y, y);
}
/* A job's footprint at a pose: its spans when it has them, else its box. */
static void pp_stale_pose(const gfx_paint_ctx_t *pc)
{
    const gfx_spans_t *sp;
    if (pc->rot && gfx_spans_of(pc, &sp)) {
        for (int32_t y = sp->y0; y <= sp->y1; y++) if (sp->x0[y] <= sp->x1[y]) pp_stale_add(y, sp->x0[y], sp->x1[y]);
        return;
    }
    for (int32_t y = pc->by1; y <= pc->by2; y++) pp_stale_add(y, pc->bx1, pc->bx2);
}

void gfx_prep_mode(int mode)
{
    s_pp_lean = (mode & GFX_PREP_LEAN) != 0;
    mode &= ~GFX_PREP_LEAN;
    s_pp_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
    s_pp_nmoved = 0;
    memset(s_pp_run, 0, sizeof s_pp_run);
    s_pp_valid = false;
    s_pp_nj = -1;
    s_pp_nx_frame = -1;
    s_pp_land_pending = false;
    memset(&s_pps, 0, sizeof s_pps);
    memset(s_pz_r, 0, sizeof s_pz_r);
    memset(s_pz_full, 0, sizeof s_pz_full);
    pp_stale_clear_all();
    /* §61: off frees the planes. They are four full glasses (~1.7 MB of PSRAM), and they
     * outlived the dial: the clock's pause turns pre-paint off (orb_sched_pause), yet the
     * planes stayed with Settings on the glass, and the theme carousel opened from the
     * clock's own page found no room for more than five pictures (owner, 2026-11-08). The
     * dial's resume turns it back on and takes them again. */
    if (!s_pp_mode && (s_pf || s_pb || s_pfn || s_pbn)) {
        orb_glass_free(s_pf);  orb_glass_free(s_pb);
        orb_glass_free(s_pfn); orb_glass_free(s_pbn);
        s_pf = s_pb = s_pfn = s_pbn = NULL;
    }
    if (s_ah_state == AH_RUN) s_ah_cancel = true;      /* §7.7: the worker stops at its next row */
    else s_ah_state = AH_IDLE;
    if ((!s_pp_mode || !s_pp_lean) && s_ah_pb && s_ah_state == AH_IDLE) {   /* lean's alone */
        orb_glass_free(s_ah_pb);
        s_ah_pb = NULL;
    }
    if (s_pp_mode && s_pp_lean && s_pfn) {            /* §6.27: lean keeps the fronts alone */
        orb_glass_free(s_pfn); orb_glass_free(s_pbn);
        s_pfn = s_pbn = NULL;
    }
    if (s_pp_mode && (!s_pf || (!s_pp_lean && !s_pfn))) {
        const size_t n = (size_t)CLK_SCR_W * CLK_SCR_H * 2;
        if (!s_pf)  s_pf  = orb_glass_alloc(n);         /* §6.21: the glass region */
        if (!s_pb)  s_pb  = orb_glass_alloc(n);
        if (!s_pp_lean) {
            if (!s_pfn) s_pfn = orb_glass_alloc(n);
            if (!s_pbn) s_pbn = orb_glass_alloc(n);
        }
        if (!s_pf || !s_pb || (!s_pp_lean && (!s_pfn || !s_pbn))) {
            orb_glass_free(s_pf);  orb_glass_free(s_pb);
            orb_glass_free(s_pfn); orb_glass_free(s_pbn);
            s_pf = s_pb = s_pfn = s_pbn = NULL;
            s_pp_mode = 0;
            ESP_LOGE("gfx", "pre-paint: no PSRAM for the planes");
        }
    }
}
int gfx_prep_on(void) { return s_pp_mode ? s_pp_mode | (s_pp_lean ? GFX_PREP_LEAN : 0) : 0; }
bool gfx_prep_swap(const int16_t *x0, const int16_t *x1, const uint8_t *alp)
{
    if (!s_pp_mode || !s_pp_valid) return false;
    for (int32_t y = 0; y < CLK_SCR_H; y++) {
        const int32_t a = x0 && x1 ? x0[y] : 0, b = x0 && x1 ? x1[y] : CLK_SCR_W - 1;
        if (!s_sw_rows) { s_sw0[y] = INT16_MAX; s_sw1[y] = -1; }
        if (a > b) continue;
        if (a < s_sw0[y]) s_sw0[y] = (int16_t)a;
        if (b > s_sw1[y]) s_sw1[y] = (int16_t)b;
        row_add(y, a, b);                                 /* repainted by this commit */
        display_mark_rows_clocked(y, y);
    }
    s_sw_rows = true;
    s_lg_alp = alp;
    s_lg_wait = true;
    s_pps.swaps++;
    return true;
}
static int ah_segs(int32_t y, int32_t a, int32_t b, int32_t *sx, int *lo, int32_t *pt);
static void ah_build_row(int32_t y, int32_t a, int32_t b, uint16_t *row);

bool gfx_prep_ahead(int layer, const int16_t *x0, const int16_t *x1, const uint8_t *front_alp,
                    const uint16_t *back_col, const uint8_t *back_alp)
{
    if (!s_pp_mode || !s_pp_lean || !s_pp_valid || !s_pp_jc || !x0 || !x1 || !s_pb || !s_pf) return false;
    if (s_ah_state == AH_RUN) return true;
    if (s_ah_state == AH_DONE && s_ah_alp == back_alp && s_ah_gen == s_pp_gen) return true;
    if (!s_ah_pb) {
        const size_t n = (size_t)CLK_SCR_W * CLK_SCR_H * 2;
        /* The glass region first; else the general PSRAM heap, only while 1 MB of it stays
         * free after (Bronze: a pair taken regardless left PSRAM's low water at 204 KB). Freed
         * whenever pre-paint goes off (the carousel, another app). */
        s_ah_pb = orb_glass_alloc_only(n);
        if (!s_ah_pb && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= n + AH_PSRAM_FLOOR)
            s_ah_pb = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ah_pb) return false;                       /* the swap composes as before */
    }
    int live = -1;
    for (int i = 0; i < s_pp_jn && i < PP_JOBS; i++) {
        if (s_pp_jc[i].drum) return false;                /* not built off the LVGL task */
        if (s_pp_jc[i].alp == front_alp) live = i;
    }
    if (live < 0 || s_pp_fast[live] || s_pp_jn > PP_JOBS) return false;
    memcpy(s_ah_jc, s_pp_jc, (size_t)s_pp_jn * sizeof *s_ah_jc);
    s_ah_jc[live].col = back_col;
    s_ah_jc[live].alp = back_alp;
    memcpy(s_ah_fast, s_pp_fast, sizeof s_ah_fast);
    memcpy(s_ah_pzcx, s_pz_cx, sizeof s_ah_pzcx);
    memcpy(s_ah_pzcy, s_pz_cy, sizeof s_ah_pzcy);
    memcpy(s_ah_pzr, s_pz_r, sizeof s_ah_pzr);
    s_ah_jn = s_pp_jn;
    s_ah_low = s_pp_low;
    s_ah_canvas = s_pp_canvas;
    s_ah_src_pb = s_pb;
    memcpy(s_ah_x0, x0, sizeof s_ah_x0);
    memcpy(s_ah_x1, x1, sizeof s_ah_x1);
    memset((void *)s_ah_bad, 0, sizeof s_ah_bad);
    s_ah_alp = back_alp;
    s_ah_gen = s_pp_gen;
    s_ah_cancel = false;
    s_ah_state = AH_RUN;
    lw_job_t j = { .op = LW_AHEAD, .layer = (uint8_t)layer };
    if (orb_live_work_post(&j) != ESP_OK) { s_ah_state = AH_IDLE; return false; }
    return true;
}

/* The live worker's half: the spare planes, row by row. */
void gfx_prep_ahead_run(void)
{
    if (s_ah_state != AH_RUN) return;
    static uint16_t *row;
    if (!row) row = heap_caps_malloc(CLK_SCR_W * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!row) row = heap_caps_malloc(CLK_SCR_W * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) { s_ah_state = AH_IDLE; return; }
    const size_t rb = (size_t)CLK_SCR_W * 2;
    for (int32_t y = 0; y < CLK_SCR_H; y++) {
        if (s_ah_cancel) { s_ah_state = AH_IDLE; return; }
        const size_t off = (size_t)y * CLK_SCR_W;
        memcpy(s_ah_pb + off, s_ah_src_pb + off, rb);
        if (s_ah_x0[y] <= s_ah_x1[y]) ah_build_row(y, s_ah_x0[y], s_ah_x1[y], row);
    }
    s_ah_state = AH_DONE;
}

bool gfx_prep_ahead_ready(const uint8_t *back_alp)
{
    return s_ah_state == AH_DONE && s_ah_alp == back_alp && s_ah_gen == s_pp_gen;
}

void gfx_prep_ahead_drop(const uint8_t *back_alp)
{
    if (back_alp && back_alp != s_ah_alp) return;     /* built from the other buffer: it stands */
    if (s_ah_state == AH_RUN) s_ah_cancel = true;
    else s_ah_state = AH_IDLE;
}

void gfx_prep_stale(int32_t y0, int32_t y1, int32_t x0, int32_t x1)
{
    if (!s_pp_mode || !s_pp_valid) return;
    for (int32_t y = y0; y <= y1; y++) pp_stale_add(y, x0, x1);
}
void gfx_prep_stats(gfx_prep_stats_t *o) { *o = s_pps; }

static void pp_key_of(const gfx_paint_ctx_t *pc, ppkey_t *k)
{
    k->alp = pc->alp; k->ang = pc->ang_10; k->posx = pc->posx; k->posy = pc->posy;
    k->bx1 = pc->bx1; k->by1 = pc->by1; k->bx2 = pc->bx2; k->by2 = pc->by2;
    k->w = pc->w; k->h = pc->h; k->opa = pc->opa; k->smooth = pc->smooth; k->bmode = pc->bmode; k->rot = pc->rot;
}
static bool pp_key_eq(const ppkey_t *a, const ppkey_t *b)
{
    return a->alp == b->alp && a->ang == b->ang && a->posx == b->posx && a->posy == b->posy &&
           a->w == b->w && a->h == b->h && a->opa == b->opa && a->smooth == b->smooth && a->bmode == b->bmode;
}

/* The fast jobs: the seconds row(s) and the shadow twin that sits just under each, turned
 * the same way. Everything else is slow and lives in the planes. */
static void pp_classify(int nj, const gfx_paint_ctx_t *jc)
{
    s_pp_low = nj;
    for (int i = 0; i < nj; i++) {
        s_pp_fast[i] = jc[i].role == 2;
        for (int m = 0; m < s_pp_nmoved && !s_pp_fast[i]; m++) s_pp_fast[i] = jc[i].col == s_pp_moved[m];
    }
    for (int i = 0; i < nj; i++) {
        if (jc[i].role != 3 || !jc[i].rot) continue;
        for (int k = i + 1; k < nj && k <= i + 2; k++)
            if (jc[k].role == 2 && jc[k].ang_10 == jc[i].ang_10) { s_pp_fast[i] = true; break; }
    }
    for (int i = 0; i < nj; i++) if (s_pp_fast[i]) { s_pp_low = i; break; }
}

/* The fast jobs' zones for this job list; true when any differs from the planes' (they are
 * then rebuilt whole). A fast job whose pivot moves (a slide) takes the whole panel for good. */
static bool pp_zones(int nj, const gfx_paint_ctx_t *jc)
{
    bool diff = false;
    for (int i = 0; i < nj && i < PP_JOBS; i++) {
        int16_t cx = 0, cy = 0, r = 0;
        if (s_pp_fast[i]) {
            const gfx_paint_ctx_t *pc = &jc[i];
            cx = (int16_t)(pc->posx + pc->pvx);
            cy = (int16_t)(pc->posy + pc->pvy);
            if (pc->drum || (s_pz_r[i] && (s_pz_cx[i] != cx || s_pz_cy[i] != cy))) s_pz_full[i] = true;
            if (s_pz_full[i]) { cx = CLK_SCR_W / 2; cy = CLK_SCR_H / 2; r = CLK_SCR_W + CLK_SCR_H; }
            else {
                const int32_t dx = pc->pvx > (int32_t)pc->w - pc->pvx ? pc->pvx : (int32_t)pc->w - pc->pvx;
                const int32_t dy = pc->pvy > (int32_t)pc->h - pc->pvy ? pc->pvy : (int32_t)pc->h - pc->pvy;
                r = (int16_t)(sqrtf((float)(dx * dx + dy * dy)) + 3.0f);   /* + the bilinear tap and spans' pad */
            }
        }
        if (cx != s_pz_cx[i] || cy != s_pz_cy[i] || r != s_pz_r[i]) diff = true;
        s_pz_cx[i] = cx; s_pz_cy[i] = cy; s_pz_r[i] = r;
    }
    return diff;
}

/* [a, b] of row y cut where the planes' cut changes: segment k is [sx[k], sx[k + 1] - 1]
 * with every job under lo[k] in PB (lo = the job count where no zone reaches). */
#define PP_SEG_MAX (2 * PP_JOBS + 2)    /* every zone's two edges: the cut is a function of the pixel alone */
static int pp_segs(int32_t y, int32_t a, int32_t b, int32_t *sx, int *lo)
{
    /* off the paint tasks' stacks and out of internal RAM; serialised as s_pp_row is */
    EXT_RAM_BSS_ATTR static int32_t pt[PP_SEG_MAX], c0[PP_JOBS], c1[PP_JOBS];
    EXT_RAM_BSS_ATTR static int zi[PP_JOBS];
    int np = 0, nz = 0;
    pt[np++] = a; pt[np++] = b + 1;
    for (int i = s_pp_low; i < s_pp_jn && i < PP_JOBS; i++) {
        if (!s_pp_fast[i] || !s_pz_r[i]) continue;
        const int32_t dy = y - s_pz_cy[i], r = s_pz_r[i];
        if (dy < -r || dy > r) continue;
        const int32_t hw = (int32_t)sqrtf((float)(r * r - dy * dy)) + 1;
        c0[nz] = s_pz_cx[i] - hw; c1[nz] = s_pz_cx[i] + hw; zi[nz] = i; nz++;
        if (c0[nz - 1] > a && c0[nz - 1] <= b) pt[np++] = c0[nz - 1];
        if (c1[nz - 1] + 1 > a && c1[nz - 1] + 1 <= b) pt[np++] = c1[nz - 1] + 1;
    }
    for (int i = 1; i < np; i++) for (int k = i; k > 0 && pt[k - 1] > pt[k]; k--) { int32_t t = pt[k]; pt[k] = pt[k - 1]; pt[k - 1] = t; }
    int n = 0;
    for (int k = 0; k + 1 < np; k++) {
        if (pt[k] >= pt[k + 1]) continue;
        int l = s_pp_jn;
        for (int z = 0; z < nz; z++)
            if (pt[k] >= c0[z] && pt[k] <= c1[z] && zi[z] < l) l = zi[z];
        if (n && lo[n - 1] == l) continue;              /* same cut: one segment */
        sx[n] = pt[k]; lo[n] = l; n++;
    }
    sx[n] = b + 1;
    return n;
}

/* A job's paint over [a, b] of row y, clipped to its spans when it has them. */
static void pp_paint(const gfx_paint_ctx_t *pc, int32_t y, int32_t a, int32_t b, uint16_t *row)
{
    const gfx_spans_t *sp;
    if (pc->rot && gfx_spans_of(pc, &sp)) {
        if (y < sp->y0 || y > sp->y1) return;
        if (a < sp->x0[y]) a = sp->x0[y];
        if (b > sp->x1[y]) b = sp->x1[y];
        if (a > b) return;
    }
    gfx_paint_row_ctx(pc, y, a, b, row);
}

/* Build PB and PF over [a, b] of row y into (pb, pf), from the canvas with `ov` rects laid
 * over it (the frame to come), or the canvas alone. */
static void pp_build_row(int32_t y, int32_t a, int32_t b, uint16_t *pb, uint16_t *pf,
                         const gfx_prep_rect_t *ov, int nov)
{
    if (pb == s_pb && s_ah_state != AH_IDLE) s_ah_bad[y] = 1;   /* §7.7: the copy is older */
    const uint16_t *cv = s_pp_canvas + (size_t)y * CLK_SCR_W;
    for (int32_t x = a; x <= b; x++) { const uint16_t v = cv[x]; s_pp_row[x] = (uint16_t)((v >> 8) | (v << 8)); }
    for (int r = 0; r < nov; r++) {
        const gfx_prep_rect_t *o = &ov[r];
        if (y < o->y || y >= o->y + o->h) continue;
        int32_t xa = a > o->x ? a : o->x, xb = b < o->x + o->w - 1 ? b : o->x + o->w - 1;
        const uint16_t *src = o->src + (size_t)(y - o->y) * o->w - o->x;
        for (int32_t x = xa; x <= xb; x++) { const uint16_t v = src[x]; s_pp_row[x] = (uint16_t)((v >> 8) | (v << 8)); }
    }
    /* PB: the slow layers under each stretch's cut; PF: every slow layer. A fast job under
     * a cut never has ink there (its zone does not reach), so it is left out of both. */
    EXT_RAM_BSS_ATTR static int32_t sx[PP_SEG_MAX + 1];
    EXT_RAM_BSS_ATTR static int lo[PP_SEG_MAX];
    const int ns = pp_segs(y, a, b, sx, lo);
    for (int k = 0; k < ns; k++) {
        const int32_t sa = sx[k], sb = sx[k + 1] - 1;
        for (int i = 0; i < lo[k]; i++) if (!s_pp_fast[i]) pp_paint(&s_pp_jc[i], y, sa, sb, s_pp_row);
        memcpy(pb + (size_t)y * CLK_SCR_W + sa, &s_pp_row[sa], (size_t)(sb - sa + 1) * 2);
        for (int i = lo[k]; i < s_pp_jn; i++) if (!s_pp_fast[i]) pp_paint(&s_pp_jc[i], y, sa, sb, s_pp_row);
        memcpy(pf + (size_t)y * CLK_SCR_W + sa, &s_pp_row[sa], (size_t)(sb - sa + 1) * 2);
    }
}

/* §7.7: pp_segs and pp_build_row over the snapshot, for the live worker: its own row and
 * scratch, the zones and fast set as they were at the ask, and every job painted the long way
 * (gfx_paint_row_ctx; the span cache is the LVGL task's). */
static int ah_segs(int32_t y, int32_t a, int32_t b, int32_t *sx, int *lo, int32_t *pt)
{
    int32_t c0[PP_JOBS], c1[PP_JOBS];
    int zi[PP_JOBS];
    int np = 0, nz = 0;
    pt[np++] = a; pt[np++] = b + 1;
    for (int i = s_ah_low; i < s_ah_jn && i < PP_JOBS; i++) {
        if (!s_ah_fast[i] || !s_ah_pzr[i]) continue;
        const int32_t dy = y - s_ah_pzcy[i], r = s_ah_pzr[i];
        if (dy < -r || dy > r) continue;
        const int32_t hw = (int32_t)sqrtf((float)(r * r - dy * dy)) + 1;
        c0[nz] = s_ah_pzcx[i] - hw; c1[nz] = s_ah_pzcx[i] + hw; zi[nz] = i; nz++;
        if (c0[nz - 1] > a && c0[nz - 1] <= b) pt[np++] = c0[nz - 1];
        if (c1[nz - 1] + 1 > a && c1[nz - 1] + 1 <= b) pt[np++] = c1[nz - 1] + 1;
    }
    for (int i = 1; i < np; i++) for (int k = i; k > 0 && pt[k - 1] > pt[k]; k--) { int32_t t = pt[k]; pt[k] = pt[k - 1]; pt[k - 1] = t; }
    int n = 0;
    for (int k = 0; k + 1 < np; k++) {                  /* as pp_segs */
        if (pt[k] >= pt[k + 1]) continue;
        int l = s_ah_jn;
        for (int z = 0; z < nz; z++)
            if (pt[k] >= c0[z] && pt[k] <= c1[z] && zi[z] < l) l = zi[z];
        if (n && lo[n - 1] == l) continue;              /* same cut: one segment */
        sx[n] = pt[k]; lo[n] = l; n++;
    }
    sx[n] = b + 1;
    return n;
}

static void ah_build_row(int32_t y, int32_t a, int32_t b, uint16_t *row)
{
    const uint16_t *cv = s_ah_canvas + (size_t)y * CLK_SCR_W;
    for (int32_t x = a; x <= b; x++) { const uint16_t v = cv[x]; row[x] = (uint16_t)((v >> 8) | (v << 8)); }
    int32_t sx[PP_SEG_MAX + 1], pt[PP_SEG_MAX];
    int lo[PP_SEG_MAX];
    const int ns = ah_segs(y, a, b, sx, lo, pt);
    for (int k = 0; k < ns; k++) {
        const int32_t sa = sx[k], sb = sx[k + 1] - 1;
        for (int i = 0; i < lo[k]; i++) if (!s_ah_fast[i]) gfx_paint_row_ctx(&s_ah_jc[i], y, sa, sb, row);
        memcpy(s_ah_pb + (size_t)y * CLK_SCR_W + sa, &row[sa], (size_t)(sb - sa + 1) * 2);
    }
}

/* §7.7: PF over [a, b] of row y from the new PB: the PB row, then the slow jobs over each
 * segment's cut, as pp_build_row's second half paints them. LVGL task, at the swap. */
static void pp_pf_from_pb(int32_t y, int32_t a, int32_t b)
{
    const size_t row = (size_t)y * CLK_SCR_W;
    memcpy(&s_pp_row[a], s_pb + row + a, (size_t)(b - a + 1) * 2);
    EXT_RAM_BSS_ATTR static int32_t sx[PP_SEG_MAX + 1];
    EXT_RAM_BSS_ATTR static int lo[PP_SEG_MAX];
    const int ns = pp_segs(y, a, b, sx, lo);
    for (int k = 0; k < ns; k++) {
        const int32_t sa = sx[k], sb = sx[k + 1] - 1;
        for (int i = lo[k]; i < s_pp_jn; i++) if (!s_pp_fast[i]) pp_paint(&s_pp_jc[i], y, sa, sb, s_pp_row);
        memcpy(s_pf + row + sa, &s_pp_row[sa], (size_t)(sb - sa + 1) * 2);
    }
}

static void pp_rebuild_box(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CLK_SCR_W - 1) x1 = CLK_SCR_W - 1;
    if (y1 > CLK_SCR_H - 1) y1 = CLK_SCR_H - 1;
    for (int32_t y = y0; y <= y1; y++) pp_build_row(y, x0, x1, s_pb, s_pf, NULL, 0);
    if (y1 >= y0) s_pps.rows_sync += (uint64_t)(y1 - y0 + 1);
}

void gfx_prep_landed(int frame, const gfx_prep_rect_t *cur, int ncur,
                     int next, const gfx_prep_rect_t *nxt, int nnext)
{
    if (!s_pp_mode || !s_pfn) return;                 /* lean: nothing is built ahead */
    if (ncur > PP_RECTS) ncur = PP_RECTS;
    if (nnext > PP_RECTS) nnext = PP_RECTS;
    memcpy(s_pp_land, cur, (size_t)ncur * sizeof *cur);
    s_pp_nland = ncur;
    s_pp_land_ready = s_pp_nx_frame == frame && s_pp_prep_done && s_pp_prep_gen == s_pp_gen;
    s_pp_land_pending = true;
    memcpy(s_pp_nx, nxt, (size_t)nnext * sizeof *nxt);
    s_pp_nnx = nnext;
    s_pp_nx_frame = next;
    s_pp_prep_y = 0;
    s_pp_prep_done = false;
    s_pp_prep_gen = s_pp_gen;
    /* what went stale before this build-ahead began is the swap's to carry (pp_commit reads
     * it before the next landed() can run, since every landing is followed by a commit) */
}

/* Build ahead: the next frame's rect rows into the back planes, a row at a time, until
 * the deadline. Called from the present's own wait, so it spends the slack the paced
 * frame would otherwise sleep through. */
bool gfx_prep_idle(int64_t deadline_us)
{
    if (!s_pp_mode || !s_pp_valid || !s_pp_jc) return false;
    /* Back-sourced rows first: the stale rebuild and the next build-ahead write planes those
     * rows still read from. Then the stale rows: until rebuilt, every tick composites them
     * the long way. */
    for (int32_t y = 0; y < CLK_SCR_H && s_bk_rows; y++) {
        if (s_bk0[y] > s_bk1[y]) continue;
        if (esp_timer_get_time() >= deadline_us) return true;
        pp_bk_settle(y);
    }
    if (s_lg_wait) return false;                      /* §6.27: a commit first */
    for (int32_t y = 0; y < CLK_SCR_H && s_st_rows; y++) {
        if (s_st0[y] > s_st1[y]) continue;
        if (esp_timer_get_time() >= deadline_us) return true;
        pp_build_row(y, s_st0[y], s_st1[y], s_pb, s_pf, NULL, 0);
        s_st0[y] = INT16_MAX; s_st1[y] = -1; s_st_rows--;
        s_pps.rows_sync++;
    }
    if (s_pp_nx_frame < 0 || s_pp_prep_done) return false;
    if (s_pp_prep_gen != s_pp_gen) { s_pp_prep_y = 0; s_pp_prep_gen = s_pp_gen; }
    while (s_pp_prep_y < CLK_SCR_H) {
        if (esp_timer_get_time() >= deadline_us) return true;
        const int32_t y = s_pp_prep_y++;
        int32_t a = CLK_SCR_W, b = -1;
        for (int r = 0; r < s_pp_nnx; r++) {
            const gfx_prep_rect_t *o = &s_pp_nx[r];
            if (y < o->y || y >= o->y + o->h) continue;
            if (o->x < a) a = o->x;
            if (o->x + o->w - 1 > b) b = o->x + o->w - 1;
        }
        if (a > b) continue;
        pp_build_row(y, a, b, s_pbn, s_pfn, s_pp_nx, s_pp_nnx);
        s_pps.rows_ahead++;
    }
    s_pp_prep_done = true;
    return false;
}

/* The pre-paint commit: the shipped commit's row walk, with PF standing in for the canvas
 * and every layer outside the fast spans, and the flush gate unchanged. */
static void pp_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas, uint16_t *fb,
                      const gfx_spans_t *const *bsp)
{
    s_pp_jc = jc; s_pp_jn = nj; s_pp_canvas = canvas;
    /* §7.7: rows built ahead for this swap hold only if nothing went stale since they began */
    const bool ahead_ok = s_sw_rows && s_ah_state == AH_DONE && s_ah_gen == s_pp_gen &&
                          s_lg_alp == s_ah_alp;
    s_lg_wait = false;                                /* §6.27: the job list reads the new pixels */
    pp_classify(nj, jc);
    {
        static int64_t s_said;
        const int64_t now = esp_timer_get_time();
        if (s_pp_lean && now - s_said > 10000000) {        /* §6.27: the app plan's account */
            s_said = now;
            int nf = 0;
            for (int i = 0; i < nj; i++) nf += s_pp_fast[i];
            ESP_LOGI("gfx", "pre-paint%s: %d jobs, %d fast (%d by motion) | %llu swaps (%llu built ahead), the last a %u ms blip | stale %llu rows, %d behind | %llu slow moves | live %llu px, copied %llu px",
                     s_pp_lean ? " (lean)" : "", nj, nf, s_pp_nmoved, (unsigned long long)s_pps.swaps,
                     (unsigned long long)s_pps.swaps_ahead,
                     (unsigned)(gfx_swap_blip_us() / 1000), (unsigned long long)s_pps.rows_sync, s_st_rows,
                     (unsigned long long)s_pps.rebuilds,
                     (unsigned long long)s_pps.live_px, (unsigned long long)s_pps.copy_px);
        }
    }
    const bool zones = pp_zones(nj, jc);
    /* Slow jobs that moved, appeared or vanished: their old and new boxes are rebuilt now,
     * and anything built ahead against the old poses is void. */
    bool changed = !s_pp_valid || nj != s_pp_nj || zones;
    if (!changed) {
        for (int i = 0; i < nj; i++) {
            if (s_pp_fast[i]) continue;
            ppkey_t k; pp_key_of(&jc[i], &k);
            if (s_lg_alp && k.alp == s_lg_alp) {
                /* §6.27: a swapped job's new pixels, the pose unchanged: this commit builds its rows */
                ppkey_t was = s_pp_key[i];
                was.alp = k.alp;
                if (pp_key_eq(&k, &was)) { s_pp_run[i] = 0; continue; }
            }
            if (pp_key_eq(&k, &s_pp_key[i])) { s_pp_run[i] = 0; continue; }
            /* §6.27: the same art moving commit after commit is fast from now on (one whole
             * rebuild, next commit, as its zone joins the planes' cut) */
            if (k.alp == s_pp_key[i].alp && ++s_pp_run[i] >= PP_MOVE_RUN && s_pp_nmoved < PP_MOVED_MAX) {
                s_pp_moved[s_pp_nmoved++] = jc[i].col;
                s_pp_run[i] = 0;
                s_pps.promoted++;
                ESP_LOGI("gfx", "pre-paint: job %d (role %u) moves every frame: fast from now on", i, jc[i].role);
            }
            {
                static int64_t s_pp_said;
                if (esp_timer_get_time() - s_pp_said > 2000000) {
                    s_pp_said = esp_timer_get_time();
                    ESP_LOGI("gfx", "pre-paint: job %d role %u moved: ang %ld->%ld pos %ld,%ld->%ld,%ld opa %u->%u alp %s",
                             i, jc[i].role, (long)s_pp_key[i].ang, (long)k.ang, (long)s_pp_key[i].posx,
                             (long)s_pp_key[i].posy, (long)k.posx, (long)k.posy, s_pp_key[i].opa, k.opa,
                             s_pp_key[i].alp == k.alp ? "same" : "new");
                }
                /* The old pose and the new go stale, and this tick repaints them the long
                 * way; the rebuild runs in the slack (gfx_prep_idle). */
                gfx_paint_ctx_t was = jc[i];
                was.ang_10 = s_pp_key[i].ang; was.posx = s_pp_key[i].posx; was.posy = s_pp_key[i].posy;
                was.bx1 = s_pp_key[i].bx1; was.by1 = s_pp_key[i].by1; was.bx2 = s_pp_key[i].bx2; was.by2 = s_pp_key[i].by2;
                was.rot = s_pp_key[i].rot;
                if (s_pp_key[i].alp == k.alp) pp_stale_pose(&was);
                else for (int32_t yy = was.by1; yy <= was.by2; yy++) pp_stale_add(yy, was.bx1, was.bx2);
                pp_stale_pose(&jc[i]);
                s_pps.rebuilds++;
            }
        }
    } else {
        pp_stale_clear_all();
        memset(s_pp_run, 0, sizeof s_pp_run);
        pp_rebuild_box(0, 0, CLK_SCR_W - 1, CLK_SCR_H - 1);
        s_pp_valid = true;
        s_pp_gen++; s_pps.rebuilds++;
        s_pp_land_pending = false;
    }
    s_pp_nj = nj;
    for (int i = 0; i < nj; i++) pp_key_of(&jc[i], &s_pp_key[i]);
    s_lg_alp = NULL;                                  /* the keys hold the new alpha now */
    /* §6.27: the swap's rows built into the planes here, from the new picture: the blip is
     * the whole of it, and no row is left stale for the frames after to compose the long way
     * (a storm's ~300 stale rows held the sweep at 13 fps between steps, Bronze 2026-11-08) */
    if (s_sw_rows) {
        /* §7.7: built ahead, the swap is the spare planes; rows written or gone stale since
         * the snapshot are built again from this commit's jobs. The first three after a boot
         * (and every one in the exactness mode) are also built the long way into the old
         * planes and compared: the log's proof that the two ways agree (Bronze: 0 px). */
        const bool use = ahead_ok && s_ah_gen == s_pp_gen;
        if (use) {
            uint16_t *t = s_pb; s_pb = s_ah_pb; s_ah_pb = t;
            s_ah_state = AH_IDLE;
            for (int32_t y = 0; y < CLK_SCR_H; y++) {
                if (s_ah_bad[y]) { pp_build_row(y, 0, CLK_SCR_W - 1, s_pb, s_pf, NULL, 0); s_pps.rows_sync++; }
                else if (s_sw0[y] <= s_sw1[y]) pp_pf_from_pb(y, s_sw0[y], s_sw1[y]);
            }
            if (++s_ah_used <= 3 || s_pp_mode == 2) {
                /* the long way into the old PB (PB) and a row of scratch (PF), compared */
                static uint16_t *tf;
                if (!tf) tf = heap_caps_malloc(CLK_SCR_W * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                uint32_t bad = 0;
                for (int32_t y = 0; tf && y < CLK_SCR_H; y++) {
                    if (s_sw0[y] > s_sw1[y]) continue;
                    pp_build_row(y, s_sw0[y], s_sw1[y], s_ah_pb, tf - (size_t)y * CLK_SCR_W, NULL, 0);
                    const size_t o = (size_t)y * CLK_SCR_W;
                    for (int32_t x = s_sw0[y]; x <= s_sw1[y]; x++)
                        bad += (s_ah_pb[o + x] != s_pb[o + x]) + (tf[x] != s_pf[o + x]);
                }
                s_ah_mism += bad;
                ESP_LOGI("gfx", "ahead swap %u checked: %u px differ from the long way (%u in all)",
                         (unsigned)s_ah_used, (unsigned)bad, (unsigned)s_ah_mism);
            }
            s_pps.swaps_ahead++;
        } else {
            if (s_ah_state == AH_RUN) s_ah_cancel = true;
            else s_ah_state = AH_IDLE;
            for (int32_t y = 0; y < CLK_SCR_H; y++)
                if (s_sw0[y] <= s_sw1[y]) { pp_build_row(y, s_sw0[y], s_sw1[y], s_pb, s_pf, NULL, 0); s_pps.rows_sync++; }
        }
        s_sw_rows = false;
    }
    /* The frame that landed on the canvas this tick: swap in its rows if they were built
     * ahead against these same slow poses, or build them now. */
    if (s_pp_land_pending) {
        /* the slack never caught up with the last swap: settle it before this one */
        for (int32_t y = 0; y < CLK_SCR_H && s_bk_rows; y++) pp_bk_settle(y);
        const bool ready = s_pp_land_ready && s_pp_prep_gen == s_pp_gen;
        for (int r = 0; r < s_pp_nland; r++) {
            const gfx_prep_rect_t *o = &s_pp_land[r];
            for (int y = o->y; y < o->y + o->h && y < CLK_SCR_H; y++) {
                const size_t off = (size_t)y * CLK_SCR_W + o->x;
                if (ready) {
                    /* read from the back planes until the slack copies them over */
                    (void)off;
                    if (s_bk0[y] > s_bk1[y]) s_bk_rows++;
                    if (o->x < s_bk0[y]) s_bk0[y] = o->x;
                    if (o->x + o->w - 1 > s_bk1[y]) s_bk1[y] = (int16_t)(o->x + o->w - 1);
                } else {
                    pp_build_row(y, o->x, o->x + o->w - 1, s_pb, s_pf, NULL, 0);
                    s_pps.rows_sync++;
                }
            }
        }
        if (ready) {
            /* rows built ahead before a slow move are the old pose's: they stay stale */
            for (int r = 0; r < s_pp_nland; r++) {
                const gfx_prep_rect_t *o = &s_pp_land[r];
                for (int y = o->y; y < o->y + o->h && y < CLK_SCR_H; y++) {
                    int32_t a = s_ps0[y] > o->x ? s_ps0[y] : o->x, b = s_ps1[y] < o->x + o->w - 1 ? s_ps1[y] : o->x + o->w - 1;
                    if (a <= b) pp_stale_add(y, a, b);
                }
            }
        }
        for (int y = 0; y < CLK_SCR_H; y++) { s_ps0[y] = INT16_MAX; s_ps1[y] = -1; }
        if (ready) s_pps.swaps_ready++; else s_pps.swaps_late++;
        s_pp_land_pending = false;
    }

    for (int32_t y = 0; y < CLK_SCR_H; y++) {
        if (!rowstate.need[y]) continue;
        const int32_t xa = rowstate.x0[y], xb = rowstate.x1[y];
        uint16_t *band = s_band[0];
        if (s_st0[y] <= s_st1[y]) {
            /* §6.27: a stale row this commit repaints is rebuilt into the planes now, at the
             * long way's own cost, rather than composed the long way every frame until the
             * slack rebuilds it: a frame over budget leaves no slack, and the whole disc
             * stale held a storm's sweep at 13 fps with all 466 rows behind (Bronze). */
            pp_build_row(y, s_st0[y], s_st1[y], s_pb, s_pf, NULL, 0);
            s_st0[y] = INT16_MAX; s_st1[y] = -1; s_st_rows--;
            s_pps.rows_sync++;
        }
        pp_row_src(band, s_pf, s_pfn, y, xa, xb);
        s_pps.copy_px += (uint64_t)(xb - xa + 1);
        /* The fast spans on this row, inside the restored extent: live from PB upward, each
         * on its own. One union across two seconds hands (Modern's centre and sub-dial pair,
         * §49.15) repainted every slow layer between them live, every frame; spans that touch
         * merge, so a hand and its shadow twin stay one pass. */
        int32_t fa[PP_FAST_MAX], fb_[PP_FAST_MAX];
        int nf = 0;
        for (int i = s_pp_low; i < nj; i++) {
            if (!s_pp_fast[i]) continue;
            const gfx_spans_t *sp = (i < PP_JOBS) ? bsp[i] : NULL;
            int32_t a, b;
            if (sp) { if (y < sp->y0 || y > sp->y1) continue; a = sp->x0[y]; b = sp->x1[y]; }
            else { if (y < jc[i].by1 || y > jc[i].by2) continue; a = jc[i].bx1; b = jc[i].bx2; }
            if (a < xa) a = xa;
            if (b > xb) b = xb;
            if (a > b) continue;
            int k = 0;
            while (k < nf && (b + PP_FAST_GAP < fa[k] || a > fb_[k] + PP_FAST_GAP)) k++;
            if (k < nf) {                                   /* touches span k: grow it */
                if (a < fa[k]) fa[k] = a;
                if (b > fb_[k]) fb_[k] = b;
            } else if (nf < PP_FAST_MAX) { fa[nf] = a; fb_[nf] = b; nf++; }
            else {                                          /* out of slots: widen the last */
                if (a < fa[nf - 1]) fa[nf - 1] = a;
                if (b > fb_[nf - 1]) fb_[nf - 1] = b;
            }
        }
        /* a grown span may now touch another: fold until none do */
        for (int p = 0; p < nf; p++)
            for (int q = p + 1; q < nf; q++)
                if (!(fb_[q] + PP_FAST_GAP < fa[p] || fa[q] > fb_[p] + PP_FAST_GAP)) {
                    if (fa[q] < fa[p]) fa[p] = fa[q];
                    if (fb_[q] > fb_[p]) fb_[p] = fb_[q];
                    fa[q] = fa[nf - 1]; fb_[q] = fb_[nf - 1]; nf--; q = p;
                }
        for (int s = 0; s < nf; s++) {
            const int32_t f0 = fa[s], f1 = fb_[s];
            pp_row_src(band, s_pb, s_pbn, y, f0, f1);
            EXT_RAM_BSS_ATTR static int32_t sx[PP_SEG_MAX + 1];
            EXT_RAM_BSS_ATTR static int lo[PP_SEG_MAX];
            const int ns = pp_segs(y, f0, f1, sx, lo);
            for (int k = 0; k < ns; k++)
                for (int i = lo[k]; i < nj; i++) pp_paint(&jc[i], y, sx[k], sx[k + 1] - 1, band);
            s_pps.live_px += (uint64_t)(f1 - f0 + 1);
        }
        if (s_st0[y] <= s_st1[y]) {
            /* a stale segment: canvas and every job, as the shipped commit composes it */
            int32_t a = s_st0[y] > xa ? s_st0[y] : xa, b = s_st1[y] < xb ? s_st1[y] : xb;
            if (a <= b) {
                const uint16_t *src = &canvas[(size_t)y * CLK_SCR_W];
                for (int32_t x = a; x <= b; x++) { const uint16_t v = src[x]; band[x] = (uint16_t)((v >> 8) | (v << 8)); }
                for (int i = 0; i < nj; i++) pp_paint(&jc[i], y, a, b, band);
                s_pps.live_px += (uint64_t)(b - a + 1);
            }
        }
        if (s_pp_mode == 2) {
            /* The shipped composite of the same row, for the exactness count. */
            const uint16_t *src = &canvas[(size_t)y * CLK_SCR_W];
            for (int32_t x = xa; x <= xb; x++) { const uint16_t v = src[x]; s_pp_row[x] = (uint16_t)((v >> 8) | (v << 8)); }
            for (int i = 0; i < nj; i++) pp_paint(&jc[i], y, xa, xb, s_pp_row);
            for (int32_t x = xa; x <= xb; x++) if (s_pp_row[x] != band[x]) s_pps.mismatch_px++;
            s_pps.checked_px += (uint64_t)(xb - xa + 1);
        }
        uint16_t *fbrow = fb + (size_t)y * CLK_SCR_W;
        bool send = rowstate.force[y] != 0;
        for (int32_t x = xa; x <= xb; x++) {
            const uint16_t v = band[x];
            if (fbrow[x] != v) { fbrow[x] = v; send = true; }
        }
        display_commit_row(y, send);
    }
    memset(rowstate.need, 0, sizeof(rowstate.need));
    memset(rowstate.force, 0, sizeof(rowstate.force));
}
#endif /* CONFIG_ORB_DEBUG_HTTP */

/* Rebuild the rows the app dirtied, from the ground up: background out of the
 * caller's canvas, then every layer it listed, in the order it listed them.
 *
 * Row-major on purpose. This used to be layer-major — restore one pass over the
 * marked rows, then one pass per live layer, then the gate's snapshot and compare
 * — which read-modified-wrote the PSRAM mirror ~6 times per pixel and measured (on
 * the fps line's phase breakdown) at ~90 ms of a ~145 ms commit for 6-7 fps, of
 * which DMA was only ~15 ms. Now the mirror is read once per row and written only
 * where the composite came out different, and the ~190k blend_px calls per commit
 * happen inside internal RAM.
 *
 * The marks are consumed HERE, not at the next tick. A commit only happens when
 * LVGL gets around to starting one, and at 47 ms per flush against a 40 ms GIF
 * beat the commit trails the tick that dirtied the rows: clearing at tick start
 * handed rows to a commit that never ran and dropped them, which is what made
 * hands vanish over freshly pushed background and leave a ghost at the pose the
 * first LVGL render had drawn. Rows that never get a commit stay marked until one
 * does. */
/* §6.27: a live layer's swap is one blip, its changed rows composed and sent in one commit.
 * Its cost, the commit's own time and its pixels' push (~0.35 us a pixel on this panel,
 * Bronze 2026-11-08), paces the next swap (stage_live_present). */
static volatile bool s_blip_ask;
static volatile uint32_t s_blip_us;
void gfx_swap_note(void) { s_blip_ask = true; }
uint32_t gfx_swap_blip_us(void) { return s_blip_us; }
static void gfx_commit_rows(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas);
void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas)
{
    if (!s_blip_ask) { gfx_commit_rows(nj, jc, canvas); return; }
    s_blip_ask = false;
    uint64_t px = 0;
    for (int32_t y = 0; y < CLK_SCR_H; y++)
        if (rowstate.need[y] && rowstate.x1[y] >= rowstate.x0[y]) px += (uint64_t)(rowstate.x1[y] - rowstate.x0[y] + 1);
    const int64_t t0 = esp_timer_get_time();
    gfx_commit_rows(nj, jc, canvas);
    s_blip_us = (uint32_t)(esp_timer_get_time() - t0) + (uint32_t)(px * 35 / 100);
}
static void gfx_commit_rows(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas)
{
    uint16_t *fb = display_fb_mirror();
    if (!fb) return;
#if CONFIG_ORB_DEBUG_HTTP
    /* The span bench: each rotated job's spans, resolved once per commit. */
    const gfx_spans_t *bsp[PP_JOBS] = { 0 };
    const int bmode = s_bx_mode;
    const int64_t bx_t0 = esp_timer_get_time();
    static gfx_paint_ctx_t jl[32];          /* the nearest-shadow and AA arms' own job copy */
    if ((s_bx_nearest || s_bx_aa_sec) && nj <= 32) {
        memcpy(jl, jc, (size_t)nj * sizeof *jc);
        for (int i = 0; i < nj; i++) {
            if (s_bx_nearest && jl[i].role == 3) jl[i].smooth = false;
            if (s_bx_aa_sec && jl[i].role == 2 && jl[i].rot) jl[i].smooth = true;
            if (s_bx_aa_sec >= 2 && jl[i].role == 3 && jl[i].rot) jl[i].smooth = true;
        }
        jc = jl;
    }
    s_spc_gen++;                            /* a new commit: the pose cache's pins start over */
    if (bmode) for (int i = 0; i < nj && i < PP_JOBS; i++) if (!gfx_spans_of(&jc[i], &bsp[i])) bsp[i] = NULL;
    s_bx_commits++;
    if (s_pp_mode && bmode && !s_ko_on && nj <= PP_JOBS) {
        pp_commit(nj, jc, canvas, fb, bsp);
        const uint32_t us = (uint32_t)(esp_timer_get_time() - bx_t0);
        const int b = us < 10000 ? 0 : us < 20000 ? 1 : us < 40000 ? 2 : us < 60000 ? 3 : us < 90000 ? 4 : 5;
        s_bx_hist[b]++;
        if (us > s_bx_max_us) s_bx_max_us = us;
        s_hk_paint_us += us;
        return;
    }
#endif
        /* Walk the marked rows in bands (see s_band). Runs of marked rows are the
         * normal case — a GIF rect or a hand's swept box marks a contiguous range —
         * so the band costs nothing over the one-row walk and it is what lets the
         * rotated pass run in column tiles instead of a full-width diagonal. */
        for (int32_t y0 = 0; y0 < CLK_SCR_H; ) {
            if (!rowstate.need[y0]) { y0++; continue; }
            int nr = 1;
            while (nr < CLK_COMMIT_BAND && y0 + nr < CLK_SCR_H && rowstate.need[y0 + nr]) nr++;

            int32_t bx0 = rowstate.x0[y0], bx1 = rowstate.x1[y0];
            for (int r = 1; r < nr; r++) {
                if (rowstate.x0[y0 + r] < bx0) bx0 = rowstate.x0[y0 + r];
                if (rowstate.x1[y0 + r] > bx1) bx1 = rowstate.x1[y0 + r];
            }

            /* Restore each row of the band from the canvas the plate itself reads
             * from, byte-swapped into mirror order. The restore is what makes the
             * pass idempotent: a layer that moved away has its pixels wiped back to
             * plain background, and a layer painted twice over the same row lands on
             * clean pixels each time. Doing it here (rather than leaning on LVGL to
             * re-render the plate) is what removes the re-composite cost. One PSRAM
             * read pass; the mirror side of the row is still untouched at this
             * point, which is what lets the gate below compare against it in place
             * instead of keeping a snapshot copy. */
            int64_t t0 = esp_timer_get_time();
            for (int r = 0; r < nr; r++) {
                const int32_t y = y0 + r;
                int32_t sa[2], sb[2];
                const int ns = row_segs(y, sa, sb);      /* §49.15: skip the row's gap */
                for (int g = 0; g < ns; g++) {
                    const int32_t xa = sa[g], xb = sb[g];
#if CONFIG_ORB_DEBUG_HTTP
                    s_bx_restored += (uint64_t)(xb - xa + 1);
#endif
                    const uint16_t *src = &canvas[(size_t)y * CLK_SCR_W + xa];
                    uint16_t *d = s_band[r] + xa;
                    for (int32_t x = xa; x <= xb; x++) {
                        const uint16_t v = *src++;
                        *d++ = (uint16_t)((v >> 8) | (v << 8));
                    }
                }
            }
            s_hk_rest_us += (uint64_t)(esp_timer_get_time() - t0);

            t0 = esp_timer_get_time();
            for (int i = 0; i < nj; i++) {
                /* One timer pair per (layer, band), not per (layer, row): the
                 * per-layer split is what found the carriage, and at ~1500 calls
                 * per commit esp_timer_get_time() itself was starting to cost the
                 * ~0.2 ms being measured. */
#if CONFIG_ORB_DEBUG_HTTP
                if (s_bx_noglass && jc[i].role == CLK_ROLE_GLASS) continue;
#endif
                const int64_t tr = esp_timer_get_time();
                if (!jc[i].rot) {
                    /* Static (identity) layer: texels are read straight across the
                     * row, so one call per row is already the cheapest order. */
                    for (int r = 0; r < nr; r++) {
                        const int32_t y = y0 + r;
                        int32_t sa[2], sb[2];
                        const int ns = row_segs(y, sa, sb);
                        for (int g = 0; g < ns; g++) gfx_paint_row_ctx(&jc[i], y, sa[g], sb[g], s_band[r]);
                    }
                } else {
                    /* Rotated layer: column tiles, rows innermost. The rows of a
                     * band sample parallel diagonals, so one tile's texel strip
                     * (≈ tile + band rows wide) is reused by every row in the band
                     * while it is still in the PSRAM cache — the whole reason the
                     * carriage's cost stops tracking its angle. */
                    for (int32_t tx = bx0; tx <= bx1; tx += CLK_PAINT_TILE) {
                        const int32_t te = tx + CLK_PAINT_TILE - 1;
                        for (int r = 0; r < nr; r++) {
                            const int32_t y = y0 + r;
                            int32_t sa[2], sb[2];
                            const int ns = row_segs(y, sa, sb);
                            for (int g = 0; g < ns; g++) {
                                int32_t xa = sa[g], xb = sb[g];
                                if (xb < tx || xa > te) continue;
                                if (xa < tx) xa = tx;
                                if (xb > te) xb = te;
#if CONFIG_ORB_DEBUG_HTTP
                                if (i < PP_JOBS && bsp[i]) {
                                    paint_spanned(&jc[i], y, xa, xb, s_band[r], bsp[i]);
                                    continue;
                                }
#endif
                                gfx_paint_row_ctx(&jc[i], y, xa, xb, s_band[r]);
                            }
                        }
                    }
                }
                const uint8_t role = jc[i].role;
                if (role < CLK_PAINT_ROLES) s_hk_role_us[role] += (uint64_t)(esp_timer_get_time() - tr);
            }
    #if DISPLAY_BENCH
            /* Inside the same timed paint window: the CAD hook/px fields must
             * carry the ink's cost, not hide it in an untimed phase. */
            if (s_band_hook) s_band_hook(y0, nr);
    #endif
            s_hk_paint_us += (uint64_t)(esp_timer_get_time() - t0);

            /* Flush gate: write back and send only the rows whose re-composite
             * came out different (the mirror is the panel's truth, so identical
             * needs no DMA at all). Force rows (GIF pushes, whose pixels reached
             * the mirror but never the panel) and rows LVGL rendered itself this
             * frame (chunk path) ride regardless of the compare. */
            t0 = esp_timer_get_time();
            for (int r = 0; r < nr; r++) {
                const int32_t y = y0 + r;
                uint16_t *fbrow = fb + (size_t)y * CLK_SCR_W;
                bool send = rowstate.force[y] != 0;
                int32_t sa[2], sb[2];
                const int ns = row_segs(y, sa, sb);
                for (int g = 0; g < ns; g++) {
                const int32_t xa = sa[g], xb = sb[g];
                /* The keep-out cut, in the one place it has to live: the staged
                 * row still holds the dial's pixels for a rect's span, but
                 * they never reach the mirror while the chrome stands, so the
                 * chrome's own bytes stay intact and the row that DMAes carries
                 * them. Only the rect's pixels are skipped: the cut before this
                 * one took the row's whole segment out whenever it touched the
                 * rect (its side runs had min and max swapped), harmless while
                 * the one chrome parked the beat, not under a live map. */
                if (!ko_row(y)) {
                    for (int32_t x = xa; x <= xb; x++) {
                        const uint16_t v = s_band[r][x];
                        if (fbrow[x] != v) { fbrow[x] = v; send = true; }
                    }
                } else {
                    for (int32_t x = xa; x <= xb; x++) {
                        if (ko_hit(x, y)) continue;
                        const uint16_t v = s_band[r][x];
                        if (fbrow[x] != v) { fbrow[x] = v; send = true; }
                    }
                }
                }
                display_commit_row(y, send);
            }
            s_hk_cmp_us += (uint64_t)(esp_timer_get_time() - t0);

            y0 += nr;
        }

        memset(rowstate.need, 0, sizeof(rowstate.need));
        memset(rowstate.force, 0, sizeof(rowstate.force));
#if CONFIG_ORB_DEBUG_HTTP
        {
            const uint32_t us = (uint32_t)(esp_timer_get_time() - bx_t0);
            const int b = us < 10000 ? 0 : us < 20000 ? 1 : us < 40000 ? 2 : us < 60000 ? 3 : us < 90000 ? 4 : 5;
            s_bx_hist[b]++;
            if (us > s_bx_max_us) s_bx_max_us = us;
        }
#endif
}
