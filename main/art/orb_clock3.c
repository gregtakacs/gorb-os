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
static const char *const CLASS_NAMES[9] = {
    "text", "sprite", "hand", "plate", "ring", "bank", "layer", "status", "aux"
};

#define CLS_SPRITE 1
#define CLS_HAND   2
#define CLS_PLATE  3
#define CLS_BANK   5
#define CLS_AUX    8            /* D34: theme_schema.CLASS_AUX, appended at 8 */
#define EF_HIDDEN  8            /* theme_schema.EF_HIDDEN */
#define AUX_F_SHADOW 0x10       /* aux row: the shadow TWIN of the row before it.
                                 * FORMAT 3's element flag column is a u8, so the
                                 * runtime's CLK_FLAG_AUX_SHADOW (1 << 8) cannot ride
                                 * it; bit 4 is the slot the element dialect leaves
                                 * free for a pointer (alpha/loop/os-adv mean nothing
                                 * to a single-frame row). orbtheme-baker.js writes it
                                 * as AUX_F_SHADOW3; the projection below maps it onto
                                 * the runtime flag, so clock_view's pairing rules
                                 * read one vocabulary however the theme arrived. */

/* D34's motion ids (theme_schema.MOTION_NAMES, in that order). The first three are
 * the ones clk_aux_t.kind has always read; the two slides are new, and each names
 * the AXIS it travels as well as the fact that it is not an angle — so `span` and
 * `zero` are pixels on those rows and tenths of a degree on the other three, with no
 * extra column anywhere claiming which. MOTION_LAST is the bound the row's byte is
 * checked against, and it moves with the vocabulary rather than being copied into a
 * count that can lag it. */
#define MOTION_ROTATE      0
#define MOTION_RETROGRADE  1
#define MOTION_INDEXED     2
#define MOTION_SLIDE_X     3
#define MOTION_SLIDE_Y     4
#define MOTION_LAST        MOTION_SLIDE_Y
#define MOTION_IS_SLIDE(k) ((k) == MOTION_SLIDE_X || (k) == MOTION_SLIDE_Y)

/* theme_schema.CLOCK_WINDOW_DOMAIN, mirrored for the device's re-check of a
 * hostile bundle (D32 ruling B: the frame-to-value map is the clock's calendar,
 * not a carried figure, and a month window is twelve plates because a year has
 * twelve months). An entry of count 0 is a source with no fixed domain here:
 * `none` gets its own sentence below, and a source the dial cannot drive at all
 * is refused as one. The suite pins this mirror against the Python table — the
 * same discipline CLASS_NAMES above uses. */
