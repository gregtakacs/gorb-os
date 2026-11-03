/* orb_clock3.h — the FORMAT 3 clock section, projected into the compiled dial.
 *
 * P6.5 §25.5(2): the device reader that makes a stored bundle drawable. A bundle
 * carries no blob for the clock to read, so this file rebuilds the two things
 * orb_stage.c actually consumes — one clk_theme_t and the blob base the colour
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

/* The rect table C4's delta plate can name: one row per gif frame after the
 * keyframe, each filled from the delta plane's own catalogue row (§4.3). Every
 * other table — a keyframe bank, a flat v4 bake, any v3 file — keeps the one
 * zero rect it has always been handed, the keyframe copy the compiled path
 * takes discarding the list. Sized at CLK_BG_MAX because that is the gif bound
 * the projection refuses against; a rect can never overrun a table its own
 * frame count was refused past. And the plate-frame table: still as the one
 * keyframe row a still plate has always been handed, or §31's keyframe bank,
 * where a plate row of N frames writes N-1 extra-plane offsets at rows
 * 0..N-2 (frame 0 stays the plate the scalars paint). */
#define ORB_CLOCK3_N_RECTS CLK_BG_MAX
#define ORB_CLOCK3_N_BGF   CLK_BG_MAX

/* D39: the clock's sound slots (theme_schema.CLOCK_SOUND_SLOTS, mirrored and pinned
 * by the suite). One slot per event, so any of them can be an owner's upload; an
 * empty slot is silence for its event. docs/history/sound-plan.md §3.1. */
enum {
    CLK_SND_TICK_LOOP = 0,   /* the tick loop, anchored to the wall second   */
    CLK_SND_CHIME_Q1,        /* :15                                          */
    CLK_SND_CHIME_HALF,      /* :30                                          */
    CLK_SND_CHIME_Q3,        /* :45                                          */
    CLK_SND_CHIME_HOUR,      /* :00, before the hour pattern                 */
    CLK_SND_STRIKE_HOUR,     /* the hour stroke the pattern repeats          */
    CLK_SND_JUMP_MINUTE,     /* a minute row's step (D41)                    */
    CLK_SND_JUMP_HOUR,       /* an hour row's step                           */
    CLK_SND_JUMP_DATE,       /* a calendar row's step at midnight            */
    CLK_SND_MORSE_DIT,       /* the morse pattern's two symbols              */
    CLK_SND_MORSE_DAH,
    CLK_SND_STRIKE_QUARTER,  /* the repeater's ting-tang                     */
    CLK_SND_STRIKE_MINUTE,   /* the repeater's high stroke                   */
    CLK_SND_SLOTS            /* 13 */
};

/* The hour pattern (§5.1), carried in every sound row's `kind` column and required
 * to agree across them. */
enum {
    CLK_PAT_COUNT = 0, CLK_PAT_SHIPS_BELLS, CLK_PAT_MORSE, CLK_PAT_REPEATER,
    CLK_PAT_NONE, CLK_PAT_N
};

/* A SOUND row's facts ride columns it already has, never a header in the bank: this
 * projection is pure and runs before a byte of the bank is resident (orb_theme.c's
 * load order), so only what the table says can be refused at the door.
 *   src     the encoding: 0 = 16-bit LE mono PCM at 22,050 Hz, the only one wired
 *   kind    the theme's hour pattern, CLK_PAT_*, the same on every sound row
 *   flags   EF_LOOP on the tick loop and on nothing else
 *   vmax    the loop's beat in beats per 100 s (100 = 1/s); 0 on every one-shot
 *   vmin    the sample the loop's beat 0 starts on, under one beat
 * The catalogue row is the bare PCM: w samples, h 1, size 2w, at an even offset so
 * a 16-bit load from the mapped bank is aligned. */
#define CLK_SND_RATE      22050u
#define CLK_SND_ENC_PCM16 0u
#define CLK_SND_F_LOOP    0x01u

/* One projected sound: where its PCM starts inside the section's bank (always
 * even, so a 16-bit load is aligned) and the row's facts. */
typedef struct {
    uint32_t pcm_off;        /* bank-relative */
    uint32_t n;              /* samples */
    uint32_t first_beat;
    uint16_t beat_chz;
    uint8_t  slot;           /* CLK_SND_* */
    uint8_t  flags;          /* CLK_SND_F_* */
} clk_sound_t;

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
     *  The carriage's frames land in the theme struct (see `rows` below). */
    clk_comp_t      *comps;
    uint32_t         comp_cap;
    uint32_t         out_n_comps;

    /* D42: the row table. A bundle's AUX rows — every hand, pointer, ring, still
     *  layer, shadow twin and the carriage — project here, one clk_row_t per row, in
     *  slot order. Same contract as `comps` directly above: a caller with no room for
     *  rows passes NULL/0 and the projection refuses a bundle that names one, so a dial
     *  never comes up with its hands quietly missing. The animated row's frames land in
     *  the theme struct (tourbillon[], tourbillon_n/fps, car_row). */
    clk_row_t       *rows;
    uint32_t         row_cap;
    uint32_t         out_n_rows;

    /* D39: the sound table, one clk_sound_t per SOUND row in slot order, and the
     *  hour pattern they agree on (CLK_PAT_NONE when no row carries one). Same
     *  contract as the two tables above: NULL/0 refuses a bundle that names a
     *  sound, so a dial never comes up quietly missing its chimes. */
    clk_sound_t     *sounds;
    uint32_t         sound_cap;
    uint32_t         out_n_sounds;
    uint8_t          out_pattern;

    /* Out: where the section's bank sits inside a mapped art region, so a
     *  multi-section bundle's clock pixels are found by adding this to the
     *  region base, and how long that bank is. the stage's bounds check reads
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
