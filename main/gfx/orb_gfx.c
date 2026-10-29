/* main/gfx/orb_gfx.c — the OS-owned compositor: the rotation and blend maths
 * that replaces lv_draw_sw_transform() on the L4 path, the swept-box geometry,
 * the per-row mark set, and the commit band that restores, paints and DMA-gates.
 *
 * Extracted from main/clock/clock_view.c in P1 with the maths unchanged: the
 * renames and the row-state consolidation are the only diff, so the shipped dial
 * composites the same bytes it did before the move.
 *
 * The hook lives in this TU for a reason: gfx_paint_row_ctx is called per layer
 * per row (~1500 times a commit on the tourbillon face), and a cross-TU call
 * there is measurable. The app builds the job list; gfx_commit paints it.
 */

#include "orb_gfx.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"

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
        if (!rowstate.need[y]) {
            rowstate.need[y] = 1;
            rowstate.x0[y] = x0;
            rowstate.x1[y] = x1;
        } else {
            if (x0 < rowstate.x0[y]) rowstate.x0[y] = x0;
            if (x1 > rowstate.x1[y]) rowstate.x1[y] = x1;
        }
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

/* lv_color_16_16_mix_inlined(), copied from lv_draw_sw_blend_private.h. */
static inline uint16_t mix_16_16(uint16_t c1, uint16_t c2, uint8_t mix)
{
    if (mix == 255) return c1;
    if (mix == 0)   return c2;
    if (c1 == c2)   return c1;
    uint32_t mix5 = ((uint32_t)mix + 4) >> 3;
    uint32_t bg = (uint32_t)(c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    uint32_t fg = (uint32_t)(c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    uint32_t res = ((((fg - bg) * mix5) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((res >> 16) | res);
}

/* Blend one sampled texel onto the mirror. Shared by the rotated and the
 * static loop in paint_row so the two can never drift apart. */
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
        for (int32_t x = xa; x <= xb; x++, si++) {
            const uint8_t a = alp[si];
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

    for (int32_t x = xa; x <= xb; x++) {
        const int32_t x_abs = x - pc->posx;
        const int32_t xs_int = (xs_r + ((pc->sxx * x_abs) >> 8)) >> 8;
        const int32_t ys_int = (ys_r + ((pc->syx * x_abs) >> 8)) >> 8;
        if (xs_int < 0 || xs_int >= (int32_t)pc->w ||
            ys_int < 0 || ys_int >= (int32_t)pc->h) continue;

        const uint32_t sidx = (uint32_t)ys_int * pc->w + (uint32_t)xs_int;
        const uint8_t a = alp[sidx];
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
static int32_t s_ko[4];                      /* x0, y0, x1, y1 */
static bool    s_ko_on;

void gfx_keepout_set(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    s_ko[0] = x0; s_ko[1] = y0; s_ko[2] = x1; s_ko[3] = y1;
    s_ko_on = true;
}

void gfx_keepout_clear(void)
{
    if (!s_ko_on) return;
    s_ko_on = false;
    /* Whatever the commit skipped while the chrome stood has to come back: a
     * hand that moved across the rect during the peek still owns those pixels,
     * so re-mark the rect whole and the next commit repaints it. */
    gfx_mark_rows(s_ko[1], s_ko[3], s_ko[0], s_ko[2], false);
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
void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas)
{
    uint16_t *fb = display_fb_mirror();
    if (!fb) return;
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
                const int32_t xa = rowstate.x0[y], xb = rowstate.x1[y];
                const uint16_t *src = &canvas[(size_t)y * CLK_SCR_W + xa];
                uint16_t *d = s_band[r] + xa;
                for (int32_t x = xa; x <= xb; x++) {
                    const uint16_t v = *src++;
                    *d++ = (uint16_t)((v >> 8) | (v << 8));
                }
            }
            s_hk_rest_us += (uint64_t)(esp_timer_get_time() - t0);

            t0 = esp_timer_get_time();
            for (int i = 0; i < nj; i++) {
                /* One timer pair per (layer, band), not per (layer, row): the
                 * per-layer split is what found the carriage, and at ~1500 calls
                 * per commit esp_timer_get_time() itself was starting to cost the
                 * ~0.2 ms being measured. */
                const int64_t tr = esp_timer_get_time();
                if (!jc[i].rot) {
                    /* Static (identity) layer: texels are read straight across the
                     * row, so one call per row is already the cheapest order. */
                    for (int r = 0; r < nr; r++) {
                        const int32_t y = y0 + r;
                        gfx_paint_row_ctx(&jc[i], y, rowstate.x0[y], rowstate.x1[y], s_band[r]);
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
                            int32_t xa = rowstate.x0[y], xb = rowstate.x1[y];
                            if (xb < tx || xa > te) continue;
                            if (xa < tx) xa = tx;
                            if (xb > te) xb = te;
                            gfx_paint_row_ctx(&jc[i], y, xa, xb, s_band[r]);
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
                int32_t xa = rowstate.x0[y], xb = rowstate.x1[y];
                uint16_t *fbrow = fb + (size_t)y * CLK_SCR_W;
                bool send = rowstate.force[y] != 0;
                /* The keep-out cut, in the one place it has to live: the staged
                 * row still holds the dial's pixels for the rect's span, but
                 * they never reach the mirror while the chrome stands, so the
                 * chip's own bytes stay intact and the row that DMAes carries
                 * them. */
                const bool ko = s_ko_on && y >= s_ko[1] && y <= s_ko[3];
                const int32_t ka = s_ko[0], kb = s_ko[2];
                if (!ko || xb < ka || xa > kb) {              /* rect misses the row */
                    for (int32_t x = xa; x <= xb; x++) {
                        const uint16_t v = s_band[r][x];
                        if (fbrow[x] != v) { fbrow[x] = v; send = true; }
                    }
                } else {
                    const int32_t lo = xa < ka ? xa : ka;     /* left run   */
                    const int32_t hi = xb > kb ? xb : kb;     /* right run  */
                    for (int32_t x = xa; x < lo; x++) {
                        const uint16_t v = s_band[r][x];
                        if (fbrow[x] != v) { fbrow[x] = v; send = true; }
                    }
                    for (int32_t x = hi + 1; x <= xb; x++) {
                        const uint16_t v = s_band[r][x];
                        if (fbrow[x] != v) { fbrow[x] = v; send = true; }
                    }
                }
                display_commit_row(y, send);
            }
            s_hk_cmp_us += (uint64_t)(esp_timer_get_time() - t0);

            y0 += nr;
        }

        memset(rowstate.need, 0, sizeof(rowstate.need));
        memset(rowstate.force, 0, sizeof(rowstate.force));
}