typedef struct { int16_t value_min; uint16_t count; } clk_domain_t;
static const clk_domain_t WINDOW_DOMAIN[CLK_SRC__MAX] = {
    [CLK_SRC_WEEKDAY]     = { 0,   7 },
    [CLK_SRC_MONTH]       = { 0,  12 },
    [CLK_SRC_DATE]        = { 1,  31 },
    [CLK_SRC_HOUR12]      = { 0,  12 },
    [CLK_SRC_HOUR24]      = { 0,  24 },
    [CLK_SRC_MINUTE]      = { 0,  60 },
    [CLK_SRC_SECOND]      = { 0,  60 },
    [CLK_SRC_MOON_AGE]    = { 0,  30 },
    [CLK_SRC_AMPM]        = { 0,   2 },
    [CLK_SRC_WEEK]        = { 1,  53 },
    [CLK_SRC_DAY_OF_YEAR] = { 1, 366 },
    [CLK_SRC_QUARTER]     = { 1,   4 },
};

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
    int16_t at_plate[2] = { -1, -1 }, at_hand[6] = { -1,-1,-1,-1,-1,-1 };
    int16_t at_shadow[5] = { -1,-1,-1,-1,-1 };
    int16_t at_aux[CLK_AUX_MAX];
    for (int i = 0; i < CLK_AUX_MAX; i++) at_aux[i] = -1;   /* the stack zeroes no -1 */
    int16_t at_bank[CLK_COMP_MAX] = { -1,-1,-1,-1,-1,-1,-1,-1,
                                      -1,-1,-1,-1,-1,-1,-1,-1 };
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
                /* §31 rule 5: the animation is the plate behind the hands. An
                 * overlay naming frames would want the plate's whole machinery
                 * on the glass, and that is a different machine. */
                if (e.slot == 1 && e.n_frames != 1)
                    return reject("an animated glass: the overlay names %u frames — "
                                  "§31 animates the plate behind the hands",
                                  (unsigned)e.n_frames);
                cell = &at_plate[e.slot];
                break;
            case CLS_HAND:
                /* D32 ruling C: slot 5 is the carriage — a hand-shaped layer
                 * whose frames animate the escapement while the seconds angle
                 * still turns it. Its row geometry places it and its start
                 * orientation rides its `degrees`. Slots past it are no slot
                 * this dial declares; the scalar pair and the frame capacity
                 * are checked with the scalars, where both halves can disagree. */
                if (e.slot >= 6)
                    return reject("a hand at slot %u: the clock declares six hand slots "
                                  "— three live hands, a rotating disc, its "
                                  "counterweight, and the carriage — and no more",
                                  (unsigned)e.slot);
                cell = &at_hand[e.slot];
                break;
            case CLS_BANK:
                /* D32 ruling A/E: a bank row names a complication slot, and its
                 * kind/src columns say how its plates draw and what indexes
                 * them. The projection below owns what they become. */
                if (e.slot >= CLK_COMP_MAX)
                    return reject("a bank at slot %u: the clock declares %u complication "
                                  "slots and the manifest of any app may not invent one",
                                  (unsigned)e.slot, (unsigned)CLK_COMP_MAX);
                cell = &at_bank[e.slot];
                break;
            case CLS_SPRITE:
                if (e.slot >= 5)
                    return reject("element %u is class %s slot %u: the compiled clock "
                                  "has no field for it, and §25.3 rules it into the "
                                  "complication mapping",
                                  (unsigned)e.n, nm, (unsigned)e.slot);
                cell = &at_shadow[e.slot];
                break;
            case CLS_AUX:
                /* D34: a pointer is addressed by its own ordinal, not by a comp tag and
                 * not by a hand slot — the row's `slot` IS the index into the aux table,
                 * which is what lets a theme say "the seconds hand of the six o'clock
                 * sub-dial" without the dial knowing what a sub-dial is. */
                if (e.slot >= CLK_AUX_MAX)
                    return reject("a pointer at slot %u: the clock declares %u pointer "
                                  "slots and the manifest of any app may not invent one",
                                  (unsigned)e.slot, (unsigned)CLK_AUX_MAX);
                cell = &at_aux[e.slot];
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
    /* The tourbillon pair (D32 ruling C): the scalar and the row must name one
     * bank. R-G keeps the wall sampler and the carried tempo, so word 25 is
     * already the animation's speed and the row's `degrees` its start. The diff
     * rects the FORMAT 2 .h carried are DERIVED here and not carried: frame 0
     * differs from no predecessor, and every later frame's changed rect is the
     * bank's own business — the clock3 io has no rect list, and the dial's
     * chord-marking path already rebuilds them from the planes it is shown. */
    if (at_hand[5] >= 0) {
        orb_bundle_elem_t car;
        orb_bundle_elem_at(sec, (uint32_t)at_hand[5], &car);
        if (!t->tourbillon_n)
            return reject("a carriage row with tourbillon_n 0: the scalars and the "
                          "table disagree, and the pair would name a bank the art "
                          "does not hold");
        if (t->tourbillon_n != car.n_frames)
            return reject("a carriage row naming %u frames while the scalars name %u: "
                          "the row's plane is the animation's length",
                          (unsigned)car.n_frames, (unsigned)t->tourbillon_n);
        if (car.n_frames > CLK_TOURB_MAX)
            return reject("a carriage naming %u frames: the dial holds %u, and the "
                          "rest is a bank it has no room for",
                          (unsigned)car.n_frames, (unsigned)CLK_TOURB_MAX);
    }
    if (t->tourbillon_n && at_hand[5] < 0)
        return reject("tourbillon_n %u with no tourbillon row: the scalars and the "
                      "table disagree, and the pair would name a bank the art does "
                      "not hold", (unsigned)t->tourbillon_n);
    /* D34 bench: the composer exported order:[1,2] for hands seated at slots
     * 0,1 — FORMAT 2's show-table +1 carried into a dialect where the table
     * holds slot ids verbatim — and the watch painted minute, skipped the empty
     * slot 2, and never drew hour. A live hand the order omits is art the file
     * carries and the dial never paints: the same half-drawn dial §25 refuses
     * for a shadow without its hand. The carriage at slot 5 rides the
     * tourbillon pair above, and hidden or matte-folded rows make no promise
     * to paint. */
    for (int k = 0; k < 5; k++) {
        if (at_hand[k] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_hand[k], &e);
        if (e.flags & EF_HIDDEN) continue;
        if (t->matte_mask & (1u << k)) continue;         /* folded into the bank */
        int named = 0;
        for (int oi = 0; oi < t->order_n; oi++)
            if (t->order[oi] == k) named = 1;
        if (!named)
            return reject("the hand at slot %d is seated but the order table names no %d: "
                          "a live hand the order omits draws nothing on the watch", k, k);
    }
    /* §31: bg_frames is a keyframe bank's length, not a gif table's. Delta mode
     * under a count is still §25.3's shape — whole planes named by a vocabulary
     * of rects the section does not carry — and the row cross-check lands with
     * the plate below, where both halves can disagree. */
    if (t->bg_frames && t->bg_mode == 1)
        return reject("%u distinct plate frames: those planes ride the gif table a "
                      "FORMAT 3 clock section does not carry",
                      (unsigned)t->bg_frames);
    if (t->bg_frames >= CLK_BG_MAX)
        return reject("a plate animation of %u frames: the dial holds %u, and the "
                      "rest is a bank it has no room for",
                      (unsigned)t->bg_frames + 1u, (unsigned)CLK_BG_MAX);
    if (t->bg_mode == 1 && t->gif_frames < 2)
        return "delta-rect background with fewer than two frames";
    if ((uint32_t)t->gif_frames + 1u > io->first_cap ||
        (uint32_t)t->gif_frames > io->delays_cap)
        return "more GIF frames than the dial holds";


    /* The plate: required, full-panel, and either still or a keyframe bank
     * (§31). The bank is the carriage's mechanism pointed at the background:
     * N whole-panel frames, contiguous at the row's frame0, colour-only, and
     * advanced by the OS. The last two below are the compiled dial's own
     * assumptions — it paints the background edge to edge and takes its
     * blank-layer centre from the plate's middle — so a table that breaks them
     * is not a clock this file can lie about. */
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
    /* The writer refuses a zero-frame row at its own door (theme_schema: 0 draws
     * nothing); this is the damaged-card backstop, where a wrapped count would
     * otherwise read as a still plate and paint no bank at all. */
    if (!plate.n_frames)
        return reject("a plate row naming 0 frames: the background is a layer that "
                      "draws, and a row with no plane draws nothing");
    /* One length, said once. The row names the planes and the scalar names the
     * loop, and either half lying about the other is the same disagreement the
     * carriage pair refuses; a keyframe bank with a gif count on it is two
     * animation vocabularies claiming one background; and a one-shot with no
     * tempo is a frame that never arrives, so it is refused rather than drawn
     * as a freeze. The plate's own plane — the consumer's frame 0 — is seated
     * below, where the catalogue has been walked and the row bounds proved. */
    const uint8_t plate_anim = plate.n_frames > 1u || t->bg_frames != 0;
    if (plate_anim) {
        if (t->bg_frames != (uint16_t)(plate.n_frames - 1u))
            return reject("a plate row naming %u frames while the scalars name %u "
                          "extra: the row's planes are the animation's length",
                          (unsigned)plate.n_frames, (unsigned)t->bg_frames);
        if (t->gif_frames)
            return reject("an animated plate naming %u gif frames: a keyframe bank "
                          "carries whole planes and no delta rects",
                          (unsigned)t->gif_frames);
        if (!t->bg_loop && t->bg_fps < 1)
            return reject("a one-shot plate bank at bg_fps %d: an animation with no "
                          "tempo is a frame that never arrives", (int)t->bg_fps);
    }
    const int16_t cx = (int16_t)(plate.x + plate.w / 2);
    const int16_t cy = (int16_t)(plate.y + plate.h / 2);

    /* The catalogue against the bank, before any offset is trusted: §25.3's last
     * bullet, read from the device end. The plate's rows are colour-only because
     * they are the background — the plate itself and, since §31, every keyframe
     * of its bank, the consumer painting each one the plate widget already
     * paints — and every other plane carries an alpha stride, so each row's cost
     * is computed from its own extent rather than believed. A bank this catalogue
     * does not account for whole is a bank this projection does not understand,
     * and the extents below would only be a guess about it. */
    uint64_t at = 0;
    for (uint32_t i = 0; i < ns; i++) {
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, i, &sp);
        const int on_plate = i >= plate.frame0 && i < plate.frame0 + plate.n_frames;
        const uint64_t want = PLATE_BYTES_T(sp.w, sp.h)
                            + (on_plate ? 0 : (uint64_t)sp.w * sp.h);
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
     * and wrong pixels on a wall clock is what D26's direction refuses. A
     * multi-frame row (the carriage, a bank window) is checked frame by frame:
     * the frames are consecutive catalogue rows, and one plane at frame0 is only
     * the first of what the row animates through. */
    for (uint32_t i = 0; i < ne; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.frame0 >= ns)
            return reject("element %u points at sprite row %u of a table with %u rows",
                          (unsigned)e.n, (unsigned)e.frame0, (unsigned)ns);
        if ((uint64_t)e.frame0 + e.n_frames > ns)
            return reject("element %u names %u frames from row %u of a table with %u "
                          "rows: the animation runs off the end of the catalogue",
                          (unsigned)e.n, (unsigned)e.n_frames, (unsigned)e.frame0,
                          (unsigned)ns);
        for (uint32_t f = 0; f < e.n_frames; f++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, e.frame0 + f, &sp);
            if (sp.w != e.w || sp.h != e.h)
                return reject("element %u is %ux%u while its plane is %lux%lu: the "
                              "table and the catalogue describe different layers",
                              (unsigned)e.n, (unsigned)e.w, (unsigned)e.h,
                              (unsigned long)sp.w, (unsigned long)sp.h);
        }
    }

    /* Frame 0 of the loop is the plate: what the consumer paints at rest, what
     * the plate scalar points at, and what a still keyframe table names, are one
     * plane. A bank whose first frame sat elsewhere would show one frame twice
     * and another never. */
    if (plate_anim) {
        orb_bundle_spr_t pf;
        orb_bundle_spr_at(sec, plate.frame0, &pf);
        if (t->plate_off != pf.off)
            return reject("a plate bank whose first plane sits at %lu while the "
                          "scalars paint the plate at %u: frame 0 of the loop is "
                          "the plate",
                          (unsigned long)pf.off, (unsigned)t->plate_off);
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

    /* ---- the carriage (D32 ruling C): one row, a bank of frames ----
     * Every plane of the row's frame window is one record of the bank. The
     * geometry — draw point, pivot, centre — is the ROW's and shared by every
     * frame, which is what lets the dial rotate any frame about one pivot and
     * dirty one box: the same one-bbox rule a kit bake enforced by cropping a
     * whole cycle to one ink box, arrived at from the other end. The diff
     * rects a FORMAT 2 .h pre-computed are zero here by proof, not by luck:
     * the chord-marking path rebuilds the changed ink from the plane it is
     * shown, and the diff-box path degrades to the swept box it already
     * carries for a frame with no rect. The row's `degrees` is the start
     * orientation (R-G): the wall sampler stays, the dial adds this. */
    if (at_hand[5] >= 0) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_hand[5], &e);
        t->tourbillon_start = e.degrees;
        for (uint32_t f = 0; f < e.n_frames; f++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, e.frame0 + f, &sp);
            fill_sprite(&t->tourbillon[f], &e, &sp, NULL);
        }
    }

    /* ---- the complications (D32 rulings A, B, D) ----
     * A BANK row is one comp: the box, kind and src are the row's, the tile
     * base and stride the catalogue's, and the frame-to-value domain the
     * clock's (R-B: a month window is twelve plates because a year has twelve
     * months, so the row's count is CHECKED against the table, never invented
     * by it). The row's `order` is the window's z bucket, verbatim (R-D), and
     * the dialect's bound on it is the dial's five live layers. A kind no
     * FORMAT 3 row carries the fields to draw is refused here and now, in
     * words the owner can read at either end of the wire (R-E, R-F). */
    io->out_n_comps = 0;
    for (int slot = 0; slot < CLK_COMP_MAX; slot++) {
        if (at_bank[slot] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_bank[slot], &e);
        if (e.kind >= CLK_KIND__MAX)
            return reject("a bank at slot %u drawn as kind %u: the dialect names %u "
                          "ways to plate a value and this names another",
                          (unsigned)slot, (unsigned)e.kind, (unsigned)CLK_KIND__MAX);
        if (e.kind != CLK_KIND_WINDOW)
            return reject("a bank at slot %u drawn as kind %u: the element row carries "
                          "plates and a box, and a %s needs the glyph, circle or blend "
                          "fields no row holds",
                          (unsigned)slot, (unsigned)e.kind,
                          e.kind == CLK_KIND_STRIP ? "strip"
                          : e.kind == CLK_KIND_RING ? "ring" : "flare");
        if (e.src >= CLK_SRC__MAX)
            return reject("a bank at slot %u driven by source %u: this dial reads %u "
                          "values and none of them is that",
                          (unsigned)slot, (unsigned)e.src, (unsigned)CLK_SRC__MAX);
        if (e.src == CLK_SRC_BATTERY || e.src == CLK_SRC_POWER_RESERVE ||
            e.src == CLK_SRC_TEXT || e.src == CLK_SRC_VALUE)
            return reject("a bank at slot %u driven by source %u: this dial has no "
                          "such input, and D32's ruling refuses the flip rather than "
                          "ship a comp that would draw nothing",
                          (unsigned)slot, (unsigned)e.src);
        uint16_t count;
        int16_t  value_min = 0;
        if (e.src == CLK_SRC_NONE) {
            if (e.n_frames != 1)
                return reject("a static window at bank slot %u names %u plates: a value "
                              "that never changes indexes one",
                              (unsigned)slot, (unsigned)e.n_frames);
            count = 1;
        } else if (WINDOW_DOMAIN[e.src].count) {
            if (e.n_frames != WINDOW_DOMAIN[e.src].count)
                return reject("a window at bank slot %u driven by source %u indexes %u "
                              "plates, and the row carries %u",
                              (unsigned)slot, (unsigned)e.src,
                              (unsigned)WINDOW_DOMAIN[e.src].count,
                              (unsigned)e.n_frames);
            count     = WINDOW_DOMAIN[e.src].count;
            value_min = WINDOW_DOMAIN[e.src].value_min;
        } else {
            count = e.n_frames;       /* a source with no fixed domain indexes by
                                       * its own packed range, and the row is the
                                       * authority on that many plates. */
        }
        if (e.order > 4)
            return reject("a bank row at slot %u ordered at rung %u: a window's depth "
                          "is a bucket under one of the dial's five live layers",
                          (unsigned)slot, (unsigned)e.order);
        if (!io->comps || !io->comp_cap)
            return reject("a bank row at slot %u, and this dial was built without a "
                          "complication table to project it into", (unsigned)slot);
        if (io->out_n_comps >= io->comp_cap)
            return reject("more complications than this dial's table holds (%lu)",
                          (unsigned long)io->comp_cap);
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, e.frame0, &sp);
        clk_comp_t *c = &io->comps[io->out_n_comps++];
        memset(c, 0, sizeof *c);
        c->tag         = (uint16_t)slot;
        c->kind        = e.kind;
        c->src         = e.src;
        c->x           = e.x;
        c->y           = e.y;
        c->w           = e.w;
        c->h           = e.h;
        c->count       = count;
        c->first       = e.order;             /* R-D: the z bucket */
        c->off         = sp.off;
        c->frame_bytes = (uint32_t)e.w * (uint32_t)e.h * 3u;
        c->value_min   = value_min;
        c->scale       = e.scale16;           /* 0 reads as 16 in comp maths */
        c->show        = (uint8_t)(e.flags & EF_HIDDEN ? 0 : 1);
        /* flags stays 0: the bank gate costs every row past the plate at
         * w*h*3, so a FORMAT 3 tile is always the runtime-composited kind. The
         * under-dial case is one a FORMAT 2 bake could produce and a FORMAT 3
         * catalogue cannot describe. */
    }

    /* ---- the pointers (D34): one AUX row, one clk_aux_t, driven by the row's own
     * drive. Every refusal below is a thing the row could get wrong in a way that
     * would draw a WRONG dial rather than no dial — a pointer that never moves, one
     * that divides by a zero domain, one that turns about a point off its own ink, one
     * asked to slide further than the glass is wide. Those are refused here, in the
     * dial's own words, exactly as the bank rows above are, so an author reads at
     * either end of the wire what the device would have made of the theme.
     *
     * The row is read as a clk_aux_t and nothing invents a number it did not carry:
     * `kind` is the motion, `src` the value, span/zero/vmin/vmax the drive, and the
     * sprite's geometry is the row's own box and pivot, which is why a pointer sits
     * wherever the theme drew it rather than at the panel centre. A slide's pivot is
     * left at the plate's own middle: it never rotates, and clk_sprite_t's pivot is
     * read by gfx_swept_box() for the dirty mark, so a slide that named a corner would
     * mark the wrong rows. */
    io->out_n_aux = 0;
    for (int slot = 0; slot < CLK_AUX_MAX; slot++) {
        if (at_aux[slot] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_aux[slot], &e);
        if (e.kind > MOTION_LAST)
            return reject("a pointer at slot %u moves as motion %u: the dial knows %u "
                          "ways for a row to move (rotate, retrograde, indexed, "
                          "slide-x, slide-y) and this names none of them",
                          (unsigned)slot, (unsigned)e.kind, (unsigned)(MOTION_LAST + 1));
        if (e.src >= CLK_SRC__MAX)
            return reject("a pointer at slot %u driven by source %u: this dial reads %u "
                          "values and none of them is that",
                          (unsigned)slot, (unsigned)e.src, (unsigned)CLK_SRC__MAX);
        if (e.src == CLK_SRC_NONE)
            return reject("a pointer at slot %u driven by 'none': a value that never "
                          "changes is plate art, and a row that never moves is a lie "
                          "about being a pointer", (unsigned)slot);
        if (e.src == CLK_SRC_TEXT || e.src == CLK_SRC_VALUE)
            return reject("a pointer at slot %u driven by source %u: this dial has no "
                          "such input, and D32's ruling refuses the flip rather than "
                          "ship a comp that would draw nothing",
                          (unsigned)slot, (unsigned)e.src);
        if (!e.vmax)
            return reject("a pointer at slot %u with a domain of 0: its span is shared "
                          "across the domain, so nothing can be computed from it",
                          (unsigned)slot);
        if (e.vmin > e.vmax)
            return reject("a pointer at slot %u whose value_min %u is above value_max "
                          "%u: the domain runs low to high", (unsigned)slot,
                          (unsigned)e.vmin, (unsigned)e.vmax);
        if (!e.span)
            return reject("a pointer at slot %u with a span of 0: it would sit at its "
                          "zero forever, which is a plate drawn as a row",
                          (unsigned)slot);
        if (e.n_frames != 1u)
            return reject("a pointer at slot %u naming %u frames: a pointer is one "
                          "plate that moves, and several plates indexed by a value is a "
                          "bank row", (unsigned)slot, (unsigned)e.n_frames);
        if (!MOTION_IS_SLIDE(e.kind)) {
            /* An angle stays an angle: inside one turn of home, and turning about a
             * point on its own ink. Both are the HAND rules, inherited deliberately —
             * a pointer that fails either would draw a mark swinging about nothing. */
            /* One turn is 3600 tenths, and that is also the widest offset these two
             * i16 columns carry with room to spare. A pointer needing more travel than
             * a turn is a disc, and the sentence says so at the door. */
            if (e.zero < -3600 || e.zero > 3600)
                return reject("a pointer at slot %u with a zero of %d: an offset past "
                              "one full turn is not a direction",
                              (unsigned)slot, (int)e.zero);
            if (e.span < -3600 || e.span > 3600)
                return reject("a pointer at slot %u with a span of %d: more than one "
                              "full turn is a disc, not a hand", (unsigned)slot,
                              (int)e.span);
            if (e.w && e.h && !(e.pivot_x >= 0 && (int32_t)e.pivot_x < (int32_t)e.w &&
                                e.pivot_y >= 0 && (int32_t)e.pivot_y < (int32_t)e.h))
                return reject("a pointer at slot %u turns about (%d,%d), outside its own "
                              "%ux%u plate", (unsigned)slot, (int)e.pivot_x,
                              (int)e.pivot_y, (unsigned)e.w, (unsigned)e.h);
        } else {
            /* A slide is in pixels, and the panel is 466 px across: a slide that long
             * leaves the glass at one end of its domain whatever the value is. */
            if (e.span > 466 || e.span < -466 || e.zero > 466 || e.zero < -466)
                return reject("a pointer at slot %u slides by %d from %d: a slide's span "
                              "and zero are pixels, and this one leaves the %u px panel",
                              (unsigned)slot, (int)e.span, (int)e.zero,
                              (unsigned)scr_w);
        }
        if (!io->aux || !io->aux_cap)
            return reject("a pointer at slot %u, and this dial was built without a "
                          "pointer table to project it into", (unsigned)slot);
        if (io->out_n_aux >= io->aux_cap)
            return reject("more pointers than this dial's table holds (%lu)",
                          (unsigned long)io->aux_cap);
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, e.frame0, &sp);
        clk_aux_t *a = &io->aux[io->out_n_aux++];
        memset(a, 0, sizeof *a);
        fill_sprite(&a->spr, &e, &sp, NULL);
        a->tag       = (uint16_t)slot;
        a->src       = (uint8_t)e.src;
        /* clk_aux_t.kind and the row's `kind` share the first three ids by D34's
         * design; the two slides are the dial's own appends to that enum and are read
         * by clock_view.c's slide path. */
        a->kind      = (uint8_t)e.kind;
        a->host      = 0;               /* every pointer places itself: no host arbour */
        a->span      = e.span;
        a->zero      = e.zero;
        a->value_min = (int16_t)e.vmin;
        a->value_max = e.vmax;
        /* No row column names JUMP, WRAP or 24H for a pointer, so the projection
         * carries none of them rather than borrow a byte that means something else:
         * scale16 is the comp maths' x16 fraction, and reading it as a snap flag
         * would be a meaning nobody wrote. A pointer therefore interpolates, and the
         * flag rides the next ABI append when a theme actually asks for the stutter.
         * The twin is the one meaning a row can carry: AUX_F_SHADOW lands on the
         * runtime flag every clock-side pairing rule already reads. */
        a->flags     = 0;
        if (e.flags & AUX_F_SHADOW) {
            /* The twin takes its pose from the row before it and draws under it, so
             * the row before it must exist and must be a hand. A twin with no hand
             * is the same lie §25.2 refuses for a hand shadow with no hand: it can
             * only ever draw a smudge where a pointer should be. */
            orb_bundle_elem_t pe;
            if (slot == 0 || at_aux[slot - 1] < 0)
                return reject("a pointer shadow at slot %u with no hand before it: the twin takes its pose from the row under it, and the row before it is empty",
                              (unsigned)slot);
            orb_bundle_elem_at(sec, (uint32_t)at_aux[slot - 1], &pe);
            if (pe.flags & AUX_F_SHADOW)
                return reject("a pointer shadow at slot %u under another shadow: a twin darkens a pointer, not a shadow",
                              (unsigned)slot);
            a->flags = CLK_FLAG_AUX_SHADOW;
        }
        a->spr.blend = 0;
        a->spr.show  = (uint8_t)(e.flags & EF_HIDDEN ? 0 : 1);
    }

    /* Extents, layer by layer, in FORMAT 2's own words: the same lie at either door
     * gets the same sentence. A layer the dial will not look at is not checked, for
     * the reason orb_clock3_parse gives. The plate and the keyframe are checked
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
    for (uint32_t f = 0; f < t->tourbillon_n; f++) {
        const clk_sprite_t *b = &t->tourbillon[f];
        if (b->show && b->w && b->h &&
            (uint64_t)b->color_off + SPRITE_BYTES_T(b) > art)
            return "carriage pixels lie outside the declared payload";
    }
    for (uint32_t k = 0; k < io->out_n_comps; k++) {
        const clk_comp_t *c = &io->comps[k];
        if (c->w && c->h &&
            (uint64_t)c->off + (uint64_t)c->count * c->frame_bytes > art)
            return "complication pixels lie outside the declared payload";
    }
    if (t->bg_mode == 1 &&
        (t->bg_key_off % 2u ||
         (uint64_t)t->bg_key_off + PLATE_BYTES_T(scr_w, scr_h) > (uint64_t)art))
        return "the keyframe pixels lie outside the declared payload";

    /* ---- the gif block, derived (§25.6), and the plate bank (§31) ----
     * One zero rect: the compiled path the shipped pair takes is the keyframe copy,
     * which discards the rect list, and a FORMAT 3 clock section carries no delta
     * bank to index. Every window boundary is therefore 0. The plate-frame table
     * is the bgAnim consumer's own row list: a still plate keeps the one keyframe
     * row it has always been handed, and an animated one gets one offset per
     * extra plane, in bank order — frame 0 needs no row because the consumer
     * starts on the plate. R6 retired the per-frame delay, so the delay table
     * carries the OS beat — the same uniform figure a PNG frame pack's own bake
     * printed, which is the 67 ms the shipped av-kit pair carries. */
    io->rects[0].x = io->rects[0].y = 0;
    io->rects[0].w = io->rects[0].h = 0;
    io->rects[0].off = 0;
    for (uint32_t i = 0; i <= (uint32_t)t->gif_frames; i++) io->rect_first[i] = 0;
    const uint32_t beat = (uint32_t)(t->tick_rate ? t->tick_rate : 1);
    uint32_t ms = (1000u + beat / 2u) / beat;
    if (!ms) ms = 1;
    for (uint32_t i = 0; i < (uint32_t)t->gif_frames; i++)
        io->gif_delays[i] = (uint16_t)ms;
    if (plate.n_frames > 1u) {
        for (uint32_t f = 1; f < plate.n_frames; f++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, plate.frame0 + f, &sp);
            io->bg_frames[f - 1] = sp.off;
        }
    } else {
        io->bg_frames[0] = 0;
    }
    return NULL;
}
