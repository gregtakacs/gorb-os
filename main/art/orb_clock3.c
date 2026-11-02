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
#define CLASS_N 11
static const char *const CLASS_NAMES[CLASS_N] = {
    "text", "sprite", "hand", "plate", "ring", "bank", "layer", "status", "aux",
    "preview", "sound"
};

#define CLS_SPRITE    1
#define CLS_HAND      2
#define CLS_PLATE     3
#define CLS_BANK      5
#define CLS_AUX       8   /* D34: theme_schema.CLASS_AUX, appended at 8 */
#define CLS_PREVIEW   9   /* the picker's plane, appended at 9: this dial does not draw it */
#define CLS_SOUND    10   /* D39: a waveform, appended at 10: the clock plays it, never draws it */
#define EF_HIDDEN  8            /* theme_schema.EF_HIDDEN */
#define EF_LOOP    2            /* theme_schema.EF_LOOP: on a SOUND row, the tick loop */
#define EF_DELTA_WRAP 64        /* §37: the row's LAST sprite, one past its own frame
                                 * window, is the rect that returns the finished canvas
                                 * to the keyframe — the delta the device applies when a
                                 * looping delta plate wraps to frame 0. Only a looping
                                 * v4 delta plate may carry it; the refusal lives with
                                 * the plate gate below, and the baker's rowDelta sets
                                 * the bit in the same breath as shipping the row. */
#define EF_INSTANCE 32          /* C4: the row's one plane is its whole frame
                                 * window, and the comp maths draws `n_frames`
                                 * slots of it. The reading rule is one condition
                                 * (§4.2); the bit means nothing below a window
                                 * and nothing outside the delta dialect, so
                                 * both refusals live here, not in the schema's
                                 * u8 gate alone. */
#define PARENT_NONE 0xFFFFu     /* C4's element-row parent column, none */

/* C4's row table is bounded by the clock's own slot vocabulary — two plates, the
 * twenty-four moving rows (D42), sixteen complications, a preview, the sound slots —
 * and a table that outgrows it died in the walk above for naming a slot twice or a
 * class the dial has no field for. The parent pass reserves one slot per row it
 * accepted, so this bound is the walk's, not a new cap: a row count past it cannot
 * arrive here at all, and the assert below is the check that says so. */
#define CLOCK_ROWS_MAX (2 + CLK_ROW_MAX + CLK_COMP_MAX + 1 + CLK_SND_SLOTS)
#define ELEM_ORDER_N   64       /* theme_schema.ELEM_ORDER_N: the depth rungs */
#define CLK_SWEEP_TIER 15       /* theme_schema.SWEEP_TIER: a sweep that names no rate */
#define CLK_SWEEP_FPS_MAX 30    /* theme_schema.SWEEP_FPS_MAX: the panel's ceiling */

/* §31 rule 5: the plate row's frames, and every row's parent claim, are the two
 * vocabularies a v3 stamp has never carried a meaning for. The gate: */
#define AUX_F_SHADOW 0x10       /* aux row: the shadow TWIN of the row before it.
                                 * FORMAT 3's element flag column is a u8, so the
                                 * runtime's CLK_FLAG_AUX_SHADOW (1 << 8) cannot ride
                                 * it; bit 4 is the slot the element dialect leaves
                                 * free for a pointer (alpha/loop/os-adv mean nothing
                                 * to a single-frame row). orbtheme-baker.js writes it
                                 * as AUX_F_SHADOW3; the projection below maps it onto
                                 * the runtime flag, so the stage's pairing rules
                                 * read one vocabulary however the theme arrived. */

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
#define W_BG_FRAMES       9u
#define W_BG_FPS         10u
#define W_BG_LOOP        11u
#define W_BG_EVERY       12u
#define W_PLATE_OFF      13u
#define W_BG_MODE        21u
#define W_BG_KEY_OFF     22u
#define W_GIF_FRAMES     23u
/* The words D42 retired (theme_schema.CLOCK_RETIRED_SCALARS): plate_follow, the shadow
 * trio, second_sweep, second_railway, text_over_hands, tick_rate, the order table,
 * matte_mask and the tourbillon pair. A v6 section carries each as 0 — the rows say
 * what they said — and a non-zero one is a bake from before the ruling, refused. */
static const uint8_t RETIRED_W[] = { 1, 2, 3, 4, 5, 6, 7, 8, 14, 15, 16, 17, 18, 19,
                                     20, 24, 25 };

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

/* D41 §2.2, row-local: what a row's cadence columns may say, given the source it
 * reads. The same refusals as theme_schema.check_element, in its words where they can
 * be shared. */
static const char *cadence_check(const orb_bundle_elem_t *e)
{
    if (!e->cadence && !e->cad_arg && !e->ease) return NULL;
    if (e->cls != CLS_AUX && e->cls != CLS_BANK)
        return reject("element %u is class %u and names a cadence: when a row moves is "
                      "a driven row's question (a moving row, a bank)",
                      (unsigned)e->n, (unsigned)e->cls);
    if (e->cadence >= CLK_CAD__MAX)
        return reject("element %u names cadence %u: this dial knows %u",
                      (unsigned)e->n, (unsigned)e->cadence, (unsigned)CLK_CAD__MAX);
    if (e->ease) {
        /* D41 §2.1: a settle eases a jump or a step slower than the second; a flyback
         * eases a flyback row's return. Nothing else has an instant worth easing, and
         * a seconds step would buy a burst of frames every second. */
        const bool fly   = e->cls == CLS_AUX && e->end == CLK_END_FLYBACK;
        const bool steps = (e->cadence == CLK_CAD_JUMP || e->cadence == CLK_CAD_STEP) &&
                           e->src != CLK_SRC_SECOND && e->cls != CLS_BANK;
        if (e->ease > 1000u)
            return reject("element %u eases over %u ms: an ease runs at most a second",
                          (unsigned)e->n, (unsigned)e->ease);
        if (!fly && !steps)
            return reject("element %u eases over %u ms with nothing to ease: a settle "
                          "needs a jump or a step slower than the second, a flyback a "
                          "flyback row", (unsigned)e->n, (unsigned)e->ease);
    }
    if (e->cadence == CLK_CAD_STEP && !e->cad_arg)
        return reject("element %u steps 0 times a unit: name how many steps per unit",
                      (unsigned)e->n);
    if ((e->cadence == CLK_CAD_NONE || e->cadence == CLK_CAD_JUMP ||
         e->cadence == CLK_CAD_ROLL) && e->cad_arg)
        return reject("element %u carries a cadence argument of %u on a cadence that "
                      "takes none", (unsigned)e->n, (unsigned)e->cad_arg);
    if (e->cadence == CLK_CAD_SWEEP && e->cad_arg > CLK_SWEEP_FPS_MAX)
        return reject("element %u sweeps at %u frames a second: the panel draws %u at "
                      "most", (unsigned)e->n, (unsigned)e->cad_arg,
                      (unsigned)CLK_SWEEP_FPS_MAX);
    if (e->cadence == CLK_CAD_STOP_TO_GO && e->src != CLK_SRC_SECOND)
        return reject("element %u is stop-to-go and reads source %u: stop-to-go is a "
                      "seconds hand's rhythm", (unsigned)e->n, (unsigned)e->src);
    if (e->cadence == CLK_CAD_DRAG && e->src != CLK_SRC_DATE && e->src != CLK_SRC_WEEKDAY &&
        e->src != CLK_SRC_MONTH && e->src != CLK_SRC_DAY_OF_YEAR)
        return reject("element %u drags and reads source %u: drag holds a calendar row",
                      (unsigned)e->n, (unsigned)e->src);
    if (e->cls == CLS_BANK && e->cadence != CLK_CAD_NONE && e->cadence != CLK_CAD_JUMP)
        return reject("element %u is a complication's plates and names cadence %u: "
                      "whole plates jump", (unsigned)e->n, (unsigned)e->cadence);
    return NULL;
}

