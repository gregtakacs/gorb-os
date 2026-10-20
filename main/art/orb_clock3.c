/* orb_clock3.c — the FORMAT 3 clock section projected into the compiled dial.
 *
 * P6.5 §25.5(2). The clock has always drawn from two things: one clk_theme_t and
 * one base pointer the colour offsets index. FORMAT 2 supplied both from a bake;
 * FORMAT 3 supplies an element table, a sprite catalogue and 26 carried scalars,
 * and this file is the projection between them. It is `bake_clock.py
 * --from-bundle` with the same rulings and the same refusal list, so the pair a
 * kit produces and the picture a bundle produces cannot drift: same field
 * identities (§25.2), same gif-table derivation (§25.6), same refusals (§25.3).
 *
 * What it does NOT do is own the pixels. `out_art_off` names where the section's
 * bank sits inside a mapped art region; the caller maps it, and the dial's blob
 * base moves to that offset for the life of the theme. Nothing is allocated here,
 * so an un-projectable table costs a refusal and not a leak. The bundle's name
 * stays with the caller, which needs it for the row and the ledger.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "orb_clock3.h"

/* theme_schema.CLASS_NAMES, mirrored. orb_assets.h carries the same words behind
 * orb_asset_class_name[], and this file cannot reach it: that header pulls
 * esp_err.h, and this one compiles on a host. The suite pins the mirror, so a
 * class renamed on one side and not the other fails there — the discipline every
 * other counted vocabulary in this firmware uses. */
static const char *const CLASS_NAMES[8] = {
    "text", "sprite", "hand", "plate", "ring", "bank", "layer", "status"
};

#define CLS_SPRITE 1
#define CLS_HAND   2
#define CLS_PLATE  3
#define EF_HIDDEN  8            /* theme_schema.EF_HIDDEN */

#define PLATE_BYTES_T(w, h) ((uint64_t)(w) * (uint64_t)(h) * 2u)
#define SPRITE_BYTES_T(s)   ((uint64_t)(s)->w * (uint64_t)(s)->h * 3u)

/* One refusal sentence at a time, for the same reason and with the same printf
 * checking orb_bundle.c's reject() has: the caller returns the pointer at once. */
static char s_msg[160];

