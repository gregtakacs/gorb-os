/*
 * main/art/orb_elems.h — one app's element table, answered by class and slot.
 *
 * D32 ruled that a theme declares WHICH elements an app displays, where they sit,
 * how deep they stack and how many frames they animate through, and that the app
 * says how to paint them. `orb_bundle.c` already unpacks a row; `orb_clock3.c`
 * already projects a whole table into the clock's one legacy `clk_theme_t`. What
 * neither does is answer the question an app actually asks at draw time:
 *
 *     "give me the pixels for (class, slot), at frame N"
 *
 * That question is the join D32 needs and this file owns it. It is app-neutral on
 * purpose: the clock's slot NAMES live in the clock (and in `theme_schema.CLOCK_SLOTS`,
 * which is the shared authority they copy), and a theme row carries only the
 * (class, slot) pair the row fulfils. So the same calls serve the next app that
 * draws from a theme, which is the whole point of D32's "a different theme
 * re-composes the same app".
 *
 * WHY A FRAME LOOKUP IS A CATALOGUE ROW AND NOT A STRIDE. An element's frames are
 * consecutive rows of the section's sprite catalogue starting at its `frame0`
 * (orb_bundle.h's own reading of the field), and a catalogue row carries its own
 * off/size/w/h. Deriving frame N as `off + N * stride` would assume every plane in
 * a bank costs the same, which the plate's colour-only first row already breaks.
 * The catalogue is the authority on what a plane is, so this file reads it and
 * checks the extent, rather than reproducing an assumption the writer never made.
 *
 * WHAT THIS FILE DOES NOT DO: it does not allocate, map a card, touch NVS, or
 * include LVGL or an ESP header. It reads a parsed section the caller already holds
 * resident, which is what puts it inside `find()`'s contract (orb_assets.h: a
 * source's find runs on the LVGL task and may not touch the card) and what lets
 * `tools/host_orbelems_test.py` drive every branch here against the shipped
 * reader's bytes with no board in the loop.
 *
 * The refusal answers are the point, not the error path. D26's direction is "say
 * cannot-draw-yet, never draw wrong", so an unbound slot, an empty row, a frame past
 * the element's own window and a plane whose size matches neither an opaque nor a
 * masked plate all come back as distinct answers the caller can print, rather than
 * as a zeroed record that paints the wrong thing at the right address.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "orb_bundle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The answers, in the order a caller meets them. Two misses stay separate words for
 * orb_assets.h's reason: "nobody filed that slot" is a bake fault and "the theme
 * bound it empty" is a theme author's choice, and only the second is guidance. */
typedef enum {
    ORB_ELEMS_OK = 0,     /* resolved; out is a whole record                     */
    ORB_ELEMS_NO_SLOT,    /* no row of that class carries that slot              */
    ORB_ELEMS_EMPTY,      /* the row is EF_HIDDEN: declared, bound, switched off  */
    ORB_ELEMS_NO_FRAME,   /* the frame asked for is past this element's own window */
    ORB_ELEMS_TORN,       /* table and catalogue disagree about the plane's size   */
    ORB_ELEMS_ARG         /* a NULL in the call: the caller's bug, not the file's  */
} orb_elems_ans_t;

const char *orb_elems_why(orb_elems_ans_t a);   /* never NULL                      */

/* One element, resolved at one frame. The geometry is the THEME's — placement, extent,
 * pivot and depth are what D32 moved out of the app — and `degrees` is the theme's own
 * fixed rotation, which the app adds to whatever the wall clock says. */
typedef struct {
    uint16_t cls, slot;           /* echoed back: the question this answers        */
    uint8_t  order;               /* depth; lower paints first, < ELEM_ORDER_N     */
    uint8_t  flags;               /* the row's EF_* bits, for the caller's blend   */
    uint8_t  alpha;               /* 255 unless EF_ALPHA is set (schema's rule)    */
    bool     os_adv;              /* EF_OS_ADV: the OS owes this row a frame ticker */
    bool     loop;                /* EF_LOOP: wrap. else clamp at the last frame    */
    uint16_t n_frames, frame0;    /* the element's own frame window into the bank   */
    int16_t  x, y;                /* theme-placed top-left, app-local coords        */
    uint16_t w, h;                /* the extent the row claims                      */
    int16_t  pivot_x, pivot_y;    /* rotation origin, relative to (x, y)            */
    uint16_t degrees;             /* fixed-point 100ths, 0..35999                   */
    uint16_t radius, scale16;     /* RING's circle, and the comp maths' scale       */
    uint32_t off, size;           /* THIS frame's plane, inside the section's bank  */
    uint32_t plane_w, plane_h;    /* what the catalogue says the plane is            */
    bool     masked;              /* size == w*h*3: an alpha plane follows the colour */
} orb_elem_t;

/** The plane for (cls, slot) at frame `frame`. Fills `out` on OK and leaves it
 *  untouched on every other answer, for the same reason orb_asset_resolve() gives: a
 *  caller that ignores the return value then draws nothing rather than half a record.
 *  When `why` is non-NULL a refusal writes the sentence naming the class, the slot and
 *  the defect into it, bounded by `cap`; a NULL `why` asks for the answer alone. */
orb_elems_ans_t orb_elems_at(const orb_bundle_sec_t *sec, uint16_t cls, uint16_t slot,
                             uint32_t frame, orb_elem_t *out, char *why, size_t cap);

/** R6's rule, in one call: the frame after `frame` for this element. One tick
 *  advances one frame; EF_LOOP wraps and a clear flag clamps at the last frame, so an
 *  animation that ends stays ended instead of cycling a carriage backwards. */
uint32_t orb_elems_next(const orb_elem_t *e, uint32_t frame);

/** The highest frame this element can be asked for, inclusive. n_frames - 1. */
uint32_t orb_elems_last_frame(const orb_elem_t *e);

#ifdef __cplusplus
}
#endif
