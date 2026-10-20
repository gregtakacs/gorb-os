/*
 * main/art/orb_elems.c — see the header for what this answers and why it is pure.
 *
 * One loop over one table. Everything that makes it more than that is a rule the
 * reader cannot afford to get wrong quietly, and each is commented where it bites.
 * No allocation and no state: a resolution is a walk of the caller's resident
 * section, which is what lets the same call serve the tick and the install gate.
 */
#include "orb_elems.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The class ids and flag bits, copied rather than included. orb_clock3.c takes the
 * same local-define route for the same reason: orb_assets.h drags clk_types.h and the
 * enum's home is the app ABI, while this file's home is a reader that must compile on
 * the host with nothing but orb_bundle.h. The three-way mirror is then pinned where it
 * can actually fail — host_orbelems_test.py re-declares these against
 * theme_schema.CLASS_NAMES/EF_* and against orb_assets.h's enum text, so a drift is a
 * suite failure and not a dial that resolves somebody else's slot. */
#define CLS_TEXT   0
#define CLS_SPRITE 1
#define CLS_HAND   2
#define CLS_PLATE  3
#define CLS_RING   4
#define CLS_BANK   5
#define CLS_LAYER  6
#define CLS_STATUS 7

#define EF_ALPHA   0x01u
#define EF_LOOP    0x02u
#define EF_OS_ADV  0x04u
#define EF_HIDDEN  0x08u

/* Class names in enum order, for the refusal sentences. Fixed strings: a sentence an
 * owner reads has to name the thing in the words the vocabulary uses. */
static const char *const CLS_NAME[8] = {
    "text", "sprite", "hand", "plate", "ring", "bank", "layer", "status"
};

static const char *cls_word(uint16_t cls)
{
    return cls < 8 ? CLS_NAME[cls] : "unknown-class";
}

const char *orb_elems_why(orb_elems_ans_t a)
{
    switch (a) {
    case ORB_ELEMS_OK:       return "ok";
    case ORB_ELEMS_NO_SLOT:  return "no-such-slot";
    case ORB_ELEMS_EMPTY:    return "slot-carried-empty";
    case ORB_ELEMS_NO_FRAME: return "frame-past-the-animation";
    case ORB_ELEMS_TORN:     return "table-and-catalogue-disagree";
    default:                 return "bad-call";
    }
}

/* One sentence at a time, printf-checked, the way orb_clock3.c's reject() does it. A
 * buffer too small for the truth says less, never wrong: the answer enum is the fact
 * and the sentence is its explanation. */