static const char *reject(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static const char *reject(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    va_end(ap);
    return s_msg;
}

/* The one place the scalar dialect is read. Word indices are
 * orbtheme.THEME_FIELDS' order — the same 26 words the FORMAT 2 blob carries in
 * the same order, which is what lets this file and orb_theme.c's FORMAT 2 parse
 * agree on one dialect without either including the other. orb_bundle.c owns the
 * sign table; the three signed fields are the only ones read through the
 * sign-extending form. */
static int32_t S(const orb_bundle_sec_t *s, uint32_t w)
{ return orb_bundle_scalar_sext(s, w); }
static uint32_t U(const orb_bundle_sec_t *s, uint32_t w)
{ return orb_bundle_scalar(s, w); }

#define W_BG              0u
#define W_PLATE_FOLLOW    1u
#define W_SHADOW_ON       2u
#define W_SHADOW_DX       3u
#define W_SHADOW_DY       4u
#define W_SECOND_SWEEP    5u
#define W_SECOND_RAILWAY  6u
#define W_TEXT_OVER_HANDS 7u
#define W_TICK_RATE       8u
#define W_BG_FRAMES       9u
#define W_BG_FPS         10u
#define W_BG_LOOP        11u
#define W_BG_EVERY       12u
#define W_PLATE_OFF      13u
#define W_ORDER0         14u
#define W_ORDER_N        19u
#define W_MATTE_MASK     20u
#define W_BG_MODE        21u
#define W_BG_KEY_OFF     22u
#define W_GIF_FRAMES     23u
#define W_TOURBILLON_N   24u
#define W_TOURBILLON_FPS 25u

/* A layer the table says nothing about. §25.2's answer, read from the other end:
 * the offer's slot exists and this theme does not draw there, so the record is
 * blank and shown=0, resting at the plate's centre — the exact record a kit bake
 * writes for an unused slot, which is what keeps the pair and the picture one
 * definition rather than two. */
static void blank_sprite(clk_sprite_t *sp, int16_t cx, int16_t cy)
{
    memset(sp, 0, sizeof *sp);
    sp->centerx = cx;
    sp->centery = cy;
}

/* §25.2's identity, run backwards: the element names a draw point and the pivot
 * relative to it, and the .h wants the draw point, the centre and the crop-space
 * pivot. `crop_pivot` is NULL for every layer except a shadow, whose crop pivot is
 * its hand's centre minus its own draw point — the light offset the source baked,
 * recoverable and never carried. */
static void fill_sprite(clk_sprite_t *sp, const orb_bundle_elem_t *e,
                        const orb_bundle_spr_t *plane, const int16_t *crop_pivot)
{
    memset(sp, 0, sizeof *sp);
    sp->w         = e->w;
    sp->h         = e->h;
    sp->offx      = e->x;
    sp->offy      = e->y;
    sp->centerx   = (int16_t)((int32_t)e->x + e->pivot_x);
    sp->centery   = (int16_t)((int32_t)e->y + e->pivot_y);
    sp->pivotx    = crop_pivot ? crop_pivot[0] : e->pivot_x;
    sp->pivoty    = crop_pivot ? crop_pivot[1] : e->pivot_y;
    sp->blend     = 0;                    /* the dialect carries no blend column */
    sp->show      = (uint8_t)(e->flags & EF_HIDDEN ? 0 : 1);
    sp->color_off = plane->off;
}

const char *orb_clock3_parse(const orb_bundle_sec_t *sec, orb_clock3_io_t *io)
{
    clk_theme_t *t = io->theme;
    const uint32_t ne = sec->n_elem, ns = sec->n_spr;
    const uint32_t art = sec->art_size;
    const uint32_t scr_w = sec->scr_w, scr_h = sec->scr_h;

    memset(t, 0, sizeof *t);
    io->out_art_off  = sec->art_off;
    io->out_art_size = art;
    if (!ne || !ns) return "a clock section with no elements or no planes draws nothing";

    /* The panel pair rides the index record (§25.2), and the dial it feeds is
     * compiled for one panel. FORMAT 2's header gate refuses the same lie. */
    if (scr_w != CLK_SCR_W || scr_h != CLK_SCR_H)
        return "a format-3 clock section made for a different panel";

    /* ---- the table, indexed by (class, slot): the identity §25.2 ruled ---- */
    int16_t at_plate[2] = { -1, -1 }, at_hand[5] = { -1,-1,-1,-1,-1 };
    int16_t at_shadow[5] = { -1,-1,-1,-1,-1 };
    for (uint32_t i = 0; i < ne; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        int16_t *cell = NULL;
        const char *nm = e.cls < 8 ? CLASS_NAMES[e.cls] : "?";
        switch (e.cls) {
            case CLS_PLATE:
                if (e.slot > 1)
                    return reject("a plate at slot %u: the clock dialect is one plate "
                                  "and one overlay", (unsigned)e.slot);
                cell = &at_plate[e.slot];
                break;
            case CLS_HAND:
                if (e.slot >= 5)
                    return reject("a tourbillon (hand slot %u): the fan-out mapping is "
                                  "not ruled, and the .h's diff-rect fields ride no "
                                  "bundle column", (unsigned)e.slot);
                cell = &at_hand[e.slot];
                break;
            case CLS_SPRITE:
                if (e.slot >= 5)
                    return reject("element %u is class %s slot %u: the compiled clock "
                                  "has no field for it, and §25.3 rules it into the "
                                  "complication mapping",
                                  (unsigned)e.n, nm, (unsigned)e.slot);
                cell = &at_shadow[e.slot];
                break;
            default:
                return reject("element %u is class %s: the compiled clock has no field "
                              "for it, and §25.3 rules it into the complication "
                              "mapping", (unsigned)e.n, nm);
        }
        if (*cell >= 0)
            return reject("two rows for %s slot %u: one slot draws one layer",
                          nm, (unsigned)e.slot);
        *cell = (int16_t)i;
    }

    /* The scalars, verbatim (D31), and then the two a table can contradict. */
    t->bg              = U(sec, W_BG);
    t->plate_follow    = (uint8_t)U(sec, W_PLATE_FOLLOW);
    t->shadow_on       = (uint8_t)U(sec, W_SHADOW_ON);
    t->shadow_dx       = (int8_t)S(sec, W_SHADOW_DX);
    t->shadow_dy       = (int8_t)S(sec, W_SHADOW_DY);
    t->second_sweep    = (uint8_t)U(sec, W_SECOND_SWEEP);
    t->second_railway  = (uint8_t)U(sec, W_SECOND_RAILWAY);
    t->text_over_hands = (uint8_t)U(sec, W_TEXT_OVER_HANDS);
    t->tick_rate       = (uint8_t)U(sec, W_TICK_RATE);
    t->bg_frames       = (uint16_t)U(sec, W_BG_FRAMES);
    t->bg_fps          = (int16_t)S(sec, W_BG_FPS);
    t->bg_loop         = (uint8_t)U(sec, W_BG_LOOP);
    t->bg_every        = (uint16_t)U(sec, W_BG_EVERY);
    t->plate_off       = U(sec, W_PLATE_OFF);
    for (int i = 0; i < 5; i++)
        t->order[i] = (uint8_t)U(sec, W_ORDER0 + (uint32_t)i);
    t->order_n    = (uint8_t)U(sec, W_ORDER_N);
    t->matte_mask = (uint8_t)U(sec, W_MATTE_MASK);
    t->bg_mode    = (uint8_t)U(sec, W_BG_MODE);
    t->bg_key_off = U(sec, W_BG_KEY_OFF);
    t->gif_frames = (uint16_t)U(sec, W_GIF_FRAMES);
    t->tourbillon_n   = (uint16_t)U(sec, W_TOURBILLON_N);
    t->tourbillon_fps = (uint16_t)U(sec, W_TOURBILLON_FPS);

    if (t->order_n > 5)              return "layer order longer than 5";
    if (t->tourbillon_n)
        return reject("tourbillon_n %u with no tourbillon row: the scalars and the "
                      "table disagree, and the pair would name a bank the art does "
                      "not hold", (unsigned)t->tourbillon_n);
    if (t->bg_frames)
        return reject("%u distinct plate frames: those planes ride the gif table a "
                      "FORMAT 3 clock section does not carry",
                      (unsigned)t->bg_frames);
    if (t->bg_mode == 1 && t->gif_frames < 2)
        return "delta-rect background with fewer than two frames";
    if ((uint32_t)t->gif_frames + 1u > io->first_cap ||
        (uint32_t)t->gif_frames > io->delays_cap)
        return "more GIF frames than the dial holds";


    /* The plate: required, full-panel, one frame. The last two are the compiled
     * dial's own assumptions — it paints the background edge to edge and takes its
     * blank-layer centre from the plate's middle — so a table that breaks them is
     * not a clock this file can lie about. */
    if (at_plate[0] < 0)
        return "no plate row: the pair's blank-layer centres come from the plate's "
               "own extent, and a clock with no plate has no centre";
    orb_bundle_elem_t plate;
    orb_bundle_elem_at(sec, (uint32_t)at_plate[0], &plate);
    if (plate.x || plate.y || plate.w != (uint16_t)scr_w || plate.h != (uint16_t)scr_h)
        return reject("a plate at extent (%d,%d,%u,%u) of a %ux%u panel: the compiled "
                      "clock draws its background full-screen, so a plate that is not "
                      "the whole panel is not a plate",
                      (int)plate.x, (int)plate.y, (unsigned)plate.w,
                      (unsigned)plate.h, (unsigned)scr_w, (unsigned)scr_h);
    if (plate.n_frames != 1)
        return reject("a gif bank: the plate names %u frames, and the delta rects that "
                      "would carry them are the shape §25.3 refuses — a FORMAT 3 clock "
                      "section holds one keyframe", (unsigned)plate.n_frames);
    const int16_t cx = (int16_t)(plate.x + plate.w / 2);
    const int16_t cy = (int16_t)(plate.y + plate.h / 2);

    /* The catalogue against the bank, before any offset is trusted: §25.3's last
     * bullet, read from the device end. Row 0 is colour-only because it is the
     * plate, and every other plane carries an alpha stride, so each row's cost is
     * computed from its own extent rather than believed. A bank this catalogue does
     * not account for whole is a bank this projection does not understand, and the
     * extents below would only be a guess about it. */
    uint64_t at = 0;
    for (uint32_t i = 0; i < ns; i++) {
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, i, &sp);
        const uint64_t want = PLATE_BYTES_T(sp.w, sp.h)
                            + (i ? (uint64_t)sp.w * sp.h : 0);
        if (sp.off != at)
            return reject("the sprite catalogue is not contiguous at row %u: it starts "
                          "at %lu, the previous plane ends at %lu",
                          (unsigned)i, (unsigned long)sp.off, (unsigned long)at);
        if (sp.size != want)
            return reject("catalogue row %u claims %lu B of a %lux%lu plane; that "
                          "extent is %lu B", (unsigned)i, (unsigned long)sp.size,
                          (unsigned long)sp.w, (unsigned long)sp.h,
                          (unsigned long)want);
        at += sp.size;
    }
    if (at != art)
        return reject("the catalogue accounts for %lu B of an art region of %u B",
                      (unsigned long)at, (unsigned)art);

    /* Every row's plane: seated in the catalogue, and the size the row claims. A
     * table and a catalogue describing different layers would draw a torn sprite,
     * and wrong pixels on a wall clock is what D26's direction refuses. */
    for (uint32_t i = 0; i < ne; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.frame0 >= ns)
            return reject("element %u points at sprite row %u of a table with %u rows",
                          (unsigned)e.n, (unsigned)e.frame0, (unsigned)ns);
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, e.frame0, &sp);
        if (sp.w != e.w || sp.h != e.h)
            return reject("element %u is %ux%u while its plane is %lux%lu: the table "
                          "and the catalogue describe different layers",
                          (unsigned)e.n, (unsigned)e.w, (unsigned)e.h,
                          (unsigned long)sp.w, (unsigned long)sp.h);
    }

    /* ---- the layers, in the dialect's own vocabulary ---- */
    for (int i = 0; i < 5; i++) {
        blank_sprite(&t->hand[i], cx, cy);
        blank_sprite(&t->shadow[i], cx, cy);
    }
    blank_sprite(&t->overlay, 0, 0);
    for (int i = 0; i < CLK_TOURB_MAX; i++) blank_sprite(&t->tourbillon[i], 0, 0);

    if (at_plate[1] >= 0) {
        orb_bundle_elem_t e;
        orb_bundle_spr_t sp;
        orb_bundle_elem_at(sec, (uint32_t)at_plate[1], &e);
        orb_bundle_spr_at(sec, e.frame0, &sp);
        fill_sprite(&t->overlay, &e, &sp, NULL);
    }
    for (int i = 0; i < 5; i++) {
        if (at_hand[i] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_spr_t sp;
        orb_bundle_elem_at(sec, (uint32_t)at_hand[i], &e);
        orb_bundle_spr_at(sec, e.frame0, &sp);
        fill_sprite(&t->hand[i], &e, &sp, NULL);
    }
    for (int i = 0; i < 5; i++) {
        if (at_shadow[i] < 0) continue;
        if (at_hand[i] < 0)
            return reject("a shadow at slot %u with no hand there: a shadow's crop "
                          "pivot is its hand's centre minus its own draw point, so the "
                          "light offset is only recoverable from the hand it hangs "
                          "under", (unsigned)i);
        orb_bundle_elem_t e;
        orb_bundle_spr_t sp;
        orb_bundle_elem_at(sec, (uint32_t)at_shadow[i], &e);
        orb_bundle_spr_at(sec, e.frame0, &sp);
        const int16_t crop[2] = { (int16_t)(t->hand[i].centerx - e.x),
                                  (int16_t)(t->hand[i].centery - e.y) };
        fill_sprite(&t->shadow[i], &e, &sp, crop);
    }

    /* Extents, layer by layer, in FORMAT 2's own words: the same lie at either door
     * gets the same sentence. A layer the dial will not look at is not checked, for
     * the reason orb_theme_geom_parse gives. The plate and the keyframe are checked
     * against the panel rather than against a catalogue row because their size is
     * the panel's, which is the one extent a clock section cannot lie about. */
    if (t->plate_off % 2u ||
        (uint64_t)t->plate_off + PLATE_BYTES_T(scr_w, scr_h) > (uint64_t)art)
        return "the plate pixels lie outside the declared payload";
    for (int i = 0; i < 5; i++) {
        const clk_sprite_t *h = &t->hand[i];
        if (h->show && h->w && h->h &&
            (uint64_t)h->color_off + SPRITE_BYTES_T(h) > art)
            return "hand pixels lie outside the declared payload";
        const clk_sprite_t *d = &t->shadow[i];
        if (t->shadow_on && d->show && d->w && d->h &&
            (uint64_t)d->color_off + SPRITE_BYTES_T(d) > art)
            return "shadow pixels lie outside the declared payload";
    }
    if (t->overlay.w && t->overlay.h &&
        (uint64_t)t->overlay.color_off + SPRITE_BYTES_T(&t->overlay) > art)
        return "overlay pixels lie outside the declared payload";
    if (t->bg_mode == 1 &&
        (t->bg_key_off % 2u ||
         (uint64_t)t->bg_key_off + PLATE_BYTES_T(scr_w, scr_h) > (uint64_t)art))
        return "the keyframe pixels lie outside the declared payload";

    /* ---- the gif block, derived (§25.6) ----
     * One zero rect: the compiled path the shipped pair takes is the keyframe copy,
     * which discards the rect list, and a FORMAT 3 clock section carries no delta
     * bank to index. Every window boundary is therefore 0, and the plate-frame table
     * holds one frame at the keyframe. R6 retired the per-frame delay, so the delay
     * table carries the OS beat — the same uniform figure a PNG frame pack's own
     * bake printed, which is the 67 ms the shipped av-kit pair carries. */
    io->rects[0].x = io->rects[0].y = 0;
    io->rects[0].w = io->rects[0].h = 0;
    io->rects[0].off = 0;
    for (uint32_t i = 0; i <= (uint32_t)t->gif_frames; i++) io->rect_first[i] = 0;
    const uint32_t beat = (uint32_t)(t->tick_rate ? t->tick_rate : 1);
    uint32_t ms = (1000u + beat / 2u) / beat;
    if (!ms) ms = 1;
    for (uint32_t i = 0; i < (uint32_t)t->gif_frames; i++)
        io->gif_delays[i] = (uint16_t)ms;
    io->bg_frames[0] = 0;
    return NULL;
}
