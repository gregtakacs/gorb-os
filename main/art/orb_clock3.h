/* orb_clock3.h — the FORMAT 3 clock section, projected into the compiled dial.
 *
 * P6.5 §25.5(2): the device reader that makes a stored bundle drawable. A bundle
 * carries no blob for the clock to read, so this file rebuilds the two things
 * clock_view.c actually consumes — one clk_theme_t and the blob base the colour
 * offsets index — out of the element table §25.2 ruled and the four gif tables
 * §25.6 proved derivable. It is `bake_clock.py --from-bundle` on the device, over
 * the same mapping, refusing the same list (§25.3) for the same reasons.
 *
 * Pure by the same rule that keeps `orb_clock3_parse` testable: no allocator,
 * no card, no partition, no static. The caller owns the theme struct and the four
 * tables and passes their capacities down, so a host vector can drive the whole
 * function with static buffers and the caps it wants to prove.
 *
 * The mirror this file is accountable to is three-way, and the suite enforces it:
 * `rebake_bundle.py` (pair -> table), `bake_clock.py --from-bundle` (table -> pair,
 * the host half of this exact projection) and `tools/host_clock3_test.py`, which
 * compiles this file with host gcc against the writer's own bytes and requires
 * every field of the shipped av-kit pair back out of the product bundle.
 */
#ifndef ORB_CLOCK3_H
#define ORB_CLOCK3_H

#include <stdbool.h>
#include <stdint.h>

#include "clk_types.h"          /* clk_theme_t, clk_sprite_t, clk_bg_rect_t */
#include "orb_bundle.h"            /* orb_bundle_sec_t, orb_bundle_elem_t      */

#ifdef __cplusplus
extern "C" {
#endif

/* The one rect a FORMAT 3 clock section can name: a plate keyframe with no
 * deltas (§25.6). And the plate-frame table: still as the one keyframe row it
 * has always been, or §31's keyframe bank, where a plate row of N frames writes
 * N-1 extra-plane offsets at rows 0..N-2 (frame 0 stays the plate the scalars
 * paint). Sized as a promise, not a capacity — the capacities a caller must
 * really have are the gif tables' and CLK_BG_MAX's. */
#define ORB_CLOCK3_N_RECTS 1u
#define ORB_CLOCK3_N_BGF   CLK_BG_MAX

/** The caller's room, its own clock_assets.h, and where the result goes. Every
 *  out pointer is required; nothing here allocates, and nothing here writes a
 *  byte outside the capacities the caller declared. */
typedef struct {
    clk_theme_t    *theme;
    clk_bg_rect_t  *rects;      /* >= ORB_CLOCK3_N_RECTS rows           */
    uint16_t       *rect_first; /* >= gif_frames + 1, the walk's window  */
    uint16_t       *gif_delays; /* >= gif_frames                        */
    uint32_t       *bg_frames;  /* >= ORB_CLOCK3_N_BGF rows             */
    uint32_t        first_cap;
    uint32_t        delays_cap;

    /* D32 slice 2: the complication table. A caller with no room for comps
     *  passes NULL/0 and the projection refuses a bundle that names one — the
     *  same refuse-whole a refused class is, never a half-drawn dial. Rows land
     *  in record order by BANK slot, so comp0 is s_geo.comps[0] whether or not
     *  comp1 exists; the count written out is the number of rows projected.
     *  The tourbillon needs no surface of its own: its frames land in the
     *  theme struct the caller already handed over, at
     *  theme->tourbillon[]/tourbillon_n/tourbillon_fps/tourbillon_start. */
    clk_comp_t      *comps;
    uint32_t         comp_cap;
    uint32_t         out_n_comps;

    /* D34: the pointer table. A bundle's AUX rows project here, one clk_aux_t per
     *  row, in slot order. Same contract as `comps` directly above: a caller with no
     *  room for pointers passes NULL/0 and the projection refuses a bundle that names
     *  one, so a dial never comes up with its sub-dial hands quietly missing. The row
     *  carries its own drive (src/motion/span/zero/vmin/vmax) because, unlike a BANK
     *  row, an AUX row is not indexed by its value — it MOVES by it. */
    clk_aux_t       *aux;
    uint32_t         aux_cap;
    uint32_t         out_n_aux;

    /* Out: where the section's bank sits inside a mapped art region, so a
     *  multi-section bundle's clock pixels are found by adding this to the
     *  region base, and how long that bank is. clock_view's bounds check reads
     *  the second; orb_theme's blob accessor reads the first. */
    uint32_t        out_art_off;
    uint32_t        out_art_size;
} orb_clock3_io_t;

/** Fill `io` from one parsed clock section. NULL on success, otherwise the reason
 *  this table is not a clock this dial can draw — in words, and the same words
 *  `--from-bundle` prints for the same lie, because a refusal an owner can read at
 *  either end of the wire is the point of D26. */
const char *orb_clock3_parse(const orb_bundle_sec_t *sec, orb_clock3_io_t *io);

/** The slug a clock section answers to, in the writer's and the reader's one
 *  spelling. A bundle may carry any set of sections; the dial asks for this one. */
#define ORB_CLOCK3_SLUG "clock"

#ifdef __cplusplus
}
#endif
#endif /* ORB_CLOCK3_H */