#define TURN_10  3600
#define PANEL_PX 466

/* D42 §8, row-local: one moving row's columns. theme_schema.check_element's AUX
 * block, rule for rule. A still row (source none) names nothing about motion; a
 * moving row names a cadence, a domain wide enough to divide and somewhere to go,
 * within one turn and on the panel; a wrap only where P1 looks like P0. */
static const char *row_check(const orb_bundle_elem_t *e, uint16_t slot)
{
    if (e->kind >= CLK_SHAPE__MAX)
        return reject("row %u is shape %u: the dial knows %u (arm, ring, text)",
                      (unsigned)slot, (unsigned)e->kind, (unsigned)CLK_SHAPE__MAX);
    if (e->kind == CLK_SHAPE_TEXT)
        return reject("row %u is a text row: the string table it reads lands with "
                      "D42's E4", (unsigned)slot);
    if (e->str)
        return reject("row %u points into the string table at %u: only a text row "
                      "reads it", (unsigned)slot, (unsigned)e->str);
    if (e->src >= CLK_SRC__MAX || e->src == CLK_SRC_TEXT || e->src == CLK_SRC_VALUE)
        return reject("row %u driven by source %u: this dial has no such input, and a "
                      "row moving by it would never move", (unsigned)slot, (unsigned)e->src);
    if (e->degrees)
        return reject("row %u carries degrees %u: a row's angle at rest is its zero",
                      (unsigned)slot, (unsigned)e->degrees);
    if (e->src == CLK_SRC_NONE) {
        if (e->cadence || e->cad_arg || e->ease || e->span || e->zero || e->vmin ||
            e->vmax || e->dx || e->dy || e->end || e->digit)
            return reject("row %u is still (source none) and names a motion: a row that "
                          "never moves names none", (unsigned)slot);
    } else {
        if (!e->cadence)
            return reject("row %u moves by source %u and names no cadence: a moving row "
                          "says when it moves", (unsigned)slot, (unsigned)e->src);
        if (e->end >= CLK_END__MAX)
            return reject("row %u names end rule %u: the dial knows %u",
                          (unsigned)slot, (unsigned)e->end, (unsigned)CLK_END__MAX);
        if (e->digit >= CLK_DIGIT__MAX)
            return reject("row %u reads digit %u: the dial knows %u",
                          (unsigned)slot, (unsigned)e->digit, (unsigned)CLK_DIGIT__MAX);
        if (e->vmax <= e->vmin)
            return reject("row %u with domain %u..%u: a domain has to hold at least one "
                          "unit to divide", (unsigned)slot, (unsigned)e->vmin,
                          (unsigned)e->vmax);
        if (!e->span && !e->dx && !e->dy)
            return reject("row %u neither turns nor travels: a moving row that goes "
                          "nowhere is a still row", (unsigned)slot);
        if (e->zero < -TURN_10 || e->zero > TURN_10 || e->span < -TURN_10 || e->span > TURN_10)
            return reject("row %u turns %d from %d: a pose stays within one turn",
                          (unsigned)slot, (int)e->span, (int)e->zero);
        if (e->dx < -PANEL_PX || e->dx > PANEL_PX || e->dy < -PANEL_PX || e->dy > PANEL_PX)
            return reject("row %u travels (%d,%d): it would leave the %u px panel",
                          (unsigned)slot, (int)e->dx, (int)e->dy, (unsigned)PANEL_PX);
        /* D42 ruling 5: a wrap starts the next unit at P0 with no visible jump, which
         * only a whole turn with no travel can do. Refused here as well as at design
         * time, so a theme made by any other means meets the same door. */
        if (e->end == CLK_END_WRAP &&
            ((e->span != TURN_10 && e->span != -TURN_10) || e->dx || e->dy))
            return reject("row %u wraps after turning %d and travelling (%d,%d): a wrap "
                          "needs a whole turn and no travel", (unsigned)slot,
                          (int)e->span, (int)e->dx, (int)e->dy);
    }
    if (e->w && e->h && !(e->pivot_x >= 0 && (int32_t)e->pivot_x < (int32_t)e->w &&
                          e->pivot_y >= 0 && (int32_t)e->pivot_y < (int32_t)e->h))
        return reject("row %u turns about (%d,%d), outside its own %ux%u plate",
                      (unsigned)slot, (int)e->pivot_x, (int)e->pivot_y,
                      (unsigned)e->w, (unsigned)e->h);
    if (e->n_frames > CLK_TOURB_MAX)
        return reject("row %u animates %u frames: the dial holds %u",
                      (unsigned)slot, (unsigned)e->n_frames, (unsigned)CLK_TOURB_MAX);
    if (e->n_frames > 1u && !e->delay)
        return reject("row %u animates %u frames and names no frame time",
                      (unsigned)slot, (unsigned)e->n_frames);
    if ((e->flags & AUX_F_SHADOW) && (e->n_frames != 1u || e->src || e->cadence))
        return reject("row %u is a shadow twin with a motion of its own: a twin takes "
                      "its hand's whole pose", (unsigned)slot);
    return cadence_check(e);
}

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
    io->out_n_sounds = 0;
    io->out_pattern  = CLK_PAT_NONE;
    io->out_art_off  = sec->art_off;
    io->out_art_size = art;
    if (!ne || !ns) return "a clock section with no elements or no planes draws nothing";

    /* The panel pair rides the index record (§25.2), and the dial it feeds is
     * compiled for one panel. FORMAT 2's header gate refuses the same lie. */
    if (scr_w != CLK_SCR_W || scr_h != CLK_SCR_H)
        return "a format-3 clock section made for a different panel";

    /* ---- D42: the clock's own dialect gate, before any row ----
     * The clock section stamps its geometry version in the index record's own
     * word, and the clock reads v6 and nothing older (no legacy is kept): element
     * rows of exactly 33 words and the six-word delta catalogue. A stride that
     * disagrees with the stamp is a file whose meaning is unknown, and leniency
     * here is the silent mispaint §4.1 refuses. */
    const uint8_t cabi = sec->abi_max;
    if (sec->abi_min > cabi || cabi != (uint8_t)ORB_BUNDLE3_ABI6)
        return reject("a clock section stamped geometry v%u..v%u; this reader's clock "
                      "dialect is v%u and nothing older — re-bake it",
                      (unsigned)sec->abi_min, (unsigned)cabi,
                      (unsigned)ORB_BUNDLE3_ABI6);
    const bool delta = true;     /* every v6 rule C4 introduced holds */
    const uint32_t ew = orb_bundle_elem_stride(sec);
    if (ew != ORB_BUNDLE_ELEM_WORDS)
        return reject("clock rows are %lu words under geometry v%u; that stamp writes "
                      "exactly %u", (unsigned long)ew, (unsigned)cabi,
                      (unsigned)ORB_BUNDLE_ELEM_WORDS);
    if (ne > CLOCK_ROWS_MAX)   /* the walk's own bound, asserted before its slots */
        return reject("%lu clock rows: the dial's slot vocabulary seats %u at most",
                      (unsigned long)ne, (unsigned)CLOCK_ROWS_MAX);
    const uint32_t sprb = orb_bundle_spr_stride(sec);
    if (sprb != ORB_BUNDLE_SPR_BYTES2)
        return reject("clock catalogue rows are %lu bytes under geometry v%u; that "
                      "stamp writes %lu", (unsigned long)sprb, (unsigned)cabi,
                      (unsigned long)ORB_BUNDLE_SPR_BYTES2);

    /* ---- the table, indexed by (class, slot): the identity §25.2 ruled ---- */
    int16_t at_plate[2] = { -1, -1 };
    int16_t at_aux[CLK_ROW_MAX];
    for (int i = 0; i < CLK_ROW_MAX; i++) at_aux[i] = -1;   /* the stack zeroes no -1 */
    int16_t at_bank[CLK_COMP_MAX] = { -1,-1,-1,-1,-1,-1,-1,-1,
                                      -1,-1,-1,-1,-1,-1,-1,-1 };
    int16_t at_sound[CLK_SND_SLOTS];
    for (int i = 0; i < CLK_SND_SLOTS; i++) at_sound[i] = -1;
    /* C4's parent bookkeeping: one slot per row the walk accepts, so the bound
     * is the walk's own (the gate refused any table that could outgrow it). */
    uint16_t row_tag[CLOCK_ROWS_MAX];
    uint16_t row_par[CLOCK_ROWS_MAX];        /* tag on entry, row index + 1 out */
    uint32_t nrows = 0;
    /* The preview row's own window, held for the catalogue walk below: a class 9
     * plane is opaque RGB565 by its class price (orb_asset_slot_bytes), so the walk
     * must not bill it the alpha stride. The sentinel is unreachable as a row index:
     * a bank's rows number under n_spr, which fits a u16 with 0xFFFF to spare. */
    uint16_t prev0 = 0xFFFF, prevn = 0;
    for (uint32_t i = 0; i < ne; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        int16_t *cell = NULL;
        const char *nm = e.cls < CLASS_N ? CLASS_NAMES[e.cls] : "?";
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
            case CLS_AUX:
                /* D42: every row that moves — and every still layer the hands pass
                 * under or over — is an AUX row addressed by its own ordinal. The
                 * clock does not know what a row indicates, only how it moves. */
                if (e.slot >= CLK_ROW_MAX)
                    return reject("a row at slot %u: the clock declares %u moving rows "
                                  "and the manifest of any app may not invent one",
                                  (unsigned)e.slot, (unsigned)CLK_ROW_MAX);
                cell = &at_aux[e.slot];
                break;
            case CLS_PREVIEW:
                /* The picker's plane rides the theme's table and this dial does not
                 * draw it: the owner chooses a theme in the OS's app-scoped picker,
                 * and the shell reads the extent by class through orb_elems.c. The
                 * row claims no clock slot, so no `cell` is set and the walk below
                 * (row_tag, nrows) still counts it. A second preview row is a
                 * refusal by checkTable's class:slot uniqueness, not the clock's. */
                prev0 = e.frame0;
                prevn = e.n_frames;
                break;
            case CLS_SOUND:
                /* D39: one row per event slot, one plane of PCM per row. Its facts
                 * are checked with the scalars below, where a loop's beat can be
                 * held against the hand it ticks for. */
                if (e.slot >= CLK_SND_SLOTS)
                    return reject("a sound at slot %u: the clock declares %u sound slots",
                                  (unsigned)e.slot, (unsigned)CLK_SND_SLOTS);
                if (e.n_frames != 1u)
                    return reject("a sound at slot %u names %u planes: a sound is one "
                                  "waveform", (unsigned)e.slot, (unsigned)e.n_frames);
                cell = &at_sound[e.slot];
                break;
            case CLS_HAND:
            case CLS_SPRITE:
                /* D42: the five hand slots and their shadow sprites are gone. Every
                 * hand, shadow and carriage is an AUX row, and a table that still
                 * names the old classes is a bake from before the ruling. */
                return reject("element %u is class %s: every hand, shadow and carriage "
                              "is a moving (aux) row since D42 — re-bake it",
                              (unsigned)e.n, nm);
            default:
                return reject("element %u is class %s: the compiled clock has no field "
                              "for it, and §25.3 rules it into the complication "
                              "mapping", (unsigned)e.n, nm);
        }
        if (cell) {
            if (*cell >= 0)
                return reject("two rows for %s slot %u: one slot draws one layer",
                              nm, (unsigned)e.slot);
            *cell = (int16_t)i;
        }
        {
            const char *why = e.cls == CLS_AUX ? row_check(&e, e.slot) : cadence_check(&e);
            if (why) return why;
        }
        if (e.cls != CLS_AUX && (e.dx || e.dy || e.end || e.digit || e.str))
            return reject("element %u is class %s and names a pose: a row's poses are a "
                          "moving row's question", (unsigned)e.n, nm);
        if (e.order >= ELEM_ORDER_N)
            return reject("element %u at depth %u: the dial has %u rungs",
                          (unsigned)e.n, (unsigned)e.order, (unsigned)ELEM_ORDER_N);

        /* C4's per-row rules, run once the row has a slot. */
        row_tag[nrows] = e.n;
        row_par[nrows] = 0u;
        if ((e.flags & EF_INSTANCE) &&
            (e.cls != CLS_BANK || e.kind != CLK_KIND_WINDOW ||
             e.src >= CLK_SRC__MAX ||
             WINDOW_DOMAIN[e.src].count == 0u ||
             e.n_frames != WINDOW_DOMAIN[e.src].count))
            return reject("element %u asks to instance %u plates over a domain of "
                          "%u: one plane standing for a window needs a fixed "
                          "domain and its full frame count",
                          (unsigned)e.n, (unsigned)e.n_frames,
                          (unsigned)(e.src < CLK_SRC__MAX ? WINDOW_DOMAIN[e.src].count : 0u));
        if (e.parent != PARENT_NONE) {
            if (e.parent == 0u)
                return reject("element %u names parent 0: zero is the plate's own "
                              "tag, and a parent of none is 0xFFFF — refused, not "
                              "guessed", (unsigned)e.n);
            if (e.cls != CLS_AUX && e.cls != CLS_BANK)
                return reject("element %u is class %s and names a parent: moving rows "
                              "and complications hang on other rows, plates are hung "
                              "from", (unsigned)e.n, nm);
            if (e.flags & AUX_F_SHADOW)
                return reject("element %u is a shadow twin and names a parent: a "
                              "twin takes its hand's pose, not a row's",
                              (unsigned)e.n);
            row_par[nrows] = e.parent;      /* resolved by tag after the walk */
        }
        if (e.delay && e.n_frames < 2u)
            return reject("element %u carries a delay of %u ms and one frame: a "
                          "tempo on a row that never advances is a bake that lost "
                          "its animation", (unsigned)e.n, (unsigned)e.delay);
        nrows++;
    }

    /* ---- C4: the parent pass. Tags resolve, chains stop at two links, and a
     * cycle is refused by the walk that would otherwise spin. O(n²) over a table
     * the slot vocabulary already caps at CLOCK_ROWS_MAX rows: a dial this small
     * is the reason the cap is honest, not a thing to optimize. Validation runs
     * over the whole table BEFORE any resolution rewrites a slot, so a resolved
     * small index can never masquerade as a tag mid-pass. ---- */
    for (uint32_t k = 0; k < ne; k++) {
        if (row_par[k] == 0u) continue;
        uint32_t cur = k;
        for (uint32_t hops = 1;; hops++) {
            const uint16_t tag = row_par[cur];
            int found = -1;
            for (uint32_t m = 0; m < ne; m++)
                if (row_tag[m] == tag) { found = (int)m; break; }
            if (found < 0)
                return reject("element %u names parent %u: no row in this table carries "
                              "that tag", (unsigned)row_tag[cur], (unsigned)tag);
            if ((uint32_t)found == k)
                return reject("element %u's parent chain is a cycle: the walk came home "
                              "after %lu link%s", (unsigned)row_tag[k],
                              (unsigned long)hops, hops == 1u ? "" : "s");
            if (hops > 2u)
                return reject("element %u hangs three deep: this compositor carries two "
                              "links — a child on a rotator, a rotator on a plate",
                              (unsigned)row_tag[k]);
            cur = (uint32_t)found;
            if (row_par[cur] == 0u) break;
        }
    }
    for (uint32_t k = 0; k < ne; k++) {
        if (row_par[k] == 0u) continue;
        for (uint32_t m = 0; m < ne; m++)
            if (row_tag[m] == row_par[k]) { row_par[k] = (uint16_t)(m + 1u); break; }
    }

    /* The scalars, verbatim (D31): the ones v6 still carries, and the retired ones
     * held to their zero. */
    for (size_t k = 0; k < sizeof RETIRED_W; k++)
        if (U(sec, RETIRED_W[k]))
            return reject("clock scalar word %u is %lu: D42 retired it and a v6 section "
                          "carries it as 0 — the rows say what it said",
                          (unsigned)RETIRED_W[k], (unsigned long)U(sec, RETIRED_W[k]));
    t->bg              = U(sec, W_BG);
    t->bg_frames       = (uint16_t)U(sec, W_BG_FRAMES);
    t->bg_fps          = (int16_t)S(sec, W_BG_FPS);
    t->bg_loop         = (uint8_t)U(sec, W_BG_LOOP);
    t->bg_every        = (uint16_t)U(sec, W_BG_EVERY);
    t->plate_off       = U(sec, W_PLATE_OFF);
    t->bg_mode    = (uint8_t)U(sec, W_BG_MODE);
    t->bg_key_off = U(sec, W_BG_KEY_OFF);
    t->gif_frames = (uint16_t)U(sec, W_GIF_FRAMES);
    t->car_row    = 0xFFu;

    /* ---- D42: the tempo, read from the seconds rows (theme_schema.clock_tempo).
     * The scheduler keeps one beat: a seconds row that sweeps or runs stop-to-go asks
     * for the sweep (at its own frame rate when the sweep names one), a stepping one
     * for its N a second, and every stepping seconds row must agree on N. Stop-to-go
     * rests until a jumping minute releases it. The figures land in the theme once
     * the plate's and the carriage's own rates are known (below). ---- */
    bool tempo_sweep = false;
    uint32_t tempo_rate = 0, tempo_sweep_fps = 0;
    {
        bool minute_jumps = false, sweep = false;
        uint32_t rate = 0, sweep_fps = 0;
        for (int slot = 0; slot < CLK_ROW_MAX; slot++) {
            if (at_aux[slot] < 0) continue;
            orb_bundle_elem_t e;
            orb_bundle_elem_at(sec, (uint32_t)at_aux[slot], &e);
            if (e.src == CLK_SRC_MINUTE && e.digit == CLK_DIGIT_WHOLE &&
                e.cadence == CLK_CAD_JUMP)
                minute_jumps = true;
            if (e.src != CLK_SRC_SECOND || e.digit != CLK_DIGIT_WHOLE ||
                (e.flags & EF_HIDDEN))
                continue;
            if (e.cadence == CLK_CAD_SWEEP || e.cadence == CLK_CAD_STOP_TO_GO) sweep = true;
            if (e.cadence == CLK_CAD_SWEEP && e.cad_arg > sweep_fps) sweep_fps = e.cad_arg;
            if (e.cadence == CLK_CAD_STEP) {
                if (rate && rate != e.cad_arg)
                    return reject("seconds rows step at %lu and %u a second: a dial keeps "
                                  "one beat", (unsigned long)rate, (unsigned)e.cad_arg);
                rate = e.cad_arg;
            }
        }
        for (int slot = 0; slot < CLK_ROW_MAX; slot++) {
            if (at_aux[slot] < 0) continue;
            orb_bundle_elem_t e;
            orb_bundle_elem_at(sec, (uint32_t)at_aux[slot], &e);
            if (e.cadence == CLK_CAD_STOP_TO_GO && !minute_jumps)
                return reject("a stop-to-go row at slot %u rests until the minute releases "
                              "it, and no minute row jumps", (unsigned)slot);
        }
        tempo_sweep = sweep;
        tempo_rate = rate;
        tempo_sweep_fps = sweep_fps;
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
     * advanced by the OS. The dial paints the background edge to edge and every
     * moving row's restore reads it, so a table without one, or with one that is
     * not the whole panel, is not a clock this file can lie about. */
    if (at_plate[0] < 0)
        return "no plate row: the dial restores every moving row from the plate, and a "
               "clock with no plate has nothing to restore from";
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
     * below, where the catalogue has been walked and the row bounds proved.
     * C4 adds the third reading: under the delta stamp a gif bank rides its row
     * (frame 0 whole, the rest rects), which is exactly the vocabulary §25.3
     * called un-carriable — it now has a carrier, so the refusal retires for
     * that shape and that shape alone. A keyframe bank keeps its own rule
     * untouched: av-kit's still plate and 15-plane gif boot the same pixels
     * under either stamp. */
    const uint8_t plate_anim = plate.n_frames > 1u || t->bg_frames != 0;
    const uint8_t plate_delta =
        delta && t->bg_mode == 1 && plate.n_frames > 1u && t->bg_frames == 0;
    /* §37 first refusal, before either animation branch: a v3 table or a
     * keyframe-shaped plate claiming the closing bit is the lie itself, and it
     * deserves the sentence that names it rather than a shape mismatch. */
    if ((plate.flags & EF_DELTA_WRAP) && !plate_delta)
        return "the closing delta means the looping v4 delta plate and nothing else: "
               "a row without that shape cannot claim its rect";
    if (plate_delta) {
        if (t->gif_frames != (uint16_t)plate.n_frames)
            return reject("a delta plate row names %u planes while the gif table "
                          "counts %u frames: the row is the loop",
                          (unsigned)plate.n_frames, (unsigned)t->gif_frames);
        if (!t->bg_loop && t->bg_fps < 1 && !plate.delay)
            return reject("a one-shot delta bank at bg_fps %d and no row delay: an "
                          "animation with no tempo is a frame that never arrives",
                          (int)t->bg_fps);
    } else if (plate_anim) {
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
    /* §37: the closing delta. A looping delta plate may carry ONE sprite past its own
     * frame window: the rect that returns the finished canvas to the keyframe, which
     * the device applies when the loop wraps to frame 0. Without it the wrap is a
     * keyframe blit no delta table can express, and the visible price is a stalled
     * tick at every wrap and a stale base under frame 1 — once per second at the
     * sweep tier's 15-frame bank. The row claims the shape with EF_DELTA_WRAP; the
     * baker's rowDelta sets the bit in the same breath as shipping the row, and this
     * is the gate that refuses the lie. A v3 build refuses the bit as it refuses the
     * delta dialect, and any other row refuses it in the walk below: an unbacked
     * bit must fail the load, never paint nothing quietly. */
    const uint8_t plate_wrap = plate_delta && (plate.flags & EF_DELTA_WRAP);
    if (plate_delta && t->gif_frames > (uint16_t)ORB_CLOCK3_N_RECTS)
        return reject("a delta loop of %u frames: %u materialized rects, and the dial's "
                      "invalid-area table holds %d — a frame forced to drop its rect "
                      "paints stale texels forever, so this bank belongs to a wider dial",
                      (unsigned)t->gif_frames, (unsigned)t->gif_frames,
                      ORB_CLOCK3_N_RECTS);
    if (plate_wrap) {
        if ((uint64_t)plate.frame0 + plate.n_frames >= ns)
            return reject("the plate row claims a closing delta and owns no sprite after "
                          "its own frame window to be it: row %u of a %u-row catalogue",
                          (unsigned)(plate.frame0 + plate.n_frames), (unsigned)ns);
        orb_bundle_spr_t spw;
        orb_bundle_spr_at(sec, plate.frame0 + plate.n_frames, &spw);
        if (spw.size != (uint32_t)spw.w * spw.h * 2u)
            return reject("the delta pays colour-only: the one row past the plate "
                          "must BE the closing rect, not a %u-by-%u row of %lu B",
                          (unsigned)spw.w, (unsigned)spw.h,
                          (unsigned long)spw.size);
        if (!spw.w || !spw.h || spw.w > plate.w || spw.h > plate.h ||
            spw.diffx < 0 || spw.diffy < 0 ||
            (int32_t)spw.w + spw.diffx > (int32_t)plate.w ||
            (int32_t)spw.h + spw.diffy > (int32_t)plate.h)
            return reject("the closing delta is a %lux%lu rect at (%d,%d) of a %ux%u "
                          "plate: it must sit inside the plate it closes back to",
                          (unsigned long)spw.w, (unsigned long)spw.h,
                          (int)spw.diffx, (int)spw.diffy,
                          (unsigned)plate.w, (unsigned)plate.h);
    }

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
        const int on_plate = i >= plate.frame0 &&
                             (i < plate.frame0 + plate.n_frames ||
                              (plate_wrap && i == plate.frame0 + plate.n_frames));
        /* A preview plane rides the same bank and pays the same two bytes: its
         * class price (orb_asset_slot_bytes, and tools/theme_schema.py's copy)
         * is RGB565 alone — the third byte would be a plane of 255s no picker
         * consults. The row still has to sit where the walk says and be the
         * width IT claims, or the lie is a torn bank wearing a nameplate. */
        const int on_preview = prev0 != 0xFFFF &&
                               (uint32_t)i - (uint32_t)prev0 < (uint32_t)prevn;
        /* A sound's plane is its PCM, two bytes a sample with no alpha stride: the
         * class price orb_asset_slot_bytes gives SOUND, w samples by h 1. */
        int on_sound = 0;
        for (int k = 0; k < CLK_SND_SLOTS && !on_sound; k++) {
            if (at_sound[k] < 0) continue;
            orb_bundle_elem_t se;
            orb_bundle_elem_at(sec, (uint32_t)at_sound[k], &se);
            on_sound = se.frame0 == i;
        }
        const uint64_t want = PLATE_BYTES_T(sp.w, sp.h)
                            + ((on_plate || on_preview || on_sound) ? 0 : (uint64_t)sp.w * sp.h);
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
        /* §37: the closing bit means this section's looping delta plate and nowhere
         * else — checked before the window, so a lie about it never widens one. */
        if ((e.flags & EF_DELTA_WRAP) &&
            !(delta && i == (uint32_t)at_plate[0] && plate_delta))
            return reject("element %u carries the closing-delta bit: the loop it closes "
                          "is the plate's background, and this row is not the section's "
                          "looping v4 delta plate", (unsigned)e.n);
        /* C4: an instanced row's frame window is virtual — one seated plane,
         * drawn n_frames times — so only its base is bounded and sized here.
         * §37: the plate's closing delta sits one past its frame count — the
         * row bounds and validates it like every other delta of its window. */
        const uint32_t win = (delta && (e.flags & EF_INSTANCE))
                                 ? 1u
                                 : e.n_frames + ((e.flags & EF_DELTA_WRAP) ? 1u : 0u);
        if ((uint64_t)e.frame0 + win > ns)
            return reject("element %u names %u frames from row %u of a table with %u "
                          "rows: the animation runs off the end of the catalogue",
                          (unsigned)e.n, (unsigned)e.n_frames, (unsigned)e.frame0,
                          (unsigned)ns);
        const uint8_t delta_stream =
            delta && (t->bg_mode == 1 ? (i == (uint32_t)at_plate[0] && plate_anim)
                                      : win > 1u);
        for (uint32_t f = 0; f < win; f++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, e.frame0 + f, &sp);
            if (e.cls == CLS_SOUND) {
                /* D39: a sound places nothing on the glass, and its length is its
                 * plane's (w samples, which a u16 row column could not hold). */
                if (e.w || e.h)
                    return reject("element %u is a sound sized %ux%u: a sound places "
                                  "nothing, and its length is its plane's",
                                  (unsigned)e.n, (unsigned)e.w, (unsigned)e.h);
            } else if (f == 0u || !delta_stream) {
                if (sp.w != e.w || sp.h != e.h)
                    return reject("element %u is %ux%u while its plane is %lux%lu: the "
                                  "table and the catalogue describe different layers",
                                  (unsigned)e.n, (unsigned)e.w, (unsigned)e.h,
                                  (unsigned long)sp.w, (unsigned long)sp.h);
                if (f && (sp.diffx || sp.diffy))
                    return reject("element %u frame %u is a whole plane that names a "
                                  "rect position: a keyframe sits at the box origin",
                                  (unsigned)e.n, (unsigned)f);
            } else if (!sp.w && !sp.h && !sp.diffx && !sp.diffy) {
                /* A still in the loop (C5 §7.2): the row exists, applies nothing, and
                 * the canvas carries forward. rowDelta ships these; gif_apply_frame
                 * skips them; the §37 closing delta is never one — it changed pixels
                 * or the flag would not have been set. */
            } else if (!sp.w || !sp.h ||
                       sp.w > e.w || sp.h > e.h ||
                       sp.diffx < 0 || sp.diffy < 0 ||
                       (int32_t)sp.w + sp.diffx > e.w ||
                       (int32_t)sp.h + sp.diffy > e.h)
                return reject("element %u frame %u carries a %lux%lu rect at (%d,%d) "
                              "outside its %ux%u box: a delta that is not a delta is "
                              "refused before it paints",
                              (unsigned)e.n, (unsigned)f,
                              (unsigned long)sp.w, (unsigned long)sp.h,
                              (int)sp.diffx, (int)sp.diffy,
                              (unsigned)e.w, (unsigned)e.h);
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

    /* ---- the glass, in the dialect's own vocabulary ---- */
    blank_sprite(&t->overlay, 0, 0);
    for (int i = 0; i < CLK_TOURB_MAX; i++) blank_sprite(&t->tourbillon[i], 0, 0);
    if (at_plate[1] >= 0) {
        orb_bundle_elem_t e;
        orb_bundle_spr_t sp;
        orb_bundle_elem_at(sec, (uint32_t)at_plate[1], &e);
        orb_bundle_spr_at(sec, e.frame0, &sp);
        fill_sprite(&t->overlay, &e, &sp, NULL);
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
        c->first       = e.order;             /* D42: the depth, on the rows' scale */
        c->off         = sp.off;
        /* C4: an instanced row's frame stride is zero — every slot of the
         * domain reads the one seated plane, the §4.2 reading rule arrived at
         * through the window maths that already indexes off + f * frame_bytes.
         * parent_row and delay_ms ride the two new columns (0 = none/default;
         * the walk refused the claims a v3 stamp cannot carry). */
        c->frame_bytes = (e.flags & EF_INSTANCE)
                       ? 0u : (uint32_t)e.w * (uint32_t)e.h * 3u;
        c->value_min   = value_min;
        c->scale       = e.scale16;           /* 0 reads as 16 in comp maths */
        c->show        = (uint8_t)(e.flags & EF_HIDDEN ? 0 : 1);
        c->parent_row  = row_par[at_bank[slot]];
        c->delay_ms    = e.delay;
        /* flags stays 0: the bank gate costs every row past the plate at
         * w*h*3, so a FORMAT 3 tile is always the runtime-composited kind. The
         * under-dial case is one a FORMAT 2 bake could produce and a FORMAT 3
         * catalogue cannot describe. */
    }

    /* ---- D42: the moving rows. One AUX row, one clk_row_t, driven by its own
     * columns; row_check refused every row-local lie in the walk. What is left needs
     * the table: a twin's hand, a parent's arbour, the one animated row. Rows land in
     * slot order, so a twin's hand is always the row projected just before it. ---- */
    io->out_n_rows = 0;
    uint8_t row_of_elem[CLOCK_ROWS_MAX];     /* element index -> row index + 1 */
    memset(row_of_elem, 0, sizeof row_of_elem);
    for (int slot = 0; slot < CLK_ROW_MAX; slot++) {
        if (at_aux[slot] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_aux[slot], &e);
        if (!io->rows || !io->row_cap)
            return reject("a row at slot %u, and this dial was built without a row "
                          "table to project it into", (unsigned)slot);
        if (io->out_n_rows >= io->row_cap)
            return reject("more rows than this dial's table holds (%lu)",
                          (unsigned long)io->row_cap);
        if (e.flags & AUX_F_SHADOW) {
            /* The twin takes its pose from the row before it and draws under it, so
             * that row must exist and must not itself be a twin: a twin with no hand
             * can only draw a smudge where a hand should be. */
            orb_bundle_elem_t pe;
            if (slot == 0 || at_aux[slot - 1] < 0)
                return reject("a shadow twin at slot %u with no row before it: the twin "
                              "takes its pose from the row in the slot under it",
                              (unsigned)slot);
            orb_bundle_elem_at(sec, (uint32_t)at_aux[slot - 1], &pe);
            if (pe.flags & AUX_F_SHADOW)
                return reject("a shadow twin at slot %u under another twin: a twin "
                              "darkens a row, not a shadow", (unsigned)slot);
        }
        const uint32_t ri = io->out_n_rows++;
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, e.frame0, &sp);
        clk_row_t *r = &io->rows[ri];
        memset(r, 0, sizeof *r);
        fill_sprite(&r->spr, &e, &sp, NULL);
        r->tag       = (uint16_t)slot;
        r->src       = e.src;
        r->shape     = e.kind;
        r->digit     = e.digit;
        r->end       = e.end;
        r->cadence   = e.cadence;
        r->order     = e.order;
        r->zero      = e.zero;
        r->span      = e.span;
        r->dx        = e.dx;
        r->dy        = e.dy;
        r->value_min = e.vmin;
        r->value_max = e.vmax;
        r->cad_arg   = e.cad_arg;
        r->ease      = e.ease;
        r->flags     = (e.flags & AUX_F_SHADOW) ? CLK_FLAG_AUX_SHADOW : 0u;
        r->delay_ms  = e.delay;
        r->parent_row = row_par[at_aux[slot]];   /* an element index + 1 until below */
        row_of_elem[at_aux[slot]] = (uint8_t)(ri + 1u);
        if (e.n_frames > 1u) {
            /* The carriage: every plane of the row's frame window is one frame of the
             * bank, sharing the row's geometry, so the dial turns any frame about one
             * pivot and dirties one box. The frame time is the row's own. */
            if (t->car_row != 0xFFu)
                return reject("a second animated row at slot %u: a dial carries one "
                              "animated row (the carriage)", (unsigned)slot);
            t->car_row        = (uint8_t)ri;
            t->tourbillon_n   = e.n_frames;
            t->tourbillon_fps = (uint16_t)((1000u + e.delay / 2u) / e.delay);
            if (!t->tourbillon_fps) t->tourbillon_fps = 1;
            for (uint32_t f = 0; f < e.n_frames; f++) {
                orb_bundle_spr_t fp;
                orb_bundle_spr_at(sec, e.frame0 + f, &fp);
                fill_sprite(&t->tourbillon[f], &e, &fp, NULL);
                if (f && (fp.w < e.w || fp.h < e.h)) {
                    t->tourbillon[f].diffx = fp.diffx;
                    t->tourbillon[f].diffy = fp.diffy;
                    t->tourbillon[f].diffw = (uint16_t)fp.w;
                    t->tourbillon[f].diffh = (uint16_t)fp.h;
                }
            }
        }
    }
    /* A parent is a moving row, and a parent that turns shares its child's arbour:
     * the child's pose adds the parent's, and a rotation about another centre would
     * swing the child off its own pivot. The parent's index becomes the row table's. */
    for (uint32_t ri = 0; ri < io->out_n_rows; ri++) {
        clk_row_t *r = &io->rows[ri];
        if (!r->parent_row) continue;
        const uint8_t pr = row_of_elem[r->parent_row - 1u];
        if (!pr)
            return reject("row %u hangs on a row that is not a moving row: a row rides "
                          "another row's pose", (unsigned)r->tag);
        const clk_row_t *p = &io->rows[pr - 1u];
        if (p->span && (p->spr.centerx != r->spr.centerx || p->spr.centery != r->spr.centery))
            return reject("row %u rides row %u, which turns about another centre: a child "
                          "of a turning row shares its arbour",
                          (unsigned)r->tag, (unsigned)p->tag);
        r->parent_row = pr;
    }
    /* A bank row's parent stays an element index + 1 (clock_view reads none yet). */

    /* The dial's beat, as the scheduler reads it (orb_sched.c: second_sweep and
     * tick_rate). A sweep asks for its own named frame rate; one that names none
     * rides the fastest animation the dial already carries — the plate's, the
     * carriage's — and a sweep over nothing moving asks for the sweep tier, 15
     * (the Station of Oct 30: a sweep asking 1 Hz stepped twice a second). A dial
     * whose seconds step asks for its N a second, and a dial with no moving
     * seconds row for one. The plate's rate is the one its author named (bg_fps); a
     * delta plate that names none is not a sweep's tempo. */
    t->second_sweep = tempo_sweep ? 1u : 0u;
    if (tempo_sweep) {
        uint32_t fps = tempo_sweep_fps;
        if (!fps) {
            if ((t->bg_frames || plate_delta) && t->bg_fps > 0) fps = (uint32_t)t->bg_fps;
            if (t->tourbillon_fps > fps) fps = t->tourbillon_fps;
            if (fps < 2u) fps = CLK_SWEEP_TIER;
        }
        t->tick_rate = (uint8_t)(fps > 255u ? 255u : fps);
    } else {
        t->tick_rate = (uint8_t)(tempo_rate ? tempo_rate : 1u);
    }

    /* Extents, layer by layer, in FORMAT 2's own words: the same lie at either door
     * gets the same sentence. A layer the dial will not look at is not checked, for
     * the reason orb_clock3_parse gives. The plate and the keyframe are checked
     * against the panel rather than against a catalogue row because their size is
     * the panel's, which is the one extent a clock section cannot lie about. */
    if (t->plate_off % 2u ||
        (uint64_t)t->plate_off + PLATE_BYTES_T(scr_w, scr_h) > (uint64_t)art)
        return "the plate pixels lie outside the declared payload";
    for (uint32_t k = 0; k < io->out_n_rows; k++) {
        const clk_sprite_t *h = &io->rows[k].spr;
        if (h->show && h->w && h->h &&
            (uint64_t)h->color_off + SPRITE_BYTES_T(h) > art)
            return "row pixels lie outside the declared payload";
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
     * A keyframe bank — and every v3 table, and a flat v4 one — lands where it
     * has always landed: one zero rect (the keyframe copy the compiled path
     * takes discards the rect list), zero windows, and the OS beat in the delay
     * table: the same uniform figure a PNG frame pack's bake printed, the 67 ms
     * the shipped av-kit pair carries. C4's delta plate says its own truth:
     * one rect row per gif frame after the keyframe, taken from the plane's
     * catalogue row — and the row's own delay column, when it carries one,
     * replaces the tick figure, because a row that names its tempo means it.
     * The plate-frame table is unchanged: the bgAnim consumer's row list, one
     * offset per extra plane in bank order — frame 0 needs no row because the
     * consumer starts on the plate. */
    /* The delta dialect is spoken by the stamp and the scalars, not by a row's
     * shape: under plate_delta every gif frame IS a rect row — §4.2's reading
     * rule — and a delta that covers the whole plate is an honest delta with a
     * whole-panel bill, not a keyframe. The pre-C5 shapes (keyframe banks, still
     * plates) have no gif mode at all: their consumer rotates whole planes from
     * the bg_frames table. A heuristic over row extents would re-decide a question
     * the dialect already answered, and used to freeze exactly this full-cover
     * case by materializing no windows. */
    const int honest_rects = plate_delta;
    const uint32_t beat = (uint32_t)(t->tick_rate ? t->tick_rate : 1);
    uint32_t ms = (1000u + beat / 2u) / beat;
    if (!ms) ms = 1;
    if (delta && plate.delay)
        ms = plate.delay;
    if (honest_rects) {
        /* Windows the L3/L4 gif consumers already read: frame tgt applies
         * [rect_first[tgt], rect_first[tgt + 1]). §37: a row carrying its closing
         * delta gives frame 0 that one rect — the wrap applies a delta like every
         * other tick, and the canvas is the keyframe again. Without the bit the
         * keyframe applies nothing (it IS the canvas — the pre-§37 shape), and
         * every frame owns exactly the one rect its plane row named. */
        const uint32_t cbase = plate_wrap ? 1u : 0u;
        io->rect_first[0] = 0;
        if (plate_wrap) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, plate.frame0 + plate.n_frames, &sp);
            io->rects[0].x    = (uint16_t)(plate.x + sp.diffx);
            io->rects[0].y    = (uint16_t)(plate.y + sp.diffy);
            io->rects[0].w    = (uint16_t)sp.w;
            io->rects[0].h    = (uint16_t)sp.h;
            io->rects[0].off  = sp.off;
        }
        io->rect_first[1] = (uint16_t)cbase;
        for (uint32_t f = 1; f < (uint32_t)t->gif_frames; f++) {
            orb_bundle_spr_t sp;
            orb_bundle_spr_at(sec, plate.frame0 + f, &sp);
            io->rects[cbase + f - 1u].x  = (uint16_t)(plate.x + sp.diffx);
            io->rects[cbase + f - 1u].y  = (uint16_t)(plate.y + sp.diffy);
            io->rects[cbase + f - 1u].w  = (uint16_t)sp.w;
            io->rects[cbase + f - 1u].h  = (uint16_t)sp.h;
            io->rects[cbase + f - 1u].off = sp.off;
            io->rect_first[f + 1] = (uint16_t)(cbase + f);
        }
    } else {
        io->rects[0].x = io->rects[0].y = 0;
        io->rects[0].w = io->rects[0].h = 0;
        io->rects[0].off = 0;
        for (uint32_t i = 0; i <= (uint32_t)t->gif_frames; i++)
            io->rect_first[i] = 0;
    }
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

    /* ---- D39: the sound table. Every fact a sound row carries rides its own
     * columns (orb_clock3.h), so each refusal below is made from the table alone,
     * before a byte of the bank is resident: a loop whose beat disagrees with the
     * hand it ticks for, a stroke the hour pattern needs and the theme does not
     * carry, a plane a 16-bit load would fault on. ---- */
    int n_snd = 0;
    for (int k = 0; k < CLK_SND_SLOTS; k++) n_snd += at_sound[k] >= 0;
    if (n_snd && (!io->sounds || io->sound_cap < (uint32_t)n_snd))
        return reject("the bundle carries %d sounds and this dial was built without "
                      "room for them", n_snd);
    int pattern = -1;
    for (int slot = 0; slot < CLK_SND_SLOTS; slot++) {
        if (at_sound[slot] < 0) continue;
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, (uint32_t)at_sound[slot], &e);
        orb_bundle_spr_t sp;
        orb_bundle_spr_at(sec, e.frame0, &sp);
        if (e.src != CLK_SND_ENC_PCM16)
            return reject("sound slot %u is encoding %u: this dial plays 16-bit PCM at "
                          "%u Hz (encoding 0) and nothing else", (unsigned)slot,
                          (unsigned)e.src, (unsigned)CLK_SND_RATE);
        if (sp.h != 1u || sp.w < 2u || sp.size != sp.w * 2u)
            return reject("sound slot %u's plane is %lux%lu in %lu B: a sound is w "
                          "samples by 1, two bytes each", (unsigned)slot,
                          (unsigned long)sp.w, (unsigned long)sp.h, (unsigned long)sp.size);
        if (sp.off & 1u)
            return reject("sound slot %u starts at odd offset %lu: its samples would "
                          "load unaligned", (unsigned)slot, (unsigned long)sp.off);
        if (e.kind >= CLK_PAT_N)
            return reject("sound slot %u names hour pattern %u: the clock knows %u",
                          (unsigned)slot, (unsigned)e.kind, (unsigned)CLK_PAT_N);
        if (pattern >= 0 && (int)e.kind != pattern)
            return reject("sound slot %u names hour pattern %u and an earlier sound "
                          "names %d: a theme has one pattern", (unsigned)slot,
                          (unsigned)e.kind, pattern);
        pattern = e.kind;
        const bool loop = (e.flags & EF_LOOP) != 0;
        if (slot == CLK_SND_TICK_LOOP && !loop)
            return reject("the tick loop is not flagged to loop: it would tick once "
                          "through and stop");
        if (slot != CLK_SND_TICK_LOOP && loop)
            return reject("sound slot %u is flagged to loop: only the tick loop runs "
                          "until it is stopped", (unsigned)slot);
        if (slot == CLK_SND_TICK_LOOP) {
            const uint32_t bchz = e.vmax;
            if (!bchz)
                return reject("the tick loop names no beat: the dial cannot hold it "
                              "against the seconds hand");
            /* a whole number of beats, so the loop's seam is a beat boundary */
            if (((uint64_t)sp.w * bchz) % ((uint64_t)CLK_SND_RATE * 100u))
                return reject("the tick loop's %lu samples are not a whole number of "
                              "beats at %lu beats per 100 s", (unsigned long)sp.w,
                              (unsigned long)bchz);
            const uint32_t step = t->tick_rate ? t->tick_rate : 1u;
            if (!t->second_sweep && bchz != step * 100u)
                return reject("the tick loop beats %lu.%02lu times a second and the "
                              "seconds hand steps %lu: the sound would walk off the hand",
                              (unsigned long)(bchz / 100u), (unsigned long)(bchz % 100u),
                              (unsigned long)step);
            if ((uint64_t)e.vmin * bchz >= (uint64_t)CLK_SND_RATE * 100u)
                return reject("the tick loop's first beat at sample %u sits past one "
                              "beat", (unsigned)e.vmin);
        } else if (e.vmax || e.vmin) {
            return reject("sound slot %u carries a beat: only the tick loop has one",
                          (unsigned)slot);
        }
        clk_sound_t *o = &io->sounds[io->out_n_sounds++];
        o->pcm_off    = sp.off;
        o->n          = sp.w;
        o->first_beat = e.vmin;
        o->beat_chz   = e.vmax;
        o->slot       = (uint8_t)slot;
        o->flags      = loop ? CLK_SND_F_LOOP : 0u;
    }
    if (pattern >= 0) {
        /* The strokes a pattern sounds must be in the theme, or the hour would be
         * a silence the owner was promised a chime for. */
        const bool strike = at_sound[CLK_SND_STRIKE_HOUR] >= 0;
        if ((pattern == CLK_PAT_COUNT || pattern == CLK_PAT_SHIPS_BELLS) && !strike)
            return reject("the hour pattern strikes and the theme carries no hour stroke");
        if (pattern == CLK_PAT_MORSE &&
            (at_sound[CLK_SND_MORSE_DIT] < 0 || at_sound[CLK_SND_MORSE_DAH] < 0))
            return reject("the hour pattern is morse and the theme lacks a dit or a dah");
        if (pattern == CLK_PAT_REPEATER &&
            (!strike || at_sound[CLK_SND_STRIKE_QUARTER] < 0 ||
             at_sound[CLK_SND_STRIKE_MINUTE] < 0))
            return reject("the hour pattern is a repeater and the theme lacks one of its "
                          "three strokes");
        io->out_pattern = (uint8_t)pattern;
    }
    return NULL;
}