static void say(char *why, size_t cap, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t cap, const char *fmt, ...)
{
    if (!why || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

/* The row that fulfils (cls, slot), at frame `frame`: found, then held to three
 * checks before it becomes a record a caller can draw with. */
static orb_elems_ans_t seat(const orb_bundle_sec_t *sec, const orb_bundle_elem_t *e,
                           uint16_t cls, uint16_t slot, uint32_t frame,
                           orb_elem_t *out, char *why, size_t cap)
{
    if (e->flags & EF_HIDDEN) {
        say(why, cap, "the theme carries %s slot %u switched off (EF_HIDDEN), so it draws "
                      "nothing and nothing is what it should draw",
            cls_word(cls), (unsigned)slot);
        return ORB_ELEMS_EMPTY;
    }
    if (frame >= e->n_frames) {
        say(why, cap, "%s slot %u carries %u frame%s and frame %lu was asked for: past "
                      "the window is a caller counting further than the theme wrote",
            cls_word(cls), (unsigned)slot, (unsigned)e->n_frames,
            e->n_frames == 1 ? "" : "s", (unsigned long)frame);
        return ORB_ELEMS_NO_FRAME;
    }
    const uint32_t row = (uint32_t)e->frame0 + frame;
    if (row >= sec->n_spr) {
        say(why, cap, "%s slot %u wants frame %u of %u, which is bank row %lu of a "
                      "catalogue with %u rows: the animation runs off the end of the art",
            cls_word(cls), (unsigned)slot, (unsigned)frame, (unsigned)e->n_frames,
            (unsigned long)row, (unsigned)sec->n_spr);
        return ORB_ELEMS_TORN;
    }

    orb_bundle_spr_t sp;
    orb_bundle_spr_at(sec, row, &sp);

    /* A plane is an opaque RGB565 plate, or that plate with an alpha stride behind it,
     * and its own size says which. Anything else means this row is not a plane this
     * build can draw — the same arithmetic orb_asset_slot_bytes() prices, read from the
     * catalogue's extent rather than from the row's claim, because a disagreement
     * between the two IS the defect being reported. */
    const uint64_t px = (uint64_t)sp.w * (uint64_t)sp.h;
    const bool masked = (sp.size == px * 3u);
    if (!masked && sp.size != px * 2u) {
        say(why, cap, "bank row %lu (%s slot %u frame %lu) claims %lu B of a %lux%lu "
                      "plane; that is neither %lu B opaque nor %lu B with a mask",
            (unsigned long)row, cls_word(cls), (unsigned)slot, (unsigned long)frame,
            (unsigned long)sp.size, (unsigned long)sp.w, (unsigned long)sp.h,
            (unsigned long)(px * 2u), (unsigned long)(px * 3u));
        return ORB_ELEMS_TORN;
    }
    /* An extent of 0 means "take the class's own default" (theme_schema), so the
     * plane's extent is the truth and the row raises no claim to contradict it. A row
     * that DOES claim one has to agree with the pixels it points at, or the table and
     * the bank describe different layers and the draw would be torn. */
    if (e->w && e->h && (sp.w != e->w || sp.h != e->h)) {
        say(why, cap, "%s slot %u is %ux%u in the table and %lux%lu in the bank: the two "
                      "describe different layers, so neither can be drawn",
            cls_word(cls), (unsigned)slot, (unsigned)e->w, (unsigned)e->h,
            (unsigned long)sp.w, (unsigned long)sp.h);
        return ORB_ELEMS_TORN;
    }

    out->cls      = cls;
    out->slot     = slot;
    out->order    = e->order;
    out->flags    = e->flags;
    out->alpha    = (e->flags & EF_ALPHA) ? e->alpha : 255u;
    out->os_adv   = !!(e->flags & EF_OS_ADV);
    out->loop     = !!(e->flags & EF_LOOP);
    out->n_frames = e->n_frames;
    out->frame0   = e->frame0;
    out->x        = e->x;
    out->y        = e->y;
    out->w        = e->w ? e->w : (uint16_t)sp.w;
    out->h        = e->h ? e->h : (uint16_t)sp.h;
    out->pivot_x  = e->pivot_x;
    out->pivot_y  = e->pivot_y;
    out->degrees  = e->degrees;
    out->radius   = e->radius;
    out->scale16  = e->scale16;
    out->kind     = e->kind;      /* the two D32 columns, both zero off a BANK row */
    out->src      = e->src;
    out->off      = sp.off;
    out->size     = sp.size;
    out->plane_w  = sp.w;
    out->plane_h  = sp.h;
    out->masked   = masked;
    return ORB_ELEMS_OK;
}

orb_elems_ans_t orb_elems_at(const orb_bundle_sec_t *sec, uint16_t cls, uint16_t slot,
                             uint32_t frame, orb_elem_t *out, char *why, size_t cap)
{
    if (!sec || !out) {
        say(why, cap, "orb_elems_at called with a NULL section or answer");
        return ORB_ELEMS_ARG;
    }

    for (uint32_t i = 0; i < sec->n_elem; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == cls && e.slot == slot) return seat(sec, &e, cls, slot, frame,
                                                        out, why, cap);
    }

    say(why, cap, "the theme's table carries no %s slot %u: the app declared a place to "
                  "draw and this theme left it unfilled", cls_word(cls), (unsigned)slot);
    return ORB_ELEMS_NO_SLOT;
}

uint32_t orb_elems_last_frame(const orb_elem_t *e)
{
    return (!e || !e->n_frames) ? 0u : (uint32_t)e->n_frames - 1u;
}

uint32_t orb_elems_next(const orb_elem_t *e, uint32_t frame)
{
    if (!e || e->n_frames < 2u) return 0u;   /* a still row has nowhere to go         */
    const uint32_t n = frame + 1u;           /* R6: one tick advances one frame. No
                                              * rate, no per-frame delay, and no
                                              * catching a beat back by skipping      */
    if (n < e->n_frames) return n;
    return e->loop ? 0u : (uint32_t)e->n_frames - 1u;
}

