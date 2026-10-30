/**
 * clock_view.c — themed ANALOG clock (Steam Punk et al.), ported from the
 * orb-os (Arduino) theme compositor onto LVGL 9 / ESP-IDF.
 *
 * The dial is composed as LVGL sibling objects rather than one CPU canvas:
 *
 *   plate (static RGB565 lv_image, never invalidated)
 *     -> per hand, back-to-front in the theme's order:
 *          shadow (planar RGB565A8 lv_image, rotated about the hand pivot)
 *          hand   (planar RGB565A8 lv_image, rotated about the hand pivot)
 *     -> overlay (hub cap), drawn last / on top
 *        (the overlay is where a baked-on GLASS PNG lives: one full-coverage
 *        A8 layer composited above everything, e.g. watch-kit glass+bezel
 *        merged at bake time — this theme's is exactly that)
 *  The second layer may instead be an ANIMATED hand: a --tourbillon frame
 *  bank (clk_theme.tourbillon[]) swapped by wall-clock frame and rotated by
 *  the seconds angle like any other hand (see tourbillon_advance).
 *
 * A shadow belongs to its own hand and sits directly under IT, not under every
 * hand: the minute hand casts onto the plane the second hand lives on, so the
 * minute shadow must overpaint the second hand it falls across. (orb-os lays
 * all the shadows down first; that reads as a smudge across a hand the theme
 * orders below, so this port interleaves instead — see clock_view_init and
 * the layer table's depth sort, which must and do agree on the stack.)
 *
 * Because LVGL runs in LV_DISPLAY_RENDER_MODE_PARTIAL it renders and flushes
 * only the invalidated rectangle. The hands are baked TRIMMED to their opaque
 * ink bounding box (tools/bake_clock.py), so rotating one dirties only its
 * swept wedge — not the whole 466x466 page. The plate is drawn once and never
 * re-flushed while it is static. This is what makes the dial cheap to animate,
 * and it is the whole reason the source PNGs (a 542x542 canvas that is 3.4% ink
 * for the minute hand) must be cropped before they reach LVGL.
 *
 * Geometry — per-hand pivot, screen centre, blend, draw order, shadow offset,
 * sweep/railway, plate-follow, background animation — comes from a generated
 * header (assets/clock_assets.h) produced by tools/bake_clock.py from the
 * theme's clock_style.json, so any theme folder drops in without touching the C.
 * The JSON is parsed OFFLINE; there is no runtime JSON on the device.
 *
 * Both the second tick and the (optional) smooth-sweep beat run as LVGL timers,
 * i.e. inside lv_timer_handler on the one LVGL task — the only context allowed
 * to touch widgets.
 *
 * Fidelity notes / known gaps for THIS build:
 *   - blend 2 ("screen") has no LVGL image blend mode -> mapped to ADDITIVE
 *     with a one-shot warning. (Steam Punk uses blend 0 everywhere.)
 *   - Text banners (text1/text2) are not drawn. Steam Punk sets show=false on
 *     both, and the theme's custom baked fonts / curved / glow text need a font
 *     pipeline this port does not yet carry. A theme that turns them on will
 *     show the dial without the words. Tracked as a follow-on.
 *
 * Background modes (clk_theme.bg_mode, chosen by tools/bake_clock.py):
 *   0 = classic theme plate + baked bgAnim plate frames (full-page source
 *       swaps; kept for non-GIF themes).
 *   1 = GIF DELTA BANK: one full keyframe + one changed-region rect per frame,
 *       §37 included: frame 0's rect is the closing delta that returns the
 *       wrapped canvas to the keyframe, so the chain is a cycle and every tick
 *       applies a delta (packed offline against the simulated device canvas,
 *       perceptual threshold). The keyframe is copied into a PSRAM canvas at init; the
 *       plate widget is an RGB565 image whose data IS that canvas. Each tick
 *       memcpy's the next frame's rects into the canvas and invalidates just
 *       those areas, so LVGL renders + flushes only the changed background
 *       plus the wedges the rotating hands cover/uncover — never a full-page
 *       source swap. The tick runs as fast as the flush pipeline allows
 *       (target 25 fps, self-throttled); a per-second log reports the
 *       achieved rate and average flush cost via display_bench_snapshot.
 */

#include "clock_view.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "display.h"
#include "orb_gfx.h"   /* the OS compositor: gfx_commit() paints the layers this file picks */
#include "orb_sched.h" /* the delta bank + frame tick: the canvas, the plate frames, the ladder */
#include "clock_base.h"  /* §26: the face shown when no theme is resident */
#include "orb_svc.h"    /* orb_time_now(): the dial's wall second, OS-owned since the P3 time commit */
#include "orb_layers.h" /* orb_layer_desc_t + orb_asset_class_prim: the rows clock_layers_build claims */
#include "orb_elems.h"  /* orb_elems_at: the element dialect's reader — Migration 2's first caller */
#include "orb_clock3.h" /* ORB_CLOCK3_SLUG: the one name the clock's section answers to */

#include "clk_types.h"   /* clk_theme, clk_sprite_t, clk_bg_frames, CLK_BLOB_BYTES */
/* Second, deliberately: this header #defines the names clock_assets.h just defined
 * as objects, so including it before would expand a macro inside the generated
 * struct definition. Everything below this line reads the theme through
 * orb_theme_geo() — which is the whole firmware change for a theme that arrives on
 * an SD card instead of in the build. */
#include "orb_theme_geo.h"
#include "art_bank.h"       /* the bank the above offsets index into */
#include "orb_files.h"      /* the source this init re-declares through orb_theme_src_register() */
#include "orb_sd.h"         /* ORB_SD_THEME_DIR, the directory that source names */
#include "orb_theme.h"      /* orb_theme_src_register(), and the geo the dial draws */

static const char *TAG = "clock";

static lv_obj_t *s_scr;
static lv_obj_t *s_overlay;
static lv_obj_t *s_hand[5];
static lv_obj_t *s_shadow[5];

static lv_image_dsc_t s_overlay_dsc;
static lv_image_dsc_t s_hand_dsc[5];
static lv_image_dsc_t s_shadow_dsc[5];

/* Which motions travel in pixels rather than an angle. One predicate, shared by the
 * maths, the mark and the painter, because a slide recognised in one place and not the
 * other is a pointer whose rows nobody dirtied — ghost ink outside the restored rows,
 * the exact failure the swept-box contract exists to prevent. */
#define MOTION_IS_SLIDE(k) ((k) == CLK_MOTION_SLIDE_X || (k) == CLK_MOTION_SLIDE_Y)

/* ---- the sub-dial pointers (the theme's clk_aux_t rows) -------------------
 * One widget, descriptor, and last-shown angle per row — the same shape of
 * state s_hand/s_shadow/s_ang_10 are for the five compiled-in slots, and for
 * the same reason: the L4 hook paints from the angles, the classic path from
 * the widgets, and both read ONE truth per tick. A hand and its blur/offset
 * twin are two rows (see the aux block for the pairing rule), which is why the
 * arrays are indexed by row and not by pointer. */
static lv_obj_t *s_aux_obj[CLK_AUX_MAX];
static lv_image_dsc_t s_aux_dsc[CLK_AUX_MAX];
/* What is currently shown per row, in the unit the row's MOTION names: tenths of a
 * degree for a rotate/retrograde/indexed pointer, whole pixels for a slide. One array
 * covers both because a row has one motion and never changes it, and a "last shown"
 * figure that had to be two types is where a slide and a rotation would start
 * disagreeing about the same tick. AUX_HIDDEN (INT32_MIN) is the nothing-drawn mark,
 * unreachable by either unit. */
static int32_t   s_aux_ang_10[CLK_AUX_MAX];
#define AUX_HIDDEN INT32_MIN
/* The record a slide actually paints: its theme sprite with centery/centerx displaced
 * by the current offset. Kept whole rather than computed inside the commit hook, which
 * runs per flush and must not redo the arithmetic per pointer per flush. */
static clk_sprite_t s_aux_pose[CLK_AUX_MAX];
static uint32_t  s_aux_logged;                /* bit i: "no input" printed for row i */
static int       s_aux_at = -1;               /* order position the block inserts before */



/* CLK_BG_DIRECT_PUSH, the L3/L4 switch the block below and the scheduler's delta
 * push both key on, lives in orb_sched.h: one number, both TUs, so the push and
 * the hook can never disagree about who owns a row. */


/* GIF-mode hand beat state (hand_angles): tick counter, the true seconds it
 * was last re-anchored to, and the wall fraction held between beats. */
static uint32_t         s_bt_ctr = 0;
static int              s_bt_sec = -1;
static double           s_bt_frac = 0.0;

/* ---- tourbillon: an animated second hand (frame bank + device rotation) ----
 * The kit's seconds layer is N full-canvas frames that share geometry and
 * differ only in internal motion (an oscillating balance); the ORBIT around
 * the dial centre is the same rotation every hand already gets, driven by the
 * seconds angle (the kit manifest's seconds/60 x 360 formula). Between frame
 * swaps only the pixels inside the baked diff rect vs the PREVIOUS frame can
 * differ, so the per-tick re-mark is that rotated diff box — not the
 * carriage's full swept box, which would re-composite ~40% of the page every
 * tick and re-create the full-page plateau the static re-mark was retired
 * over (docs/rows-capacity-2026-10-10.md). */
static int32_t        s_tourb_shown = -1;   /* frame composited now, -1 = never */
static uint32_t       s_tourb_t0;           /* lv_tick ms the frame clock started */
static lv_image_dsc_t s_tourb_dsc[CLK_TOURB_MAX];

/* D32 ruling C: the carriage is its own object. A FORMAT 3 theme places it
 * with the carriage row's geometry and turns it with the seconds angle plus
 * the row's start orientation (R-G's wall sampler, one offset), so a theme
 * that also carries a plain second hand draws BOTH — the hand as layer 2 and
 * the animated carriage directly above it, the pair a real tourbillon bridge
 * reads. Before this ruling the bank was the seconds layer (the .h's comment
 * still says so at the tourbillon array); that swap is gone, and the shipped
 * av-kit pair — tourbillon_n 0 — draws byte-identically either way. */
static lv_obj_t       *s_car;               /* the carriage object, NULL = none */
static lv_image_dsc_t  s_car_dsc;           /* its descriptor until the first swap */
static int32_t         s_car_start_10;      /* start orientation, tenths of a degree */
/* C4's consumed parent column: the carriage row's element index + 1 (0 = the
 * table holds no carriage row). The projection put the parent's ROW INDEX in
 * clk_aux_t.parent_row; this is the one figure needed to recognise it. A needle
 * whose parent_row equals this rides the carriage's rotation — see aux_update. */
static uint16_t        s_car_row_1;

/* Orbit marks (sweep perf). The generic hand marking would take the union
 * of the two rotated bounding boxes of the carriage (~260x350 swung around
 * the dial centre): most of the page on EVERY tick, so the commit hook
 * restores + re-composites ~330k px of dial+glass for a carriage that only
 * touches ~75k of them — that is what held sweep at 5 fps once the DMA
 * itself was gated away. Instead each sweep tick marks the rotated ink
 * footprint of both poses: every source row's ink extent (x0,x1) is kept
 * per frame (PSRAM chords, built on first use), rotated as a line segment
 * with 16.16 fixed-point trig, and accumulated into per-device-row x-spans
 * — marks are as wide as the ink and no wider. The commit band's flush gate
 * (orb_gfx.c, fed by the scheduler's table commit) then skips DMA for rows
 * whose rebuild came out byte-identical anyway. If the chord tables cannot be built the caller
 * falls back to the swept-box union. */
static uint16_t *s_tourb_cx0[CLK_TOURB_MAX];
static uint16_t *s_tourb_cx1[CLK_TOURB_MAX];

/* One stamp pass's device-row span accumulation (x1 < x0 = untouched row).
 * 2026-10-24: PSRAM, lazy in tourbillon_ink_mark — 2×932 B of .bss here sat
 * on the heap boundary inside the one contiguous block httpd_start needs.
 * The stamp pass already reads the chord tables from PSRAM (s_tourb_cx0/1),
 * so this rides the same bus at the same rate; a failed alloc returns false,
 * the same answer the chord model gives when it cannot build. */
static int16_t *s_wp_x0, *s_wp_x1;
static int32_t  s_wp_lo, s_wp_hi;



static bool     s_warned_screen = false;

/* Mapped base of the art bank, filled in by clock_view_init() (NULL until then).
 * Kept as a file static rather than calling art_bank_ptr() on every sprite read:
 * blob() is on the per-frame paint path and this keeps it a single load. */
static const uint8_t *s_art;

/* Every pixel of the dial comes from here: the bank now lives in the `assets`
 * flash partition and is mapped once at boot by art_bank_init() below, so this
 * stays a bare pointer return and sprite reads cost exactly what they did when
 * the bank was linked into .rodata. See main/art/art_bank.h. */
static const uint8_t *blob(void) { return s_art; }

/* The tick's policy callback (below) reads the time through it; defined with the
 * compose block further down, like tick_base_ms() used to be declared for
 * tick_timer_cb. */
static void compose_now(void);
static void clock_layers_refresh(void);








/* Map an orb-os blend (0 normal, 1 multiply, 2 screen) onto LVGL's image blend. */
static lv_blend_mode_t map_blend(uint8_t b)
{
    switch (b) {
        case 1:  return LV_BLEND_MODE_MULTIPLY;
        case 2:
            if (!s_warned_screen) {
                ESP_LOGW(TAG, "blend==2 (screen) has no LVGL image mode; using ADDITIVE");
                s_warned_screen = true;
            }
            return LV_BLEND_MODE_ADDITIVE;
        default: return LV_BLEND_MODE_NORMAL;
    }
}



/* Create one rotated-hand/shadow layer from a sprite record. The object is
 * placed so its stored pivot (in cropped coords) lands on the sprite's screen
 * centre; LVGL then rotates it about that pivot. */
static lv_obj_t *make_layer(const clk_sprite_t *spr, lv_image_dsc_t *dsc)
{
    if (!gfx_sprite_drawable(spr)) return NULL;

    gfx_fill_sprite_dsc(dsc, spr, blob());

    lv_obj_t *o = lv_image_create(s_scr);
    lv_obj_set_size(o, spr->w, spr->h);
    lv_obj_set_align(o, LV_ALIGN_TOP_LEFT);
    lv_obj_set_scrollable(o, false);
    lv_image_set_src(o, dsc);
    lv_image_set_pivot(o, spr->pivotx, spr->pivoty);
    lv_obj_set_pos(o, spr->centerx - spr->pivotx, spr->centery - spr->pivoty);
    lv_image_set_blend_mode(o, map_blend(spr->blend));
    /* LVGL defaults rotation to 4-tap bilinear (antialias=1): ~4x the
     * per-px cost of the nearest path on the S3, and it dominates the
     * hands-over-animated-orb re-composite (~40 ms/frame). The baked ink
     * already carries its own AA in the A8 plane, so nearest sampling of
     * the rotated source looks near-identical at these sprite widths. */
    lv_image_set_antialias(o, false);
    return o;
}

/* Ticks per hand beat in GIF mode: theme tickRate (beats/s) against the
 * beat the tick actually delivers. 4 bps on an 83 ms tick -> 3 ticks. */
static uint32_t hand_beat_ticks(void)
{
    int rate = clk_theme.tick_rate;
    if (rate < 1) rate = 1;
    const uint32_t t = orb_bg_gif_tick_ms();
    const uint32_t n = (uint32_t)((1000.0 / (double)rate) / (double)t + 0.5);
    return n < 1 ? 1 : n;
}

/* Seconds 0..60 (fractional while sweeping), minutes 0..60 (fractional unless a
 * railway dial sits it on a mark), hour 0..12 fractional. Mirrors orb-os
 * compose_custom: minute_hand_mins() and the p_min/p_hr derivation. */
static void hand_angles(double wall_frac, int hour, int min, int sec,
                        float ang[5])
{
    const bool sweep   = clk_theme.second_sweep;
    const bool railway = clk_theme.second_railway;

    /* GIF-mode hand cadence: the tick delivers exactly one GIF frame per tick,
     * so the second hand's beat rides the TICK COUNTER, not a wall-clock grid.
     * The theme's tickRate (beats/s) becomes ticks-per-beat (hand_beat_ticks):
     * 4 bps on a 12 fps tick = advance every 3rd tick, always the 3rd — an
     * evenly spaced beat even though 250 ms is not an exact multiple of the
     * ~83 ms the pipe actually delivers. Quantising the wall clock instead
     * would stretch or squash beats by one tick every time the phases slip.
     * On each beat the angle is sampled from the wall clock, so the hand
     * reads true time (position error <= one beat, re-anchored at every
     * second boundary); between beats the angle holds, apply_angles' eps
     * gate skips the re-pose, and the tick's commit carries GIF rows only.
     * tpb <= 1 means the tick is at or below the beat: a true sweep, wall
     * sampled every tick. Non-GIF themes are untouched — there the tick
     * already runs at tickRate and sampling once per tick IS the beat. */
    if (sweep && clk_theme.tick_rate > 0 && orb_bg_gif_mode()) {
        const uint32_t tpb = hand_beat_ticks();
        if (tpb > 1) {
            if (sec != s_bt_sec) {          /* true seconds re-anchor the phase */
                s_bt_sec = sec;
                s_bt_ctr = 0;
            }
            if ((s_bt_ctr % tpb) == 0) s_bt_frac = wall_frac;
            s_bt_ctr++;
            wall_frac = s_bt_frac;
        }
    }

    /* The second hand's beat, from the theme's own motion words. A sweep reads
     * the wall clock's fraction whole (subdivided above when the tick runs
     * faster than the beat).  A TICK beats at tick_rate steps per second: 4/s
     * quantises the wall fraction into quarter-steps, so a bank-rate tick
     * shows that beat even between wall-second changes, and a 1/s or classic
     * theme (tick_rate 0/1) stays the integer step it always was.  A ticking
     * hand over a delta plate strobes — its box repaints between the bank's
     * row pushes — which is exactly the cost the composer's Motion choice now
     * accepts instead of silently overruling it. */
    float secF;
    if (sweep)
        secF = (float)sec + (float)wall_frac;
    else if (clk_theme.tick_rate > 1)
        secF = (float)sec + (float)((int)(wall_frac * (float)clk_theme.tick_rate))
                           / (float)clk_theme.tick_rate;
    else
        secF = (float)sec;
    float minF = (railway && sweep) ? (float)min
                                    : (float)min + secF / 60.0f;
    float hrF  = (float)(hour % 12) + minF / 60.0f;

    ang[0] = hrF  * 30.0f;   /* hour   : 30 deg/hr  */
    ang[1] = minF *  6.0f;   /* minute :  6 deg/min */
    ang[2] = secF *  6.0f;   /* second :  6 deg/sec */
    ang[3] = 0.0f;
    ang[4] = 0.0f;
}


/* Last angle pushed onto each hand layer, so an unchanged hand is a no-op and
 * so the outgoing wedge is known when a hand does move. INT_MIN = never set. */
static int32_t s_ang_10[5] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };

/* Push the current angles onto the live widgets. lv_image_set_rotation returns
 * early when the 0.1-degree value is unchanged, so at a 1 Hz tick only the hand
 * that actually moved (the second, for a ticking dial) dirties its area — the
 * hour/minute are re-pose no-ops and the plate is never touched.
 *
 * HARD GATE for sweeping dials: re-posing a hand invalidates its FULL rotated
 * bounding box (tens of thousands of px; measured 38 ms/frame while all three
 * hands re-posed every 40 ms tick). While sweeping, the minute/hour angles
 * advance sub-pixel amounts per tick (minute = 0.0002 deg/tick at 25 fps —
 * ~0.00007 px of tip motion), so re-posing them every tick is pure waste:
 * only re-pose a layer when its angle left the drawn one by >= 0.2 deg
 * (~0.4 px at the longest tip — at or below visibility). The second hand
 * sweeps 1.44 deg/tick so it always passes; the minute hand re-poses every
 * ~2 s and the hour every few minutes, catching up exactly.
 *
 * The invalidations lv_image_set_rotation() issues for itself are the whole
 * (previously unfixable) cost: it queues the FULL transformed box of BOTH the
 * old and the new pose (measured: hand+shadow re-blit = ~39 ms/frame of
 * per-pixel inverse-rotate + RGB565A8 blend with LV_DRAW_SW_ASM_NONE), which
 * re-rasterizes ~2x18.6k px for a wedge that is a few hundred px wide. Wrap
 * the setters in lv_display_enable_invalidation(false/true) — the same nesting
 * counter lv_image.c itself uses around lv_obj_refresh_ext_draw_size — and
 * push only the swept union ourselves. The software renderer then clips the
 * rotated blit to that refresh area, so cost scales with the wedge, not the
 * box. */
/* Sweep marking by the carriage's rotated ink chords; defined with the
 * tourbillon block below. Returns true when it placed the marks for both
 * poses, false when the caller must fall back to the swept-box union (no
 * bank, no prior composite, shadowed seconds, or classic path). */
static bool tourbillon_ink_mark(int f, int32_t prev_ang_10, int32_t new_ang_10);
static void tourb_mark(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out);

#define HAND_REDRAW_EPS_10 2
static void apply_angles(const float ang[5])
{
    lv_display_t * disp = lv_obj_get_display(s_scr);

    for (int i = 0; i < 5; i++) {
        const int32_t a = (int32_t)lroundf(ang[i] * 10.0f);
        if (a == s_ang_10[i]) continue;
        if (s_ang_10[i] != INT32_MIN &&
            abs((int)(a - s_ang_10[i])) < HAND_REDRAW_EPS_10) continue;
        if (!s_hand[i] && !s_shadow[i] && !(i == 2 && s_car)) { s_ang_10[i] = a; continue; }

        const int32_t prev = s_ang_10[i];
        lv_area_t u;
        bool have = false;
        /* The carriage turns with the seconds layer plus its own start
         * orientation; every mark and setter below uses whichever angle the
         * layer's pixels actually stand at. */
        const bool car = (i == 2 && s_car);
        const int32_t c_prev = car && prev != INT32_MIN ? prev + s_car_start_10 : prev;
        const int32_t c_a    = car ? a + s_car_start_10 : a;

        /* Sweep tourbillon: the orbit rotates the whole carriage, so the
         * box union below is most of the page on EVERY tick and the commit
         * hook re-composites the full dial+glass across it (measured: sweep
         * ran at half tick's frame rate, the whole gap in those marks).
         * A carriage with no needle and no shadow sharing its angle is the
         * case the chord model was built for: mark the two poses' rotated
         * ink instead — each source row's ink extent rotated as a segment,
         * accumulated per device row — so the rebuild is confined to what
         * the carriage actually covered or vacated. The frame swap's
         * changed pixels are added by tourbillon_advance's diff-box marks.
         * Anything else sharing this angle still owes its own wedge, so the
         * box path runs whenever the chords are not the whole story. */
        const bool car_alone = car && !s_hand[2] && !s_shadow[2];
        const bool inked = car_alone && prev != INT32_MIN &&
                           tourbillon_ink_mark((int)s_tourb_shown, c_prev, c_a);

        if (!inked && prev != INT32_MIN) {
            /* Union of the old and new wedge of every layer sharing this angle,
             * in both their un-shifted (hand) and shifted (shadow) positions.
             * A standalone carriage contributes its own rotated box at the
             * carriage angle, not the hand's at the plain seconds angle. */
            if (car) {
                const clk_sprite_t *cs = &clk_theme.tourbillon[
                    s_tourb_shown >= 0 ? s_tourb_shown : 0];
                tourb_mark(cs, c_prev, &u);
                lv_area_t n; tourb_mark(cs, c_a, &n);
                gfx_area_union(&u, &n);
                have = true;
            }
            if (s_hand[i]) {
                lv_area_t w0, w1;
                gfx_swept_box(&clk_theme.hand[i],   prev, &w0);
                gfx_swept_box(&clk_theme.hand[i],   a,    &w1);
                gfx_area_union(&w0, &w1);
                if (have) gfx_area_union(&u, &w0);
                else { u = w0; have = true; }
            }
            if (s_shadow[i]) {
                lv_area_t su; gfx_swept_box(&clk_theme.shadow[i], prev, &su);
                lv_area_t sn; gfx_swept_box(&clk_theme.shadow[i], a, &sn);
                gfx_area_union(&su, &sn);
                if (have) gfx_area_union(&u, &su);
                else { u = su; have = true; }
            }
        }
        else if (!inked && (s_hand[i] || car)) {
            if (car) {
                const clk_sprite_t *cs = &clk_theme.tourbillon[
                    s_tourb_shown >= 0 ? s_tourb_shown : 0];
                tourb_mark(cs, c_a, &u);
            } else {
                gfx_swept_box(&clk_theme.hand[i], a, &u);
            }
            if (s_shadow[i]) {
                lv_area_t sn; gfx_swept_box(&clk_theme.shadow[i], a, &sn);
                gfx_area_union(&u, &sn);
            }
            have = true;
        }

        /* Under L4 the commit painter owns these rows outright: restoring the
         * background from the canvas and re-painting every layer over them is
         * both cheaper and more correct than asking LVGL to re-composite the
         * rotated boxes (which was the 36-48 ms we are replacing), so it does
         * not invalidate them and only the painter runs. Without a canvas the
         * classic path is back in charge and the wedge goes to LVGL. */
        if (have) {
#if CLK_BG_DIRECT_PUSH
            if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
            else
#endif
                lv_obj_invalidate_area(s_scr, &u);
        }

        if (prev == INT32_MIN) {
            /* First pose ever: nothing painted yet, the setter's own
             * (full-box) invalidation is the one that paints it. */
            if (s_hand[i])   lv_image_set_rotation(s_hand[i],   a);
            if (s_shadow[i]) lv_image_set_rotation(s_shadow[i], a);
            if (car)         lv_image_set_rotation(s_car,       c_a);
        }
        else {
            lv_display_enable_invalidation(disp, false);
            if (s_hand[i])   lv_image_set_rotation(s_hand[i],   a);
            if (s_shadow[i]) lv_image_set_rotation(s_shadow[i], a);
            if (car)         lv_image_set_rotation(s_car,       c_a);
            lv_display_enable_invalidation(disp, true);
            /* The setter early-returns without applying its angle if it
             * already equals the widget's — impossible here since s_ang_10 is
             * only updated together with the setters, but re-mark the swept
             * box once more so a lost invalidation can never stick. */
            if (have) {
#if CLK_BG_DIRECT_PUSH
                if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
                else
#endif
                    lv_obj_invalidate_area(s_scr, &u);
            }
        }
        s_ang_10[i] = a;
    }
}

/* ---- tourbillon: pick the wall-clock frame and swap it in -----------------
 * The only pixels that can differ after a frame swap are the changed rects of
 * the OUTGOING and INCOMING frame at the current angle (both are chained
 * against neighbours by the baker, like the delta bank); a first-ever draw has
 * no prior composite of this layer, so it takes the full swept box. The angle
 * changes (the orbit step) are marked by apply_angles itself, exactly as for
 * any other hand. */
static void tourb_mark(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out)
{
    if (spr->diffw && spr->diffh)
        gfx_swept_rect_box(spr, ang_10, spr->diffx, spr->diffy,
                       (int32_t)spr->diffx + (int32_t)spr->diffw - 1,
                       (int32_t)spr->diffy + (int32_t)spr->diffh - 1, out);
    else
        gfx_swept_box(spr, ang_10 == INT32_MIN ? 0 : ang_10, out);
}

/* Build frame f's per-source-row ink chords (min/max x with alpha>0), once.
 * Sequential alpha-plane scan (~90 kB from PSRAM per frame), lazy so a theme
 * only pays for the frames it shows. Returns false = no table: box fallback. */
static bool tourb_chords(int f)
{
    if (f < 0 || f >= (int)clk_theme.tourbillon_n || f >= CLK_TOURB_MAX)
        return false;
    if (s_tourb_cx0[f]) return true;
    const clk_sprite_t *s = &clk_theme.tourbillon[f];
    if (!gfx_sprite_drawable(s) || !s->w || !s->h) return false;

    const size_t sz = (size_t)s->h * sizeof(uint16_t);
    uint16_t *c0 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *c1 = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c0 || !c1) {
        if (c0) heap_caps_free(c0);
        if (c1) heap_caps_free(c1);
        return false;
    }
    const uint8_t *alp = blob() + s->color_off + (size_t)s->w * s->h * 2;
    for (int32_t y = 0; y < (int32_t)s->h; y++) {
        const uint8_t *row = alp + (size_t)y * s->w;
        int32_t lo = -1, hi = -1;
        for (int32_t x = 0; x < (int32_t)s->w; x++)
            if (row[x]) { lo = x; break; }
        for (int32_t x = (int32_t)s->w - 1; x >= 0; x--)
            if (row[x]) { hi = x; break; }
        c0[y] = (uint16_t)(lo < 0 ? 1 : lo);   /* empty row: c1 < c0 */
        c1[y] = (uint16_t)(hi < 0 ? 0 : hi);
    }
    s_tourb_cx0[f] = c0;
    s_tourb_cx1[f] = c1;
    return true;
}

/* Rotate frame f's source-row chords by ang_10 (TENTHS of a degree, the
 * gfx_swept_rect_box convention) and accumulate the device rows each segment
 * passes through into the s_wp_* spans. All-integer inner loop — the earlier
 * double-trig, gfx_mark_rows-per-segment version cost ~90k calls per tick at the
 * worst carriage angle and starved the tick task to 1-2 fps with an
 * angle-periodic wave; the same work done here is tens of thousands of ALU
 * ops into static memory.
 *
 * The forward rotation reuses paint_layer's exact trig pair (gfx_rot_sin_cos),
 * operation for operation, so the stamped pose cannot disagree with what the
 * commit paints: any residual is nearest-neighbour texel spread (±1 px/row),
 * inside the pads. */
static void tourb_stamp_pose(const clk_sprite_t *spr, int f, int32_t ang_10)
{
    const uint16_t *c0 = s_tourb_cx0[f], *c1 = s_tourb_cx1[f];
    /* Forward map = transpose of paint_layer's inverse map: sin(+a) =
     * -sinma(-a), cos(+a) = cosma(-a). The products below land in a >> 16,
     * so the pair must be sin/cos in 16.16 (1.0 == 65536): sinma is LVGL's
     * /1024 value, hence *64. (The first builds used gfx_trigo_sin's /32768
     * table — or sinma*32 — straight into the >>16 form: the stamped pose
     * sat at HALF radius, so the carriage's outer ring never got marked,
     * never got restored or repainted, and ghosted through the whole orbit
     * while the double-math diff boxes rebuilt a crisp rectangle around the
     * escapement against it — the exact three reported artifacts. LVGL's
     * own consumers pair lv_trigo_sin with >> 15, not >> 16.) */
    int32_t sma, cma;
    gfx_rot_sin_cos(ang_10, &sma, &cma);
    const int32_t si = -sma * 64, co = cma * 64;
    const int32_t pvx = (int32_t)spr->pivotx, pvy = (int32_t)spr->pivoty;
    const int32_t cxs = (int32_t)spr->centerx, cys = (int32_t)spr->centery;
    const int32_t rpad = 1 + ((int32_t)spr->w >> 7);   /* texel spread at row ends */

    for (int32_t ys = 0; ys < (int32_t)spr->h; ys++) {
        if (c1[ys] < c0[ys]) continue;                /* row carries no ink */
        const int32_t dy  = ys - pvy;
        const int32_t dxa = (int32_t)c0[ys] - pvx;
        const int32_t dxb = (int32_t)c1[ys] - pvx;
        const int32_t ax = cxs + ((co*dxa - si*dy) >> 16);
        const int32_t ay = cys + ((si*dxa + co*dy) >> 16);
        const int32_t bx = cxs + ((co*dxb - si*dy) >> 16);
        const int32_t by = cys + ((si*dxb + co*dy) >> 16);

        const bool up = (ay <= by);
        const int32_t ya  = up ? ay : by,  yb  = up ? by : ay;
        const int32_t xya = up ? ax : bx,  xyb = up ? bx : ax;
        const int32_t den = yb - ya;
        const int32_t y0 = ya - rpad, y1 = yb + rpad;

        const int32_t adx = xyb - xya >= 0 ? xyb - xya : xya - xyb;
        int32_t x16, step16;
        if (den == 0 || adx > den) {
            /* Shallow segment (horizontal included): the source row's band
             * lands across the FULL chord width on essentially the same
             * device rows, so the row-walk — one ~7 px window per row —
             * would mark a sliver of it and ghost the rest (this is what
             * smeared the counterweight tail whenever the carriage rode near
             * 12 o'clock). Mark the full span on every row the segment
             * (plus its texel pad) touches: the pad rows beyond the ends
             * still catch corner-round-off ink several px outside an
             * endpoint window — the sim rejects anything narrower. */
            const int32_t xl = (xya < xyb ? xya : xyb) - SWEPT_PAD;
            const int32_t xr = (xya < xyb ? xyb : xya) + SWEPT_PAD;
            for (int32_t y = y0; y <= y1; y++) {
                if (y < 0 || y >= CLK_SCR_H) continue;
                if ((int16_t)xl < s_wp_x0[y]) s_wp_x0[y] = (int16_t)xl;
                if ((int16_t)xr > s_wp_x1[y]) s_wp_x1[y] = (int16_t)xr;
                if (y < s_wp_lo) s_wp_lo = y;
                if (y > s_wp_hi) s_wp_hi = y;
            }
            continue;
        }
        step16 = ((xyb - xya) << 16) / den;          /* x travel per device row */
        x16 = (xya << 16) - step16 * rpad;           /* back up to the first pad row */
        for (int32_t y = y0; y <= y1; y++) {
            const int32_t q0 = x16 >> 16, q1 = (x16 + step16) >> 16;
            x16 += step16;
            if (y < 0 || y >= CLK_SCR_H) continue;
            const int32_t xl = (q0 < q1 ? q0 : q1) - SWEPT_PAD;
            const int32_t xr = (q0 < q1 ? q1 : q0) + SWEPT_PAD;
            if ((int16_t)xl < s_wp_x0[y]) s_wp_x0[y] = (int16_t)xl;
            if ((int16_t)xr > s_wp_x1[y]) s_wp_x1[y] = (int16_t)xr;
            if (y < s_wp_lo) s_wp_lo = y;
            if (y > s_wp_hi) s_wp_hi = y;
        }
    }
}

/* The sweeping carriage's mark for one tick: the union of the poses' rotated
 * ink for frame f, released in <= 466 gfx_mark_rows calls. prev_ang_10 ==
 * INT32_MIN stamps only the new pose (the frame swap calls it that way: the
 * old frame's ink was already marked at this angle by its own moves, and the
 * INCOMING frame's ink — the balance at its new position — is what the baked
 * diff rect can fall a few pixels short of, which showed up as a visible
 * rectangle around the escapement). The rest of the frame swap's own pixels
 * are still marked by tourbillon_advance's diff boxes; chords plus those
 * boxes cover every pixel that can differ on the glass — everything the old
 * full-box union touched beyond them was dial and glass re-blended onto
 * identical output. */
static bool tourbillon_ink_mark(int f, int32_t prev_ang_10, int32_t new_ang_10)
{
#if CLK_BG_DIRECT_PUSH
    if (!orb_sched_l4_active()) return false;
#endif
    /* The chord model covers the carriage's own art. It used to decline on a
     * shadowed second layer because the bank WAS that layer and its shadow
     * moved with it; since ruling C the shadow belongs to the stationary
     * needle, so a moving frame leaves it untouched and the chords stand. */
    if (clk_theme.tourbillon_n <= 0) return false;
    if (f < 0 || f >= (int)clk_theme.tourbillon_n || f >= CLK_TOURB_MAX)
        return false;                          /* no composite on the glass yet */
    if (!tourb_chords(f)) return false;        /* fall back to the box union */
    if (!s_wp_x0) {
        s_wp_x0 = heap_caps_malloc(sizeof *s_wp_x0 * CLK_SCR_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_wp_x1 = heap_caps_malloc(sizeof *s_wp_x1 * CLK_SCR_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_wp_x0 || !s_wp_x1) {
            heap_caps_free(s_wp_x0); s_wp_x0 = NULL;   /* the chord fallback's own
                                                        * shape: box union paints */
            s_wp_x1 = NULL;
            return false;
        }
    }

    for (int32_t y = 0; y < CLK_SCR_H; y++) {          /* empty spans */
        s_wp_x0[y] = 0x7fff;
        s_wp_x1[y] = (int16_t)-0x8000;
    }
    s_wp_lo = CLK_SCR_H; s_wp_hi = -1;
    if (prev_ang_10 != INT32_MIN)
        tourb_stamp_pose(&clk_theme.tourbillon[f], f, prev_ang_10);
    tourb_stamp_pose(&clk_theme.tourbillon[f], f, new_ang_10);
    if (s_wp_hi < s_wp_lo) return true;        /* zero-ink bank: nothing to mark */

    for (int32_t y = s_wp_lo; y <= s_wp_hi; y++)
        if (s_wp_x1[y] >= s_wp_x0[y])
            gfx_mark_rows(y, y, s_wp_x0[y], s_wp_x1[y], false);
    return true;
}

static void tourbillon_advance(void)
{
    const int n = (int)clk_theme.tourbillon_n;
    if (n <= 0 || !s_car) return;

    const uint32_t fps = clk_theme.tourbillon_fps ? clk_theme.tourbillon_fps : 15;
    const int f = (int)(((uint64_t)(lv_tick_get() - s_tourb_t0) * fps)
                        / 1000u % (uint32_t)n);
    if (f == s_tourb_shown) return;

    /* The carriage stands at the seconds angle plus its start orientation —
     * the pose apply_angles just set on it, whatever that was when the frame
     * clock started and between its ticks. */
    const int32_t a = s_ang_10[2] == INT32_MIN ? INT32_MIN : s_ang_10[2] + s_car_start_10;
    lv_area_t u;
    if (s_tourb_shown < 0 || a == INT32_MIN) {
        /* First draw of this layer: no prior composite exists to diff against,
         * so the whole (rotation-0 if not posed yet) swept box goes dirty. */
        gfx_swept_box(&clk_theme.tourbillon[f], a == INT32_MIN ? 0 : a, &u);
    } else {
        tourb_mark(&clk_theme.tourbillon[s_tourb_shown], a, &u);
        lv_area_t v;
        tourb_mark(&clk_theme.tourbillon[f], a, &v);
        gfx_area_union(&u, &v);
    }

    /* Mirror apply_angles' etiquette: the src setter would invalidate the
     * widget's whole transformed box; the marks below cover exactly what
     * needs rebuilding. Under L4 the commit painter owns the rows outright. */
    lv_display_t *disp = lv_obj_get_display(s_scr);
    lv_display_enable_invalidation(disp, false);
    lv_image_set_src(s_car, &s_tourb_dsc[f]);
    lv_display_enable_invalidation(disp, true);
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
    else
#endif
        lv_obj_invalidate_area(s_scr, &u);
    /* The INCOMING frame's own ink at this angle, marked by chords too: the
     * baked diff rect is an axis-aligned box that a sub-degree rotation can
     * push a few pixels of balance ink outside — that seam was the visible
     * rectangle around the escapement. The stamps cover the exact rotated
     * ink; the diff-box mark above stays as the belt to the chords' braces
     * (and is what paints when the chord tables cannot be built). */
    if (s_tourb_shown >= 0 && a != INT32_MIN)
        tourbillon_ink_mark(f, INT32_MIN, a);
    s_tourb_shown = f;
}


#if CLK_BG_DIRECT_PUSH
/* ---- L4: direct layer compositing into the panel mirror -------------------
 * With L3 the raw GIF pixels land in the mirror instantly, but everything
 * that PAINTS OVER them (hands, shadows, glass overlay) still had to come
 * back through LVGL: measured 36-48 ms/frame of render~, because for every
 * pushed row LVGL re-rasterized each overlapping layer's FULL rotated box
 * through its generic transform+blend pipeline (~500+ cyc/px even with the
 * widget's antialiasing off, and shrinking the invalidation does not shrink
 * the rotated scan).
 *
 * So skip LVGL for the re-composite entirely: the tick marks the rows its
 * push dirtied (plus the wedges of any hand that moved), and at commit --
 * after every chunk is in the mirror, before the bands DMA it out -- we
 * paint each affected layer straight into the mirror. The maths below copies
 * lv_draw_sw_transform()'s ROTATION-ONLY, ANTI-ALIAS-OFF nearest-neighbour
 * path (and lv_color_16_16_mix_inlined / rgb565_image_blend for the blend)
 * operation for operation, so our pixels are bit-identical to what LVGL
 * draws for the first paint and for the wedges it still renders itself.
 * Cost is the clipped box with two early-outs (source bounds, alpha==0):
 * the needles are mostly empty box, so the mask skip drops ~3/4 of it. */
/* The per-tick dial policy the scheduler drives: read the time, pose the hands
 * off it, and step the tourbillon's frame bank. Everything else the tick used to
 * do — the background step, the marks, the commit nudge, the deadline ladder, the
 * throughput report — is the scheduler's. Kept as ONE callback rather than two
 * because the pair has no meaning apart: the frame bank advances against the
 * angle compose_now just settled on, and a tick that ran one without the other
 * would composite last second's carriage at this second's angle. */
static void clock_tick_policy(lv_timer_t *timer)
{
    (void)timer;
    compose_now();
    tourbillon_advance();          /* no-op unless the theme has a frame bank */
    /* Then the app's side of the frame: every row of this dial's layer table
     * takes the pose the two passes above just settled (Migration 2, Commit 4).
     * Damage already sits in the row bank — the mark sites run inside them,
     * and each marks both the pose it vacates and the one it takes. The
     * scheduler's flush hook welds whatever this pass changed, so the panel
     * cannot catch a half-painted dial: the marks and the table agree before
     * the first restore row does. */
    clock_layers_refresh();
}








#if DISPLAY_BENCH
/* ---- Synthetic live layer, to price a blended pixel --------------------------
 * docs/fps-budget-benchmark-plan.md Phase 2. DISPLAY_BENCH_PX solid 60x60
 * blocks (3600 px each, so 18/36/72/112 add the planned 65k/130k/259k/403k
 * px/tick) are painted through the very gfx_blend_px() the real layers use, with
 * the colour plane in PSRAM like a real sprite. Positions are chosen at tick
 * time and consumed by the hook at commit, exactly like a hand's swept box,
 * and each block walks every tick so the flush gate cannot settle the ink out
 * of the DMA — CAD bands/px are the proof it shipped.
 *
 * Seed and wrap INSIDE the band: a block starting at y=0 would mark rows the
 * art never touches, turning a px sweep into a px+rows sweep. Rows to know:
 * the shipped commit DMA's ~265 full-width rows, and the GIF bank's 208-row
 * union is NOT what sets that — the carriage's rotating diff box forces ~255
 * on its own. Pinning rows therefore means keeping the ink inside that band,
 * and DISPLAY_BENCH_SPREAD=1 is the deliberate way out, used to price the row
 * term alone. Measured numbers: docs/fps-budget-findings-2026-10-10.md. */
#define BENCH_BLK        60
#define BENCH_MAX_BLK    128
#define BENCH_BAND_Y0    128
#define BENCH_BAND_ROWS  208
static uint16_t *s_bench_solid;                       /* PSRAM colour plane   */
static int16_t   s_bench_x[BENCH_MAX_BLK], s_bench_y[BENCH_MAX_BLK];
static uint32_t  s_bench_n;                           /* blocks this tick     */
/* The ink count lives in the scheduler (orb_sched_ink_px_add), which is the TU
 * that reports it next to the flush numbers it has to agree with. */

static void bench_paint_band(int32_t y0, int nr);   /* gfx band hook, below */

#if ORB_ART_M3_PROBE
/* M3 graft: point the blocks at the bank the M3 probe left resident instead
 * of the synthetic mag plane — same px count, same blend, the ONLY variable
 * is where the colours live (flash MMU map on arm A, PSRAM arena on arm B).
 * Offsets re-stripe every tick in bench_plan so each pass reads every page of
 * the bank; a bench that reread one cache line proves nothing. If the plane
 * never arrived the bench says so and stays dark — mag ink under an "M3"
 * label would be the confident lie the probe already refuses to print. */
static size_t  s_m3_off[BENCH_MAX_BLK];
static size_t  s_m3_bytes;
static unsigned s_m3_pass;

static bool m3_arm_plane(void)
{
    s_m3_bytes = art_bank_m3_bytes();
    const uint8_t *plane = art_bank_m3_plane();
    const size_t need = (size_t)BENCH_MAX_BLK * BENCH_BLK * BENCH_BLK * 2u;
    if (!plane || s_m3_bytes < need + (size_t)BENCH_BLK * BENCH_BLK * 2u) {
        ESP_LOGE(TAG, "bench M3: no bench plane (bank %u B, need >%u B) — NO blocks will "
                      "paint; read the m3: lines above for why",
                 (unsigned)s_m3_bytes, (unsigned)need);
        return false;
    }
    s_bench_solid = (uint16_t *)(void *)(uintptr_t)plane;   /* read-only from here */
    ESP_LOGI(TAG, "bench M3: colour source := %s bank, %u B — the reads ARE the measurement",
             art_bank_m3_is_psram() ? "PSRAM arena" : "flash-map", (unsigned)s_m3_bytes);
    return true;
}
#endif /* ORB_ART_M3_PROBE */

static void bench_init(void)
{
    if (DISPLAY_BENCH_PX == 0 || s_bench_solid) return;
    /* The commit loop moved to gfx; the synthetic ink still has to ride inside
     * its timed paint window, so it is registered as the band hook rather than
     * called from the hook that no longer exists here. */
    gfx_set_band_hook(bench_paint_band);
#if ORB_ART_M3_PROBE
    if (!m3_arm_plane()) return;
#else
    const size_t n = (size_t)BENCH_BLK * BENCH_BLK * sizeof(uint16_t);
    s_bench_solid = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bench_solid) return;
    uint16_t *p = s_bench_solid;
    for (size_t i = 0; i < n / sizeof(uint16_t); i++) p[i] = 0xF81F;   /* mag */
#endif
    /* Seed inside the band: a block that starts at y=0 marks rows the GIF
     * never touches, and the first ~60 ticks would then measure a row change
     * as well as a px change -- exactly the confound this sweep must not have. */
    for (uint32_t i = 0; i < DISPLAY_BENCH_PX; i++) {
        s_bench_x[i] = (int16_t)((i * 61) % (CLK_SCR_W - BENCH_BLK));
#if DISPLAY_BENCH_SPREAD
        s_bench_y[i] = (int16_t)((i * 37) % (CLK_SCR_H - BENCH_BLK));
#else
        s_bench_y[i] = (int16_t)(BENCH_BAND_Y0
                                 + (i * 37) % (BENCH_BAND_ROWS - BENCH_BLK));
#endif
    }
    ESP_LOGI(TAG, "bench: %u blocks of %dx%d = %u px/tick, alpha %u",
             (unsigned)DISPLAY_BENCH_PX, BENCH_BLK, BENCH_BLK,
             (unsigned)(DISPLAY_BENCH_PX * BENCH_BLK * BENCH_BLK),
             (unsigned)DISPLAY_BENCH_ALPHA);
}

/* Tick side: walk the blocks and mark their rows through the hand's own path,
 * so the hook restores+repaints exactly the pixels the ink will land on. Both
 * axes wrap INSIDE the band, so the marked row set stays a subset of the
 * shipped GIF union and CAD bands/px report the same rows at every point. */
static void bench_plan(void)
{
    if (!s_bench_solid) return;
    s_bench_n = DISPLAY_BENCH_PX < BENCH_MAX_BLK ? DISPLAY_BENCH_PX : BENCH_MAX_BLK;
    for (uint32_t i = 0; i < s_bench_n; i++) {
        int32_t x = s_bench_x[i] + 7;
        int32_t y = s_bench_y[i] + 5;
        if (x + BENCH_BLK > CLK_SCR_W) x = (int32_t)((i * 29) % (CLK_SCR_W - BENCH_BLK));
#if DISPLAY_BENCH_SPREAD
        if (y + BENCH_BLK > CLK_SCR_H) y = (int32_t)((i * 13) % (CLK_SCR_H - BENCH_BLK));
#else
        if (y + BENCH_BLK > BENCH_BAND_Y0 + BENCH_BAND_ROWS)
            y = BENCH_BAND_Y0 + (int32_t)((i * 13) % (BENCH_BAND_ROWS - BENCH_BLK));
#endif
        s_bench_x[i] = (int16_t)x;
        s_bench_y[i] = (int16_t)y;
        gfx_mark_rows(y, y + BENCH_BLK - 1, x, x + BENCH_BLK - 1, false);
    }
#if ORB_ART_M3_PROBE
    /* Re-stripe the colour-source windows so every pass sweeps the WHOLE bank:
     * one block per band of it, rotated by the pass counter — no block pair
     * shares a source page, and a tick that read only the cache would show up
     * as a zero row-cost difference between arms. This is the M3 variable:
     * identical blends, different source location. */
    if (s_bench_n && s_m3_bytes) {
        const size_t win  = (size_t)BENCH_BLK * BENCH_BLK * 2u;
        const size_t band = s_m3_bytes / s_bench_n;
        const size_t span = s_m3_bytes - win;
        for (uint32_t i = 0; i < s_bench_n; i++)
            s_m3_off[i] = (((size_t)i * band + (size_t)s_m3_pass * 0x1150u) % span)
                        & ~(size_t)1u;
        s_m3_pass++;
    }
#endif
}

/* Hook side: blend the planned blocks into the staged band rows, after the
 * layer loop and inside the same timed paint window, so the CAD hook/px
 * fields carry the ink's true cost. */
static void bench_paint_band(int32_t y0, int nr)
{
    if (!s_bench_solid || !s_bench_n) return;
    const gfx_rows_t *R = gfx_rows();
    for (uint32_t i = 0; i < s_bench_n; i++) {
        const int32_t by0 = s_bench_y[i];
        if (by0 > y0 + nr - 1 || by0 + BENCH_BLK - 1 < y0) continue;
        const int32_t bx0 = s_bench_x[i];
        for (int r = 0; r < nr; r++) {
            const int32_t y = y0 + r;
            if (y < by0 || y > by0 + BENCH_BLK - 1) continue;
            int32_t xa = bx0, xb = bx0 + BENCH_BLK - 1;
            if (xa < R->x0[y]) xa = R->x0[y];
            if (xb > R->x1[y]) xb = R->x1[y];
            if (xa > xb) continue;
#if ORB_ART_M3_PROBE
            /* Source walks the bank, not a 7.2 KB mag plane: this line's
             * memory location IS the A-vs-B question, at cadence. */
            const uint16_t *src = s_bench_solid + (s_m3_off[i] >> 1)
                                + (size_t)(y - by0) * BENCH_BLK;
#else
            const uint16_t *src = &s_bench_solid[(size_t)(y - by0) * BENCH_BLK];
#endif
            uint16_t *row = gfx_band(r);
            for (int32_t x = xa; x <= xb; x++)
                gfx_blend_px(&row[x], src[x - bx0], DISPLAY_BENCH_ALPHA, 0);
            orb_sched_ink_px_add((uint32_t)(xb - xa + 1));
        }
    }
}
#endif /* DISPLAY_BENCH */

#endif /* CLK_BG_DIRECT_PUSH */

/* ---- complications: baked windows and strips under the live dial -----------
 *
 * The policy is locked with the baker (tools/bake_clock.py::bake_complications):
 * window tiles pack RAW — colour plane w*h*2 then planar alpha w*h — never
 * composited against the dial. The tile a value selects goes down as the
 * bottom-most paint job and the live dial's own window frame composites over
 * its edge, so a tile sits in its bezel the way a real disc sits behind a
 * real aperture. Windows address the art blob directly
 * (c->off + frame*c->frame_bytes); the shared sprite bank serves strips only
 * — glyphs and window pieces — exactly as tools/orbtheme.py pack_geom packed
 * them.
 *
 * The frame-selection maths is the exact mirror of
 * tools/theme_schema.py::frame_index: the bake laid tiles out by that
 * function, so the rounding, the clamp and the wrap must match to the pixel.
 * Everything stays integer, so no libm floor can drift from the Python.
 *
 * What this pass draws: WINDOW and STRIP. RING and FLARE validate at
 * orb_theme.c but ship on no bake and nothing here fixes how a value rotates
 * one or blends: logged once, hidden, the same discipline the reader applies
 * to a kind newer than the dial. Under-dial windows (opaque, bake-composited
 * — see orb_theme.h) and battery-driven comps (the orb is wall-powered:
 * main/net/orb_net.c) are likewise parsed, validated, and not drawn. */

#define CLK_COMP_SLOTS (CLK_STRIP_GLYPHS_MAX + 2)  /* glyph positions + window pieces L/R */
static lv_obj_t       *s_comp_obj[CLK_COMP_MAX][CLK_COMP_SLOTS];
static lv_image_dsc_t  s_comp_dsc[CLK_COMP_MAX][CLK_COMP_SLOTS];
/* The frame drawn right now, per comp: the window's tile index; the strip's
 * digit at each glyph position and a 0 for each window piece; -1 = nothing
 * drawn. Sole truth for both paths: the layer table's pose pass reads these
 * to pick each BANK row's plane, and the classic path reads the LVGL widgets
 * comp_slot_show keeps in step. Any drift between the two is the one-tick
 * ghost the hands paid for before apply_angles learned to hold its angles. */
static int32_t         s_comp_frame[CLK_COMP_MAX][CLK_COMP_SLOTS];
static uint32_t        s_comp_logged;        /* bit i: hide reason for comp i printed */

/* theme_schema.frame_index() verbatim: sign-aware round-to-nearest at
 * scale = frames-per-value-unit x16, clamp below the domain, and WRAP —
 * never clamp — at count, so weekday and month keep cycling. */
static uint16_t comp_frame_index(int32_t value, uint16_t count,
                                 int16_t value_min, uint16_t scale)
{
    if (!count) return 0;
    const int32_t s = scale ? (int32_t)scale : 16;
    int64_t idx = (int64_t)(value - (int32_t)value_min) * s;
    idx = (idx >= 0) ? (idx + 8) / 16 : -((-idx + 8) / 16);
    if (idx < 0)               idx = 0;
    if (idx >= (int64_t)count) idx %= (int64_t)count;
    return (uint16_t)idx;
}

/* How many glyph positions a strip box holds: the same ceil(w / glyph_step)
 * the reader took when it accepted the bake. That side refuses a bake this
 * side cannot lay out, so this side only repeats the division. */
static int comp_strip_positions(const clk_comp_t *c)
{
    if (c->glyph_step <= 0) return 0;             /* the reader refused such a strip */
    const uint32_t pos = ((uint32_t)c->w + (uint32_t)c->glyph_step - 1u)
                       / (uint32_t)c->glyph_step;
    return (pos > CLK_STRIP_GLYPHS_MAX) ? CLK_STRIP_GLYPHS_MAX : (int)pos;
}

/* theme_schema.strip_indices() verbatim: the value's decimal, most
 * significant first, each glyph reduced into the glyph set, cut to the last
 * npos digits the box can hold. out[p] == -1 leaves position p empty, and
 * the drawn run sits right-aligned like a real date drum; a zero-padded bake
 * fills the spare left positions with the zero glyph instead. */
static void comp_strip_digits(int32_t value, int npos, bool zero_pad,
                              uint16_t count, int16_t *out)
{
    for (int p = 0; p < npos; p++) out[p] = -1;
    if (npos <= 0) return;
    char buf[12];
    if (value < 0) value = 0;                     /* no source goes negative; belt */
    snprintf(buf, sizeof buf, "%d", (int)value);
    int len = (int)strlen(buf);
    if (zero_pad && len < npos) {
        memmove(buf + (npos - len), buf, (size_t)len);
        memset(buf, '0', (size_t)(npos - len));
        len = npos;
    }
    if (len > npos) { memmove(buf, buf + len - npos, (size_t)npos); len = npos; }
    const int first = npos - len;
    const int gmax  = count ? (int)count : 1;
    for (int p = 0; p < len; p++)
        out[first + p] = (int16_t)((buf[p] - '0') % gmax);
}

/* One known new moon (2000-01-06 18:14 UTC) and the mean synodic month, both
 * whole seconds: the age stays integer end to end, 0..29 days. The 30-tile
 * moon a bake lays out indexes the age once per day — the half-day the mean
 * month carries over 29.53 costs a tile-edge rounding at most, which is what
 * a float-free dial path buys. */
#define MOON_NEW_MOON_S  947182440LL
#define MOON_SYNODIC_S   2551443LL        /* 29.530588853 d */

static bool comp_year_leap(int y) { return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0); }

/* ISO 8601 week number: week 1 holds the year's first Thursday, so the
 * Thursday of the date's own week says which year the week belongs to. A
 * Thursday past the year end is the coming year's week 1; one before its
 * start is the outgoing year's LAST week — numbered off ITS Dec 28, which
 * always sits inside that final week (Dec 31's Thursday can already be in the
 * new year, which is exactly how numbering off Dec 31 over-counted). All from
 * the tm fields compose_now already filled: no mktime round-trip here. */
static int32_t comp_iso_week(const struct tm *ti)
{
    const int yday1 = ti->tm_yday + 1;
    const int wd7   = (ti->tm_wday + 6) % 7 + 1;           /* Mon==1 .. Sun==7 */
    const int pdays = comp_year_leap(ti->tm_year + 1900) ? 366 : 365;
    const int th    = yday1 + 4 - wd7;                     /* week's Thursday  */
    if (th > pdays) return 1;                              /* next year's W1   */
    if (th >= 1)    return (yday1 + 10 - wd7) / 7;
    const int py    = ti->tm_year + 1900 - 1;
    const int pyd   = comp_year_leap(py) ? 366 : 365;
    const int jan1  = ((wd7 - 1 - (ti->tm_yday % 7)) % 7 + 7) % 7 + 1;
    const int d31   = ((jan1 - 2) % 7 + 7) % 7 + 1;        /* prev Dec 31      */
    const int d28   = ((d31 - 4) % 7 + 7) % 7 + 1;         /* three days on    */
    return (pyd - 3 + 10 - d28) / 7;
}

/* The value that drives a comp, from the same local struct tm and whole
 * second compose_now sampled the hands from. The tile ORDER is the bake's
 * (bake_clock._WEEKDAY_TILES is Monday-first, _MONTH_TILES January-first),
 * so tm_wday/tm_mon map into the frame domain directly. Returns false = no
 * input exists on this hardware for that source. */
static bool comp_value(uint8_t src, const struct tm *ti, time_t t, int32_t *out)
{
    switch (src) {
    case CLK_SRC_NONE:        *out = 0;                            break;  /* a FORMAT 3 static window: one plate, frame 0 (D32 R-B: no fixed domain to consult, the row is one tile) */
    case CLK_SRC_WEEKDAY:     *out = (ti->tm_wday + 6) % 7;   break;
    case CLK_SRC_MONTH:       *out = ti->tm_mon;              break;
    case CLK_SRC_DATE:        *out = ti->tm_mday;             break;
    case CLK_SRC_HOUR12:      *out = ti->tm_hour % 12;        break;
    case CLK_SRC_HOUR24:      *out = ti->tm_hour;             break;
    case CLK_SRC_MINUTE:      *out = ti->tm_min;              break;
    case CLK_SRC_SECOND:      *out = ti->tm_sec;              break;
    case CLK_SRC_AMPM:        *out = ti->tm_hour >= 12;       break;
    case CLK_SRC_WEEK:        *out = comp_iso_week(ti);       break;
    case CLK_SRC_DAY_OF_YEAR: *out = ti->tm_yday + 1;         break;
    case CLK_SRC_QUARTER:     *out = ti->tm_mon / 3 + 1;      break;
    case CLK_SRC_MOON_AGE: {
        int64_t m = ((int64_t)t - MOON_NEW_MOON_S) % MOON_SYNODIC_S;
        if (m < 0) m += MOON_SYNODIC_S;
        *out = (int32_t)(m / 86400);
        break; }
    default:                  return false;  /* none/text/value/battery/reserve */
    }
    return true;
}

/* Point one comp slot's image at blob pixels and un-hide it. Under L4 the
 * widget never paints — nothing invalidates it — but it must still say what
 * the job list says: whenever LVGL renders a page itself (bounce-on, a
 * refused direct push) it renders the widget, and a tick of drift between
 * the two is a ghost on the glass. */
static void comp_slot_show(uint16_t i, int p, uint32_t color_off,
                           uint16_t w, uint16_t h, int32_t x, int32_t y)
{
    lv_image_dsc_t *d = &s_comp_dsc[i][p];
    memset(d, 0, sizeof *d);
    d->header.magic  = LV_IMAGE_HEADER_MAGIC;
    d->header.cf     = LV_COLOR_FORMAT_RGB565A8;   /* planar colour+alpha, as packed */
    d->header.w      = w;
    d->header.h      = h;
    d->header.stride = (uint32_t)w * 2u;
    d->data_size     = (uint32_t)w * (uint32_t)h * 3u;
    d->data          = blob() + color_off;
    lv_obj_t *o = s_comp_obj[i][p];
    if (!o) return;
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, (lv_coord_t)x, (lv_coord_t)y);
    lv_image_set_src(o, d);
    lv_obj_set_hidden(o, false);
}

static void comp_slot_hide(uint16_t i, int p)
{
    if (s_comp_obj[i][p]) lv_obj_set_hidden(s_comp_obj[i][p], true);
}

/* Ask whoever owns the pixels to rebuild this comp's box: L4 marks the rows
 * (the hook restores the canvas under them and repaints EVERY job, comps
 * included), the classic path invalidates so LVGL re-renders plate and
 * children over the area. box_h runs the mark down to the tallest thing the
 * strip actually draws — the reader bounds the RECORD's box against the
 * panel and the glyphs' art against the payload, not the glyphs against the
 * box. */
static void comp_touch_box(const clk_comp_t *c, int32_t box_h)
{
    int32_t x0 = c->x, y0 = c->y;
    int32_t x1 = c->x + (int32_t)c->w - 1;
    int32_t y1 = c->y + box_h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > CLK_SCR_W - 1) x1 = CLK_SCR_W - 1;
    if (y1 > CLK_SCR_H - 1) y1 = CLK_SCR_H - 1;
    if (x1 < x0 || y1 < y0) return;
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_l4_active()) { gfx_mark_rows(y0, y1, x0, x1, false); return; }
#endif
    lv_area_t b = { x0, y0, x1, y1 };
    lv_obj_invalidate_area(s_scr, &b);
}

/* The per-tick pass. A comp only dirties pixels when its SELECTION moved —
 * tile index, glyph digits, shown/hidden — so a hung calendar costs one
 * integer compare per comp per tick, and a theme with no complications costs
 * one loop-exit. */
static void comps_update(const struct tm *ti, time_t t)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++) {
        const clk_comp_t *c = &g->comps[i];
        const bool window = (c->kind == CLK_KIND_WINDOW);
        const bool strip  = (c->kind == CLK_KIND_STRIP);
        const int  npos   = strip ? comp_strip_positions(c) : 0;
        const int  slots  = window ? 1 : (strip ? npos + 2 : 0);
        bool dirty = false;

        /* What to draw, with every valid-but-hidden case logged once per
         * comp: a kind with no painter, an opaque under-dial tile, a source
         * with no input, an explicit DISABLED, a blink off-beat. */
        bool live = c->show && !(c->flags & CLK_FLAG_DISABLED);
        if (live && !window && !strip) {
            live = false;
            if (!(s_comp_logged & (1u << i))) {
                s_comp_logged |= 1u << i;
                ESP_LOGW(TAG, "comp %" PRIu32 ": kind %u has no painter yet — not drawn",
                         (uint32_t)i, (unsigned)c->kind);
            }
        }
        if (live && window && (c->flags & CLK_FLAG_UNDER_DIAL)) {
            live = false;
            if (!(s_comp_logged & (1u << i))) {
                s_comp_logged |= 1u << i;
                ESP_LOGW(TAG, "comp %" PRIu32 ": bake-composited under-dial tile — this dial "
                              "draws raw tiles under the live frame only", (uint32_t)i);
            }
        }
        int32_t value = 0;
        if (live && !comp_value(c->src, ti, t, &value)) {
            live = false;
            if (!(s_comp_logged & (1u << i))) {
                s_comp_logged |= 1u << i;
                ESP_LOGW(TAG, "comp %" PRIu32 ": source %u has no input on this device — not drawn",
                         (uint32_t)i, (unsigned)c->src);
            }
        }
        if (live && (c->flags & CLK_FLAG_BLINK) && (t % 2) != 0) live = false;

        int32_t box_h = (int32_t)c->h;
        if (strip && npos > 0 && c->count) {
            const clk_sprite_t *b = &g->bank[c->first];
            if ((int32_t)b->h > box_h) box_h = b->h;
        }

        if (!live) {
            for (int p = 0; p < slots; p++)
                if (s_comp_frame[i][p] != -1) {
                    comp_slot_hide(i, p);
                    s_comp_frame[i][p] = -1;
                    dirty = true;
                }
            if (dirty) comp_touch_box(c, box_h);
            continue;
        }

        if (window) {
            const int32_t f = (int32_t)comp_frame_index(value, c->count,
                                                        c->value_min, c->scale);
            if (s_comp_frame[i][0] != f) {
                comp_slot_show(i, 0, c->off + (uint32_t)f * c->frame_bytes,
                               c->w, c->h, c->x, c->y);
                s_comp_frame[i][0] = f;
                dirty = true;
            }
        } else {
            int16_t dig[CLK_STRIP_GLYPHS_MAX];
            comp_strip_digits(value, npos, (c->flags & CLK_FLAG_ZERO_PAD) != 0,
                              c->count, dig);
            for (int p = 0; p < npos; p++) {
                if (s_comp_frame[i][p] == dig[p]) continue;
                if (dig[p] < 0) {
                    comp_slot_hide(i, p);
                } else {
                    const clk_sprite_t *b = &g->bank[c->first + (uint16_t)dig[p]];
                    comp_slot_show(i, p, b->color_off, b->w, b->h,
                                   c->x + c->glyph_lead + (int32_t)p * c->glyph_step,
                                   c->y);
                }
                s_comp_frame[i][p] = dig[p];
                dirty = true;
            }
            /* Window pieces: fixed pixels, drawn whenever the strip is, so
             * their frame goes to -1 only while the whole comp is hidden. */
            if (c->edge_left != 0xFFFFu && s_comp_frame[i][npos] < 0) {
                const clk_sprite_t *b = &g->bank[c->edge_left];
                comp_slot_show(i, npos, b->color_off, b->w, b->h, c->x, c->y);
                s_comp_frame[i][npos] = 0;
                dirty = true;
            }
            if (c->edge_right != 0xFFFFu && s_comp_frame[i][npos + 1] < 0) {
                const clk_sprite_t *b = &g->bank[c->edge_right];
                comp_slot_show(i, npos + 1, b->color_off, b->w, b->h,
                               c->x + (int32_t)c->w - (int32_t)b->w, c->y);
                s_comp_frame[i][npos + 1] = 0;
                dirty = true;
            }
        }
        if (dirty) comp_touch_box(c, box_h);
    }
}

/* One comp's widgets, hidden until comp_slot_show points them at pixels — the
 * same widgets the L4 hook mirrors, so both paths must create them in the same
 * relative order or a row LVGL renders itself disagrees with a row the hook
 * paints. The caller creates them at the point in the sibling chain that puts
 * the tile UNDER the live layer the bake named: the classic path's only notion
 * of z-order is creation order, and the tile must sit under its frame, not
 * just somewhere on the page. */
static void comp_create_slots(const orb_theme_geo_t *g, uint16_t i)
{
    const clk_comp_t *c = &g->comps[i];
    int slots = 0;
    if      (c->kind == CLK_KIND_WINDOW) slots = 1;
    else if (c->kind == CLK_KIND_STRIP)  slots = comp_strip_positions(c) + 2;
    for (int p = 0; p < slots && p < CLK_COMP_SLOTS; p++) {
        lv_obj_t *o = lv_image_create(s_scr);
        lv_obj_set_align(o, LV_ALIGN_TOP_LEFT);
        lv_obj_set_scrollable(o, false);
        lv_image_set_blend_mode(o, map_blend(c->blend));
        lv_obj_set_hidden(o, true);
        s_comp_obj[i][p] = o;
    }
}

/* The z bucket of one record: a WINDOW's `first` IS it, and every other kind
 * addresses the bank with that field, so it rides the bottom bucket. */
static int comp_bucket(const clk_comp_t *c)
{
    return (c->kind == CLK_KIND_WINDOW) ? (int)c->first : 0;
}

/* Create the widgets of ONE bucket, in record order. clock_view_init calls this
 * at the point in the sibling chain that puts the tile UNDER the live layer the
 * bake named — the classic path's only notion of z-order is creation order, so
 * the bucket has to be honoured HERE and not just in the L4 job list, or the two
 * paths disagree about who covers whom. Already-created records are skipped, so
 * a bucket nobody asked for is not lost: the caller drains the rest afterwards. */
static void complications_create_bucket(const orb_theme_geo_t *g, int bucket)
{
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++) {
        const clk_comp_t *c = &g->comps[i];
        if (!c->show || (c->flags & CLK_FLAG_DISABLED)) continue;
        if (s_comp_obj[i][0]) continue;                     /* created already */
        if (comp_bucket(c) != bucket) continue;
        comp_create_slots(g, i);
    }
}

static void complications_create(void)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    memset(s_comp_obj,   0,    sizeof s_comp_obj);
    memset(s_comp_frame, 0xFF, sizeof s_comp_frame);      /* -1: nothing drawn yet */
    complications_create_bucket(g, 0);
}


/* ---- aux hands: the sub-dial pointers a theme's style declares -------------
 *
 * One clk_aux_t row is a sprite plus how it is driven: the value named by `src`
 * turns it through `span` tenths of a degree from `zero`, once across the domain
 * value_max wide. The five compiled-in slots cannot carry them — a sub-dial
 * pointer is not the dial's hour hand, and the clock cannot grow a compiled-in
 * count for a theme that arrives on a card — and the kit's geometry mounts each
 * one on its OWN sub-dial's centre, which is why the row owns its pivot/centre
 * pair and its driver reads the same struct tm compose_now already sampled. The
 * value comes from comp_value(), so a date ring and the date WINDOW can never
 * disagree about what day it is.
 *
 * A shadowed pointer is TWO rows: the hand, then its blur/offset twin flagged
 * CLK_FLAG_AUX_SHADOW (tools/bake_clock.py lays them down adjacent, hand first,
 * and tools/theme_schema.py packs that pair; the FORMAT 3 composer bakes the
 * same pair through orbtheme-baker.js, where row flag bit 4 — AUX_F_SHADOW3,
 * mapped in orb_clock3.c — carries it into this vocabulary). The twin owns
 * pixels and a centre (the hand's, moved by the light vector) and no input of
 * its own — the tick gives it the row before it's angle — so it can never come
 * loose from the pixels it exists to darken. The bake packs the twin so its
 * pivot lands on its own centre, the hand's centre plus the light: pointer and
 * twin turn as one rigid pair, and the offset stays fixed in screen space
 * while the pointer sweeps.
 *
 * z-order: the block inserts BEFORE the first live hand (a pointer on its own
 * ring belongs under the main hands that sweep across it) and above every live
 * static (it belongs on top of the plate it points at), which is the kit's own
 * reading — the date pointer is z=1, the hour z=2.
 * s_aux_at is that order position, computed once per view in clock_view_init and
 * honoured by BOTH paint paths, because a row LVGL renders itself has to come
 * out identical to a row the L4 hook paints. */

/* Where the block joins clk_theme.order: the order position it goes BEFORE. A
 * sub-dial pointer stands on its own ring, UNDER the main hands that sweep
 * across it, so the block precedes the first live hand — which for every bake
 * that orders statics first (the kit bakes, the composer's, bake_clock.py's) is
 * also the index directly above the last live static, the same join point this
 * function has always resolved to for those tables. A theme with live statics
 * and no live hand joins above the last static; one with neither returns -1,
 * which leaves the callers' fall-through ride the block above everything that
 * does exist — the only reading left when there is no hand to hide it under.
 * The D34 composer bakes order:[0,1] with no static rows at all, and a block
 * appended after the loop then drew the seconds pointer OVER the hour hand. */
static int aux_insert_at(void)
{
    int last_static = -1;
    for (int oi = 0; oi < (int)clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (clk_theme.matte_mask & (1u << k)) continue;   /* folded into the bg */
        if (k <= 2) return oi;              /* below the first live hand */
        last_static = oi + 1;               /* above the last live static */
    }
    return last_static;
}

/* The value that turns a pointer, in thousandths of one unit: day 5 is 5000, and
 * a sub-dial seconds hand at 30.4 s is 30400. aux_angle_10() divides by
 * value_max, so the fraction has to reach that division or a sweep becomes a
 * stutter — and CLK_FLAG_JUMP is the kit ASKING for the stutter, the mechanical
 * cadence of a date ring that only moves at midnight anyway. */
static bool aux_value_1000(const clk_aux_t *a, const struct tm *ti, time_t t,
                           double frac, int64_t *out)
{
    int32_t v;
    if (!comp_value(a->src, ti, t, &v)) return false;   /* no input on this orb */

    int64_t v1000 = (int64_t)v * 1000;
    if (!(a->flags & CLK_FLAG_JUMP)) {                  /* next finer field = sweep */
        switch (a->src) {
        case CLK_SRC_SECOND:  v1000 += (int64_t)(frac * 1000.0);        break;
        case CLK_SRC_MINUTE:  v1000 += (int64_t)ti->tm_sec * 1000 / 60; break;
        case CLK_SRC_HOUR12:
        case CLK_SRC_HOUR24:  v1000 += (int64_t)ti->tm_min * 1000 / 60; break;
        default:                                                        break;
        }
    }
    if (!(a->flags & CLK_FLAG_WRAP)) {        /* WRAP is the ring that keeps going */
        const int64_t lo = (int64_t)a->value_min * 1000;
        const int64_t hi = (int64_t)a->value_max * 1000;
        if (v1000 < lo) v1000 = lo;
        if (v1000 > hi) v1000 = hi;
    }
    *out = v1000;
    return true;
}

/* One line of arithmetic, two readings of its answer. D34 gave a pointer a MOTION
 * column, and the motion decides what `span` and `zero` ARE: tenths of a degree on a
 * rotate/retrograde/indexed row, pixels along the named axis on a slide-x/slide-y row.
 * Both are "zero + value * span / value_max", so the divide is written ONCE — a second
 * copy of that expression is where a slide and a rotation would start disagreeing about
 * the same theme. Half-up, not truncated: the bake placed day 5 at 58.1 degrees, and a
 * truncated read parks the pointer a tenth off the mark the art was drawn to.
 *
 * Returns the raw figure. The caller interprets it, because the two readings have
 * genuinely different bounds: an angle wraps the circle and a displacement does not —
 * clamping a slide to 3600 would stop a reserve bar a third of the way across its own
 * scale and read as a watch that lies about its battery. */
static int32_t aux_travel(const clk_aux_t *a, int64_t v1000)
{
    const int64_t den = (int64_t)a->value_max * 1000;
    const int64_t num = (int64_t)a->zero * den + v1000 * (int64_t)a->span;
    return (int32_t)((num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den));
}

/* The angle a pointer holds, in tenths of a degree. The wrap belongs HERE: an angle
 * has no meaning past a turn, and CLK_MOTION_RETROGRADE is the one that must not wrap
 * — it reaches its span and comes back, which is what a retrograde date ring does.
 * It is expressed as a clamp rather than a bounce because the value driving it already
 * resets at the top of its own domain: tm_mday returns to 1 without ever passing 31,
 * so clamping at the span is exactly the mechanical "reach the far mark, then jump
 * home" and no extra state is needed to draw it. */
static int32_t aux_angle_10(const clk_aux_t *a, int64_t v1000)
{
    int32_t t = aux_travel(a, v1000);
    /* One clamp, in the direction the span points. A negative span is a ring that
     * reads counter-clockwise, and its retrograde limit is that same far mark. */
    if (a->kind == CLK_MOTION_RETROGRADE &&
        ((a->span >= 0 && t > a->span) || (a->span < 0 && t < a->span)))
        t = a->span;
    t %= 3600;
    if (t < 0) t += 3600;
    return t;
}

/* The displacement a slide holds, in whole pixels from its row's placed origin. No
 * wrap and no modulus: a reserve bar at 100% sits at zero+span and stays there, and a
 * figure that wrapped at 3600 would stop the bar a third of the way across its own
 * scale and draw a watch that lies about its battery. */
static int32_t aux_offset_px(const clk_aux_t *a, int64_t v1000)
{
    return aux_travel(a, v1000);
}

/* The box a pointer at its current pose covers, in whichever geometry its motion
 * uses. A slide's swept box is the displaced record at no angle, which gfx_swept_box
 * answers correctly given the displaced sprite — so one helper serves both and the
 * invalidation below cannot mark the rows of the wrong shape. */
static void aux_mark_box(const clk_aux_t *a, int32_t move_10, lv_area_t *out)
{
    if (MOTION_IS_SLIDE(a->kind)) {
        clk_sprite_t d = a->spr;
        if (a->kind == CLK_MOTION_SLIDE_X) d.centerx = (int16_t)(d.centerx + move_10);
        else                              d.centery = (int16_t)(d.centery + move_10);
        gfx_swept_box(&d, 0, out);
        return;
    }
    gfx_swept_box(&a->spr, move_10, out);
}

/* Widgets for every row the theme ships, twin before hand so a shadow goes down
 * directly under its own pointer. make_layer() is the helper the five slots use,
 * so the pivot/centre contract, the blend mapping, and the nearest-neighbour
 * rotation are the same code here — the two paths cannot disagree about a layer
 * they were not both built from. Called from clock_view_init at the order point
 * s_aux_at marks, which is what puts the block above its plate. */
static void aux_create(void)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    uint16_t n = g->n_aux; if (n > CLK_AUX_MAX) n = CLK_AUX_MAX;
    for (uint16_t i = 0; i < n; i++) {
        if (g->aux[i].flags & CLK_FLAG_AUX_SHADOW) continue;   /* with its hand */
        if (i + 1 < n && (g->aux[i + 1].flags & CLK_FLAG_AUX_SHADOW))
            s_aux_obj[i + 1] = make_layer(&g->aux[i + 1].spr, &s_aux_dsc[i + 1]);
        s_aux_obj[i] = make_layer(&g->aux[i].spr, &s_aux_dsc[i]);
    }
}

/* The per-tick pass. A pointer dirties pixels only when its angle actually
 * moved, so a date hand costs one integer compare a tick until midnight, and a
 * theme with no rows costs one loop exit. A row the reader hid (an unknown src
 * or kind, a sprite with no size) and a row whose value this orb cannot supply
 * are both INT32_MIN: no job, no widget, one log line each. */
static void aux_update(const struct tm *ti, time_t t, double frac)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    uint16_t n = g->n_aux; if (n > CLK_AUX_MAX) n = CLK_AUX_MAX;
    for (uint16_t i = 0; i < n; i++) {
        const clk_aux_t *a = &g->aux[i];
        int32_t a_new;
        if (a->flags & CLK_FLAG_AUX_SHADOW) {
            a_new = (i > 0) ? s_aux_ang_10[i - 1] : AUX_HIDDEN;  /* follow the hand */
        } else if (!a->spr.show) {
            a_new = AUX_HIDDEN;
        } else {
            int64_t v1000;
            if (!aux_value_1000(a, ti, t, frac, &v1000)) {
                a_new = INT32_MIN;
                if (!(s_aux_logged & (1u << i))) {
                    s_aux_logged |= 1u << i;
                    ESP_LOGW(TAG, "aux %" PRIu32 ": source %u has no input on this "
                                  "device — pointer not drawn", (uint32_t)i,
                             (unsigned)a->src);
                }
            } else {
                /* One call, one reading of the row’s own motion: the angle a pointer
                 * turns through, or the pixels a bar slides. Reading a slide through
                 * the angle path would wrap its offset at 3600 tenths and park a reserve
                 * bar a third of the way across its own scale — a dial that lies about
                 * its battery while every number on the wire stays legal. */
                a_new = MOTION_IS_SLIDE(a->kind) ? aux_offset_px(a, v1000)
                                                 : aux_angle_10(a, v1000);
            }
        }
        /* C4's other half, unspent since the column landed: a needle parented to
         * the carriage rides the carriage's rotation too. The needle's arbour IS
         * the carriage's axis (the §25.2 crop pivot carries it), so rotation
         * about a shared centre commutes and the whole pose is one summed angle
         * — the same figure the carriage row itself wears (s_ang_10[2] plus the
         * row's start orientation). The twin is untouched: it mirrors its hand
         * row's stored angle and inherits the sum with it. */
        if (a->parent_row && a->parent_row == s_car_row_1 && s_car &&
            a_new > AUX_HIDDEN && !MOTION_IS_SLIDE(a->kind) &&
            s_ang_10[2] != INT32_MIN)
            a_new = (int32_t)(((int64_t)a_new + s_ang_10[2] + s_car_start_10) % 3600);
        if (a_new == s_aux_ang_10[i]) continue;               /* nothing moved */

        /* The wedge to rebuild: where the pointer was and where it is going,
         * about the point the device pins its pivot. Same discipline as
         * apply_angles() — under L4 the commit hook owns those rows. */
        const int32_t prev = s_aux_ang_10[i];
        /* Stage the INCOMING pose before anything reads it: the commit hook and the
         * classic widget below both draw a slide from s_aux_pose, and a mark computed
         * from one displacement with a paint reading another is the ghost the swept-box
         * contract exists to prevent. */
        if (a_new != AUX_HIDDEN && MOTION_IS_SLIDE(a->kind)) {
            s_aux_pose[i] = a->spr;
            if (a->kind == CLK_MOTION_SLIDE_X)
                s_aux_pose[i].centerx = (int16_t)(s_aux_pose[i].centerx + a_new);
            else
                s_aux_pose[i].centery = (int16_t)(s_aux_pose[i].centery + a_new);
        }
        if (s_aux_obj[i]) {
            lv_area_t u;
            bool have = false;
            if (prev != AUX_HIDDEN) {
                aux_mark_box(a, prev, &u); have = true;
                if (a_new != AUX_HIDDEN) {
                    lv_area_t nn; aux_mark_box(a, a_new, &nn);
                    gfx_area_union(&u, &nn);
                }
            } else if (a_new != AUX_HIDDEN) {
                aux_mark_box(a, a_new, &u); have = true;
            }
            if (have) {
                if (u.x1 < 0) u.x1 = 0;
                if (u.y1 < 0) u.y1 = 0;
                if (u.x2 > CLK_SCR_W - 1) u.x2 = CLK_SCR_W - 1;
                if (u.y2 > CLK_SCR_H - 1) u.y2 = CLK_SCR_H - 1;
#if CLK_BG_DIRECT_PUSH
                if (orb_sched_l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
                else
#endif
                    lv_obj_invalidate_area(s_scr, &u);
            }
        }
        if (s_aux_obj[i]) {
            /* Same discipline as apply_angles(): a rotation setter invalidates
             * the FULL transformed box of both poses, which is the 36-48 ms a
             * rotating layer used to cost every tick. Under L4 the hook owns the
             * rows we just marked, so the setter runs with invalidation off; the
             * first pose ever needs LVGL's own render because nothing has ever
             * painted these pixels. */
            lv_display_t *disp = lv_obj_get_display(s_scr);
            /* One pose, two setters. A pointer that turns gets the rotation setter
             * apply_angles() uses for the hands; a pointer that slides gets a position
             * setter and NO rotation at all, because rotating a sprite the commit
             * paints at zero would put the classic path and the L4 path on two
             * different pixels. The displacement rides the same s_aux_pose record
             * the mark and the table's pose pass read, so all three paths move by
             * one number. */
            const bool slide = MOTION_IS_SLIDE(a->kind);
            if (a_new == AUX_HIDDEN) {
                lv_obj_set_hidden(s_aux_obj[i], true);
            } else {
                if (prev == AUX_HIDDEN) lv_obj_set_hidden(s_aux_obj[i], false);
                else lv_display_enable_invalidation(disp, false);
                if (slide) {
                    /* make_layer() parked the widget so its pivot lands on the sprite's
                     * centre; the slide moves it along ONE axis from there by the same
                     * displacement s_aux_pose carries and the mark was computed from. */
                    const lv_coord_t base_x = a->spr.centerx - a->spr.pivotx;
                    const lv_coord_t base_y = a->spr.centery - a->spr.pivoty;
                    lv_obj_set_pos(s_aux_obj[i],
                                   a->kind == CLK_MOTION_SLIDE_X
                                       ? (lv_coord_t)(base_x + a_new) : base_x,
                                   a->kind == CLK_MOTION_SLIDE_Y
                                       ? (lv_coord_t)(base_y + a_new) : base_y);
                } else {
                    lv_image_set_rotation(s_aux_obj[i], (uint16_t)a_new);
                }
                if (prev != AUX_HIDDEN) lv_display_enable_invalidation(disp, true);
            }
        }
        s_aux_ang_10[i] = a_new;
    }
}

static void compose_now(void)
{
    /* The wall reading is the OS's now: the two sources, the CLOCK_EPOCH
     * baseline, the trust rule and the drift re-anchor live in main/app/
     * orb_svc.c as orb_time_now() (plan §4.3, the P3 time commit), and the
     * clock takes the same pair every app will. What stays in this file is
     * the dial's own: the zone conversion below, the hands, the comps, the
     * aux drives — D15's side of the line, app-side by ruling. */
    bool   synced_unused;
    time_t t;
    double frac;
    orb_time_now(&t, &frac, &synced_unused);

    /* The zone is device config, not a constant in this file: orb_net owns TZ
     * (NVS key `tz`, loaded at bring-up and settable from the config page or during
     * provisioning) and applies it with setenv()+tzset(). That is why changing the
     * zone needs no reboot and no call into this file — this localtime_r() is the
     * only place the zone reaches the dial, and the next frame after a change is
     * already in the new one. */
    struct tm ti;
    localtime_r(&t, &ti);

    float ang[5];
    hand_angles(frac, ti.tm_hour, ti.tm_min, ti.tm_sec, ang);
    apply_angles(ang);

    /* The complication pass: every window/strip re-reads its value from the
     * SAME local minute the hands were sampled from, and only the comps whose
     * selected frame actually changed dirty their rows (see the block above
     * compose_now). A theme with no complications costs one counter here. */
    comps_update(&ti, t);

    /* Then the sub-dial pointers, off the SAME reading: a date ring and the date
     * window are two views of one tm_mday, and the tick that moved one has moved
     * the other. frac is the wall fraction hand_angles swept against, which is
     * what lets a sub-dial second hand sweep too instead of stamping 6 degrees. */
    aux_update(&ti, t, frac);
}



/* ---- the layer table: the dial's rows, claimed the way every app claims ----
 * (Migration 2, Commit 3.)
 *
 * The rows come from the bundle, not from the geometry adapter: the adapter is
 * what the projection builds, and the projection is what this migration
 * deletes. So the adopted section is opened (orb_theme_live_sec — the same
 * bytes the dial is drawing, proven at the same door), its element table is
 * walked in the theme's own depth order, and every row is answered by
 * orb_elems_at(), the element dialect's reader, before it becomes an
 * orb_layer_desc_t the broker gates and the compositor commits.
 *
 * The fold. orb_elems_at() answers a plane's offset inside its section's own
 * bank; orb_asset_t.off is documented payload-relative (orb_assets.h), and the
 * commit pass resolves a base-less row against art_bank_ptr() (orb_sched.c).
 * The two conventions meet HERE, at the resolve step: off = plane + bank_off,
 * where bank_off is the figure the getter answers. Section 14 of
 * tools/host_orblayers_test.py pins both readings against the shipped hook:
 * the folded row lands the element's own pixels, an unfolded one lands its
 * neighbour's.
 *
 * Rows enter the table hidden — `show = 0`, live and counted, painting
 * nothing — and the tick's pose pass below (clock_layers_refresh) wakes and
 * drives them: the legacy renderer that owned this dial's composite until
 * Commit 4 is deleted, the scheduler's flush hook is the only commit path,
 * and the pass line is the ledger: the owner's app row reports the live count
 * equal to the bake's runtime rows, refused 0, and the frame byte-identical
 * against the dial-c3 baselines.
 *
 * What is not a row: the plate (class plate slot 0 — the canvas is the plate,
 * and a row for it would blend the background over its own restore); a row the
 * bake folded into the plate (the matte mask names it, honoured the way the
 * legacy job list skips a folded layer); and a switched-off row (EF_HIDDEN),
 * which is a theme's own off switch and not a refusal.
 */

/* One claimed row: the identity the bundle named, what the pose pass needs
 * to derive its current pose, and the descriptor as last pushed through the
 * gate — the pose shadow the per-tick diff reads. Commit 4 grew the record
 * for exactly that; Commit 3 only remembered (cls, slot). */
typedef struct {
    uint16_t cls, slot;        /* the identity, echoed back for logs          */
    uint16_t frame0, n_frames; /* the element's own frame window into the bank */
    int16_t  x, y;             /* theme placement — what a slide displaces     */
    uint8_t  kind;             /* BANK: CLK_KIND_*; AUX: CLK_MOTION_*; else 0  */
    uint8_t  gidx;             /* BANK/AUX: this row's index in the geo table
                                * the tick state is indexed by; 0xFF = none */
    orb_layer_desc_t d;        /* the pose the table holds right now          */
} clk_layer_key_t;
#define CLK_EF_AUX_TWIN   0x10u  /* theme_schema EF_AUX_SHADOW, the twin bit */
#define CLK_LAYER_KEYS_MAX 37   /* the clock's whole drawn vocabulary: 2 plates, 6 hands,
                                 * 5 shadows, 16 complications, 8 pointers. An ADOPTED
                                 * table cannot exceed it — orb_clock3.c refuses a row
                                 * outside those cells and a duplicate inside — so the
                                 * bound here is a belt, not a budget. */
/* 2026-10-24: PSRAM heap, lazy in clock_layers_build. As .bss these 2,960
 * bytes were the whole internal-RAM regression that starved httpd_start of
 * its 8 kB task stack (ESP_ERR_HTTPD_TASK, boot 2026-10-24: largest internal
 * block 10,240 with the ladder already logged). The pose pass reads the rows
 * once per tick and the broker's per-app row table has lived on PSRAM since
 * P5.5 (svc_layers_alloc, orb_svc.c) — same rate, same memory, now the same
 * heap. NULL until the first claim; every reader sits behind s_lay_n > 0 or
 * the build's own refusal path. */
static clk_layer_key_t *s_lay_key;
static int *s_lay_handle;
static int s_lay_n;
static orb_app_t *s_lay_app;                /* the row the table is claimed under */
/* The section and bank base the rows' frame offsets resolve against. The
 * adopted bundle changes only at boot — the store door stages, the flip
 * worker reboots — and this pass rebuilds on any re-show, so caching what
 * clock_layers_build opened is safe. NULL with s_lay_n stays louder than a
 * wrong answer: the pass keeps the glass it has. */
static const orb_bundle_sec_t *s_lay_sec;
static uint32_t s_lay_bank_off;

#define CLK_EF_HIDDEN  0x08u    /* theme_schema EF_HIDDEN, the mirror orb_clock3.c keeps */
#define CLK_W_MATTE    20u      /* orb_clock3.c W_MATTE_MASK: one truth, read the same way */

/* The fps bucket a row's art belongs to — the mapping the legacy job list
 * uses: an ordered layer counts under its slot, the carriage (hand slot 5)
 * rides the seconds count it turns with, a shadow counts with the hand casting
 * it, and the glass and the baked rows keep their own buckets. */
static uint8_t clk_row_role(uint16_t cls, uint16_t slot)
{
    if (cls == ORB_ASSET_HAND)   return (uint8_t)(slot == 5u ? 2u : slot);
    if (cls == ORB_ASSET_SPRITE) return (uint8_t)slot;
    if (cls == ORB_ASSET_PLATE)  return CLK_ROLE_GLASS;
    if (cls == ORB_ASSET_BANK)   return CLK_ROLE_COMP;
    return CLK_ROLE_AUX;
}

/* One element's row. The descriptor's own placement, pivot and blend stay
 * zero: the resolver's box and pivot answer them (orb_layers.c's recipe); the
 * FORMAT 3 dialect carries no blend column (a shadow is its own alpha plate);
 * and the angle starts at the theme's fixed degrees — the wall angle is what
 * Commit 4's tick adds, per row, per beat. */
static void clk_row_desc(const orb_elem_t *e, uint32_t bank_off,
                         orb_layer_desc_t *d)
{
    d->art.cls     = e->cls;
    d->art.tier    = ORB_ASSET_OK;                    /* the theme answered it  */
    d->art.x       = e->x;
    d->art.y       = e->y;
    d->art.w       = (int16_t)e->w;
    d->art.h       = (int16_t)e->h;
    d->art.pivotx  = e->pivot_x;
    d->art.pivoty  = e->pivot_y;
    /* A shadow twin (class sprite, slot<5) carries the §25.2 light offset on
     * its pivot COLUMN: its centre is hand-centre + (dx,dy) while its rotation
     * pivot is the CROP pivot (hand centre minus its own draw point). The two
     * break the centre = top-left + pivot identity that orb_layer_desc_t and
     * theme_spr bake into every other row, so feeding the raw pivot_x here made
     * the L4 painter spin the shadow about a point (dx,dy) off its true arbour:
     * the shadow swung out around the hand as it turned, worst on the second
     * hand (largest offset, full sweep). orb_clock3_parse already worked the
     * identity back into clk_theme.shadow[slot]; restate the row's box and
     * pivot from it so the L4 path rotates the shadow about the hand's real
     * arbour and lands its centre on hand + light, exactly like the LVGL path. */
    if (e->cls == ORB_ASSET_SPRITE && e->slot < 5u) {
        const clk_sprite_t *sh = &clk_theme.shadow[e->slot];
        d->art.x       = (int16_t)(sh->centerx - sh->pivotx);   /* top-left */
        d->art.y       = (int16_t)(sh->centery - sh->pivoty);
        d->art.pivotx  = sh->pivotx;                             /* crop pivot */
        d->art.pivoty  = sh->pivoty;
    }
    d->art.prim    = (uint16_t)orb_asset_class_prim((orb_asset_class_t)e->cls);
    d->art.global  = 0;                               /* the clock's namespace  */
    d->art.off     = bank_off + e->off;               /* the fold — see above   */
    d->art.bytes   = e->size;
    d->ang_10      = (int16_t)(e->degrees / 10u);     /* hundredths -> tenths   */
    d->show        = 0;                               /* Commit 4 wakes it      */
    d->role        = clk_row_role(e->cls, e->slot);
    d->gen_prim    = 0xFFu;                           /* the answer's own prim  */
    d->alpha       = 255;
}

static void clock_layers_build(orb_app_t *self)
{
    const orb_svc_t *svc = orb_svc_table();
    /* The broker wipes an app's rows when it goes behind the glass
     * (orb_svc_app_suspended — P5.5's clause that an app behind the dial owns
     * no pixels), and the wipe kills the handles behind this file's back. So
     * the delete-first pass runs only over a live table; behind a wipe the
     * handles are already dead, the table itself survives for the re-add, and
     * deleting them here would bill the ledger refusals the app never made. */
    if (orb_svc_layers_live(self) > 0)
        for (int i = 0; i < s_lay_n; i++) svc->layer_del(self, s_lay_handle[i]);
    s_lay_n = 0;
    s_lay_sec = NULL;                          /* the pose pass parks with the  */
    s_lay_bank_off = 0;                        /* rows it cannot resolve        */
    s_car_row_1 = 0;                           /* no carriage row seen yet      */

    const orb_bundle_sec_t *sec = NULL;
    uint32_t bank_off = 0;
    if (!orb_theme_live_sec(ORB_CLOCK3_SLUG, &sec, &bank_off))
        return;             /* no adopted bundle: no rows. The base face claims none. */
    /* Claim needs the two side tables before its first row: one PSRAM block
     * each, allocated once and kept for every later rebuild. An allocation
     * failure here lands in the same shape as the geo pass finding nothing:
     * zero rows claimed, the base face, and the refusal printed. */
    if (!s_lay_key) {
        s_lay_key = heap_caps_malloc(sizeof *s_lay_key * CLK_LAYER_KEYS_MAX,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_lay_handle = heap_caps_malloc(sizeof *s_lay_handle * CLK_LAYER_KEYS_MAX,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_lay_key || !s_lay_handle) {
        ESP_LOGE(TAG, "layer keys: PSRAM alloc failed — no rows claimed this build");
        return;
    }
    s_lay_sec = sec; s_lay_bank_off = bank_off;
    const orb_theme_geo_t *g_geo = orb_theme_geo();
    const int n_comps_geo = (int)g_geo->n_comps  < CLK_COMP_MAX
                            ? (int)g_geo->n_comps  : CLK_COMP_MAX;
    const int n_aux_geo   = (int)g_geo->n_aux     < CLK_AUX_MAX
                            ? (int)g_geo->n_aux    : CLK_AUX_MAX;

    /* Depth first, table order second. The insert sort is over at most 37
     * entries before anything is claimed. */
    const uint32_t matte = orb_bundle_scalar(sec, CLK_W_MATTE);
    uint16_t pick_cls[CLK_LAYER_KEYS_MAX], pick_slot[CLK_LAYER_KEYS_MAX];
    uint16_t pick_ord[CLK_LAYER_KEYS_MAX];
    int n = 0, hidden = 0, folded = 0;
    for (uint32_t i = 0; i < sec->n_elem && n < CLK_LAYER_KEYS_MAX; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == ORB_ASSET_HAND && e.slot == 5u)
            s_car_row_1 = (uint16_t)(i + 1u);  /* C4: needles parented to this row ride it */
        if (e.cls == ORB_ASSET_PLATE && e.slot == 0u) continue;   /* the canvas */
        if (e.flags & CLK_EF_HIDDEN) { hidden++; continue; }
        if ((e.cls == ORB_ASSET_HAND || e.cls == ORB_ASSET_SPRITE)
            && e.slot < 5u && (matte & (1u << e.slot))) { folded++; continue; }
        if (e.cls == ORB_ASSET_PLATE && (matte & 0x20u)) { folded++; continue; }
        /* A pointer's twin draws UNDER its hand — the legacy job list put it
         * there and the classic path's sibling order agrees. The bake packs
         * the pair adjacent, hand first, and gives both the hand's depth, so
         * depth-ties must break twin-first: the twin's sort key steps down
         * one rung. Equal keys otherwise keep table order. */
        const uint8_t twin = (e.cls == ORB_ASSET_AUX &&
                              (e.flags & CLK_EF_AUX_TWIN)) ? 1u : 0u;
        pick_cls[n] = e.cls; pick_slot[n] = e.slot;
        pick_ord[n] = (twin && e.order) ? (uint16_t)(e.order - 1u) : e.order;
        n++;
    }
    for (int a = 1; a < n; a++) {                 /* stable: equal depths keep table order */
        const uint16_t kc = pick_cls[a], ks = pick_slot[a], ko = pick_ord[a];
        int b = a - 1;
        while (b >= 0 && pick_ord[b] > ko) {
            pick_ord[b + 1] = pick_ord[b];
            pick_cls[b + 1] = pick_cls[b];
            pick_slot[b + 1] = pick_slot[b];
            b--;
        }
        pick_ord[b + 1] = (uint8_t)ko;
        pick_cls[b + 1] = kc;
        pick_slot[b + 1] = ks;
    }

    int refused = 0;
    for (int k = 0; k < n; k++) {
        orb_elem_t el;
        char why[160];
        const orb_elems_ans_t a = orb_elems_at(sec, pick_cls[k], pick_slot[k], 0u,
                                               &el, why, sizeof why);
        if (a != ORB_ELEMS_OK) {
            /* An adopted table answers OK to its own rows; anything else is
             * stated once and skipped rather than half-claimed. */
            ESP_LOGW(TAG, "layer row %u/%u: %s", (unsigned)pick_cls[k],
                     (unsigned)pick_slot[k], why);
            refused++;
            continue;
        }
        orb_layer_desc_t d;
        memset(&d, 0, sizeof d);
        clk_row_desc(&el, bank_off, &d);
        const int h = svc->layer_add(self, &d);
        if (h < 0) { refused++; continue; }   /* full, malformed, or no heap: the
                                               * table counted it; the walk moves on */
        clk_layer_key_t *r = &s_lay_key[s_lay_n];
        r->cls = pick_cls[k]; r->slot = pick_slot[k];
        r->frame0 = el.frame0; r->n_frames = el.n_frames;
        r->x = el.x; r->y = el.y;
        r->kind = (uint8_t)el.kind;
        /* The tick state — s_comp_frame, s_aux_ang_10 — is indexed by the
         * GEO table's own slot, which a dense bake makes the element slot and
         * a sparse one not. Resolve it once here, against the same projection
         * the update passes walk, and the pose pass never guesses. */
        r->gidx = 0xFFu;
        if (pick_cls[k] == ORB_ASSET_BANK && el.kind == CLK_KIND_WINDOW) {
            for (int c = 0; c < n_comps_geo; c++)
                if (g_geo->comps[c].tag == el.slot) { r->gidx = (uint8_t)c; break; }
        } else if (pick_cls[k] == ORB_ASSET_AUX) {
            for (int c = 0; c < n_aux_geo; c++)
                if (g_geo->aux[c].tag == el.slot) { r->gidx = (uint8_t)c; break; }
        }
        r->d = d;                               /* the pose the table holds now */
        s_lay_handle[s_lay_n] = h;
        s_lay_n++;
    }

    ESP_LOGI(TAG, "layer table: %d row%s, %d hidden, %d folded, %d refused (base %lu)",
             s_lay_n, s_lay_n == 1 ? "" : "s", hidden, folded, refused,
             (unsigned long)bank_off);
}

/* The per-tick pose pass (Migration 2, Commit 4): what the deleted
 * clock_layer_commit's job-list walk did at the flush, done against the wall
 * state at the tick instead — once per row, on the numbers that pass already
 * kept, and nothing reaches the gate twice.
 *
 * Each row's pose is DERIVED, never stored twice: hands and shadows read
 * s_ang_10 (apply_angles' angle, set before this runs), the carriage reads it
 * plus its start orientation at the frame tourbillon_advance just composited,
 * BANK rows read s_comp_frame the complication pass just selected, pointer
 * rows read s_aux_ang_10 / the slide s_aux_pose aux_update just staged, and
 * the glass is the static it was baked as. The gates match the retired job
 * list line for line — an angle of INT32_MIN means never posed, a comp frame
 * of -1 means the pass already hid it, shadow rows honour the theme's
 * shadow_on scalar — so a row that the legacy path would not have painted
 * wakes hidden and stays that way.
 *
 * Diff first: an hour hand that stands still costs one integer compare, the
 * way the retired hook made it cost. A 0->1 transition also TOUCHES: that is
 * the one damage a mark site cannot have computed, because until now the row
 * drew nothing and its box never entered the row bank. Vacated poses need no
 * touch here — every move-mark (apply_angles' wedges, the comp and pointer
 * boxes, the tourbillon diff) marks old and new together, exactly as the
 * legacy path demanded, and the flush weld restores and repaints both. */
static void clock_layers_refresh(void)
{
    if (s_lay_n == 0 || !orb_sched_l4_active())
        return;                       /* base face, classic path: nothing to feed */
    const orb_svc_t *svc = orb_svc_table();
    for (int i = 0; i < s_lay_n; i++) {
        clk_layer_key_t *r = &s_lay_key[i];
        orb_layer_desc_t d = r->d;    /* start from the pose the table holds */
        uint8_t show = 0;

        /* The visibility gates are the retired job list's own, widgets and
         * all: s_hand/s_shadow/s_car/s_overlay exist exactly where
         * clock_view_init decided a layer draws, and a row whose widget the
         * compiled vocabulary does not build — a pointer twin's hand that
         * was matte-folded, a sprite beyond the shadowed slots — stays
         * hidden here for the same reason it stayed out of that list. The
         * ledger counts it (the bake named a real row); generalising the
         * renderer past the compiled five is a later commit's bench. */
        switch (r->cls) {
        case ORB_ASSET_HAND:
            if (r->slot < 5u) {
                const int32_t a = s_ang_10[r->slot];
                if (s_hand[r->slot] && a != INT32_MIN) { show = 1; d.ang_10 = (int16_t)a; }
            } else if (s_car && s_ang_10[2] != INT32_MIN) {
                /* The carriage: the seconds angle plus its start orientation,
                 * at the frame the wall clock just picked — the same figure
                 * apply_angles and tourbillon_advance mark against. */
                int f = s_tourb_shown;
                if (f < 0 || f >= (int)r->n_frames) f = 0;
                orb_bundle_spr_t sp;
                orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + f), &sp);
                d.art.off = s_lay_bank_off + sp.off;
                d.art.bytes = sp.size;
                d.ang_10 = (int16_t)(s_ang_10[2] + s_car_start_10);
                show = 1;
            }
            break;
        case ORB_ASSET_SPRITE:
            if (r->slot < 5u && s_shadow[r->slot]) {
                const int32_t a = s_ang_10[r->slot];
                if (a != INT32_MIN) { show = 1; d.ang_10 = (int16_t)a; }
            }
            break;
        case ORB_ASSET_PLATE:
            if (s_overlay) show = 1;  /* the glass: built, unfolded, static   */
            break;
        case ORB_ASSET_BANK: {
            /* FORMAT 3 admits no comp kind but the window, so one row is one
             * plate and the complication pass' own selection answers which. */
            const int32_t f = (r->gidx != 0xFFu && r->kind == CLK_KIND_WINDOW)
                              ? s_comp_frame[r->gidx][0] : -1;
            if (f >= 0 && (uint32_t)f < r->n_frames) {
                orb_bundle_spr_t sp;
                orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + (uint32_t)f), &sp);
                d.art.off = s_lay_bank_off + sp.off;
                d.art.bytes = sp.size;
                show = 1;
            }
            break; }
        case ORB_ASSET_AUX: {
            const int32_t v = (r->gidx != 0xFFu) ? s_aux_ang_10[r->gidx] : AUX_HIDDEN;
            if (v != AUX_HIDDEN && v != INT32_MIN) {
                if (MOTION_IS_SLIDE(r->kind)) {
                    /* A slide reaches the compositor as the one thing it is:
                     * the plate at its placed origin, displaced along its
                     * axis by aux_update's own figure — the same number
                     * s_aux_pose carries to the marks and the widgets, so
                     * all three paths move by one and the static paint path
                     * costs what every static row costs. */
                    d.x = (int16_t)(r->x + (r->kind == CLK_MOTION_SLIDE_X ? v : 0));
                    d.y = (int16_t)(r->y + (r->kind == CLK_MOTION_SLIDE_Y ? v : 0));
                    d.ang_10 = 0;
                } else {
                    d.x = 0; d.y = 0;   /* the resolver's own placement        */
                    d.ang_10 = (int16_t)v;
                }
                show = 1;
            }
            break; }
        default:
            break;
        }
        d.show = show;
        if (memcmp(&d, &r->d, sizeof d) == 0) continue;
        const bool wake = !r->d.show && show;
        svc->layer_set(s_lay_app, s_lay_handle[i], &d);
        r->d = d;
        if (wake)                     /* the row's first box: no mark site knew */
            svc->layer_touch(s_lay_app, s_lay_handle[i], NULL);
    }
}

esp_err_t clock_view_init(orb_app_t *self)
{
    /* B1/B2, and the owner's rule that file management is the OS's: the clock does
     * not walk /sd itself and never will. It declares the one directory it reads
     * and the one extension a file must carry to be one of its themes, and the OS
     * (orb_files) owns the mount, the card lock, the name validation and the
     * enumeration. That declaration is also how the shell's options page and the
     * browser's copy of it can list this app's files without either one knowing
     * what an .orbtheme is — see main/sd/orb_files.h.
     *
     * The declaration itself lives in orb_theme.c, and this call re-declares it:
     * orb_theme_boot() put it in the table on the boot ladder, before any door
     * task could ask, so a hub that installed and enabled the clock over the wire
     * meets a theme door that already knows where the themes live. The values are
     * one copy in the tree, and a second registration is what orb_files_register
     * takes an app's init running twice to be. */
    const esp_err_t fe = orb_theme_src_register();
    if (fe != ESP_OK)
        ESP_LOGE(TAG, "themes file source refused (%s) — the theme list will be empty",
                 esp_err_to_name(fe));

    /* The bank was already found and mapped by orb_theme_boot(), which ran BEFORE
     * display_init() — a theme install must complete before anything reads pixels,
     * and only orb_theme knows which A/B slot the bank is in. §26: residency is
     * decided HERE, before either leg, and it is the one thing the two legs diverge
     * on. A resident bank — the flashed default at offset 0, or a slot-B container —
     * draws the dial on the themed leg below. No resident bank draws the base digital
     * face, the fallback for a board that has lost its theme. Residency is checked
     * before art is demanded so a bankless board is never failed at an art gate and
     * left on the panel's reset black.
     *
     * The middle term is the projection verdict, and it is not redundancy: mapping
     * and adoption are two separate facts, and the arithmetic alone reads
     * `bytes >= blob_off + art_size` over a zeroed geo — after a refusal's
     * use_no_art() that is `bytes >= 0`, true — sending this leg into orb_bg_init
     * over pixels that are not a bank. Bronze panicked in exactly that loop on
     * 2026-10-22. The theme answers the one honest question here — is there an
     * adopted, drawable theme — and the arithmetic stays what it always was: does
     * the mapping cover the blob. */
    const bool art_resident = art_bank_ready()
                    && orb_theme_resident()
                    && art_bank_bytes() >= orb_theme_geo()->blob_off
                                            + orb_theme_geo()->art_size;

    s_scr = lv_obj_create(NULL);

    /* §26: the base render, the no-resident-art fallback. Placed before the themed
     * leg because it needs no bank — it paints three labels — and because a board
     * with no resident art has nothing to demand. It covers the screen black and runs
     * its own readout, so the clock still tells the time on a shelf or after a theme
     * is lost. The themed leg below then reads the same residency flag to know its
     * mapping is real; s_art is never a wild pointer. */
    if (!art_resident) {
        lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
        clock_base_show(s_scr);
        /* The readout keeps itself current: clock_base owns a one-second LVGL timer
         * rather than borrowing the dial's scheduler, whose policy is a sprite view's
         * — pose hands, advance a frame bank, mark layer boxes — and none of which a
         * text face has. Installing that policy over three labels would be the sched-
         * uler ticking a callback that marks nothing, at 12 fps, to redraw a string
         * that changes once a minute. */
        clock_base_run();
        ESP_LOGW(TAG, "no resident art — base digital face (§26); the clock still "
                      "tells the time");
        return ESP_OK;
    }

    /* Themed leg: the residency flag is true here, so the mapping exists and covers
     * what the geometry says it must, and s_art is a real read rather than NULL plus
     * an offset. */
    s_art = art_bank_ptr() + orb_theme_geo()->blob_off;
    orb_bg_init(s_art);   /* the delta bank reads the same mapping, through the app */
    ESP_LOGI(TAG, "art bank mapped: %u B (theme needs %u B) from %s",
             (unsigned)art_bank_bytes(), (unsigned)orb_theme_geo()->art_size,
             orb_theme_source());

    /* Screen bg stays TRANSPARENT: the plate covers the full 466x466 opaquely,
     * so a black cover fill here would be rendered (and re-rendered per dirty
     * chunk) purely to be painted over — ~0.7 us/px of LVGL CPU time on the
     * GIF ticks. AMOLED is black out of reset; nothing can show through. */
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    /* ---- plate / background source ----
     * GIF delta mode: allocate the PSRAM canvas, seed it with the keyframe,
     * and make the plate widget read ITS pixels. Classic mode: plate + baked
     * bgAnim frames come straight out of the blob (zero RAM). */
    const lv_image_dsc_t *plate_src = NULL;
    if (clk_theme.bg_mode == 1)
        plate_src = orb_bg_gif_canvas_init();
    if (!plate_src) {
        orb_bg_classic_init();
        plate_src = orb_bg_plate_dsc();
    }
    lv_obj_t *plate = lv_image_create(s_scr);
    lv_obj_set_size(plate, CLK_SCR_W, CLK_SCR_H);
    lv_obj_set_pos(plate, 0, 0);
    lv_obj_set_scrollable(plate, false);
    lv_image_set_src(plate, plate_src);
    orb_bg_set_plate(plate);
    orb_bg_init_frames();
    /* Fresh widgets are all at rotation 0, so forget the angles we last pushed;
     * otherwise a rebuilt view would skip the re-pose as "unchanged". */
    for (int i = 0; i < 5; i++) s_ang_10[i] = INT32_MIN;
    /* The sub-dial pointers take the same rule, and the reason is the same and
     * worse: s_aux_ang_10 is static-zero, and 0 is a REAL pose rather than a
     * "never posed" mark — a seconds pointer rebuilt at 30 s would compare its
     * new angle against the dead theme's last one, skip the setter as
     * "unchanged" when they match, and the stale s_aux_obj entries would point
     * at widgets this destroy already freed. AUX_HIDDEN is the honest "nothing
     * drawn yet", so the first tick poses every fresh widget and marks its box. */
    for (int i = 0; i < CLK_AUX_MAX; i++) {
        s_aux_ang_10[i] = AUX_HIDDEN;
        s_aux_obj[i] = NULL;
    }
    s_aux_logged = 0;

    /* Complication widgets, bucket 0 first: created BEFORE the hand/layer loop
     * below so the classic path's sibling order is plate -> comp tiles -> S2 H2
     * S1 H1 S0 H0 -> glass — the tile a value selects sits under the live dial
     * frame, as the bake's runtime-composite policy requires. The tiles with a
     * bucket (a hosted window: the moon under its sub-dial's plate) are created
     * INSIDE that loop, at the point that puts them under the layer the bake
     * named, because creation order is the only z this path has. No-op for every
     * theme that ships no complications (the compiled-in pair and every
     * pre-calendar container both report n_comps == 0). */
    complications_create();
    const orb_theme_geo_t *geo = orb_theme_geo();
    s_aux_at = aux_insert_at();

    /* D32 ruling C REVERSED (§32.6): the carriage rides at geartrain depth, under
     * every hand, exactly as the preview composes it (watch-engine.js draws the cage
     * inside the geartrain pass). It used to be created after the hand loop, so the
     * frame bank blitted unrotated over the needle and hands the carriage was meant
     * to carry. Created here — after the bucket-0 complications and before the
     * shadow/hand loop — it sits above the plate and below the hands, and the
     * bucketed hosted windows created later still cover it the way their bucket says.
     * One object when the theme names a frame bank, placed by the bank's own shared
     * geometry and turned by tourbillon_advance with the seconds angle plus the row's
     * start orientation. A theme that carries both hand/2 and hand/5 now draws both. */
    s_car = NULL;
    s_car_start_10 = ((int32_t)clk_theme.tourbillon_start + 5) / 10 % 3600;
    if (clk_theme.tourbillon_n > 0 && clk_theme.tourbillon_n <= CLK_TOURB_MAX &&
        clk_theme.tourbillon[0].show &&
        gfx_sprite_drawable(&clk_theme.tourbillon[0]))
        s_car = make_layer(&clk_theme.tourbillon[0], &s_car_dsc);

    /* Each shadow goes down immediately before its OWN hand, walking the
     * theme's back-to-front order, so the z-stack is
     *     S2 H2 S1 H1 S0 H0   (S=shadow, H=hand, 2=second, 1=minute, 0=hour)
     * rather than every shadow under every hand. A shadow is cast by the hand
     * above onto the plane of the hands below it, so a hand must never stand
     * over the shadow of a hand that draws above it — with all the shadows in
     * one group first, the minute shadow fell UNDER the second hand and the
     * second hand was never darkened by it. (orb-os groups all shadows first;
     * that reads as a smudge on a hand the theme orders below, and it is the
     * one place this port diverges from it on purpose.)
     * Statics (3,4) are the face, not hands: they cast no shadow. */
    bool aux_built = false;
    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        /* The sub-dial block joins BEFORE the first live hand — the depth a
         * pointer on its own ring must sit at, since the main hands sweep over
         * it (see aux_insert_at). */
        if (!aux_built && oi == s_aux_at) { aux_create(); aux_built = true; }
        /* Folded into the background at bake time (tools/bake_clock.py
         * --matte): the plate/bank already carries this layer's pixels, so an
         * object here would paint a SECOND copy over them. The bake also ships
         * the sprite blank, so this is the belt to that pair of braces -- it
         * is what keeps a half-swapped bin+header pair from double-blending. */
        if (clk_theme.matte_mask & (1u << k)) continue;
        complications_create_bucket(geo, k);      /* the tiles under layer k */
        if (k <= 2 && clk_theme.shadow_on)
            s_shadow[k] = make_layer(&clk_theme.shadow[k], &s_shadow_dsc[k]);
        s_hand[k] = make_layer(&clk_theme.hand[k], &s_hand_dsc[k]);
    }
    if (!aux_built) aux_create();                 /* nothing to join under: ride the top */
    /* The carriage (s_car) is already created above, at geartrain depth — see the
     * D32-reversal note before the hand loop. What the dial actually built for the
     * sub-dials, in the line the owner reads after a re-bake: the count the theme
     * declared, and the order position the block joined. A row with an src this
     * build cannot drive is reported by aux_update the first time it tries, and
     * shows no blends/s here either. */
    if (geo->n_aux)
        ESP_LOGI(TAG, "aux: %u sub-dial pointer row%s above order[%d]",
                 (unsigned)geo->n_aux, (geo->n_aux == 1) ? "" : "s", s_aux_at);
    /* Any bucket whose layer never got an object (hidden, or folded under a
     * --matte force) lands above the layers that do exist, which is still the
     * plate's own position in the kit's stack and better than never drawing the
     * window at all. */
    for (int b = 1; b < 5; b++) complications_create_bucket(geo, b);

    /* Glass / hub cap last, on top of everything. */
    s_overlay = make_layer(&clk_theme.overlay, &s_overlay_dsc);

    /* The layer table, from the same bundle the widgets above were projected
     * from (Migration 2, Commits 3 and 4). Rows enter hidden and the tick's
     * pose pass wakes them — see clock_layers_refresh. A failure here is a
     * smaller ledger row on the owner's app page and a log line — never a
     * dial that fails to boot: the table only feeds L4, and a dial without a
     * canvas still draws through the widgets above. */
    s_lay_app = self;
    clock_layers_build(self);

    /* Tourbillon frame bank: descriptors for the second-hand layer's swap.
     * Reset the frame state so the first tick (re)draws it whole. */
    for (int i = 0; i < (int)clk_theme.tourbillon_n && i < CLK_TOURB_MAX; i++)
        if (gfx_sprite_drawable(&clk_theme.tourbillon[i]))
            gfx_fill_sprite_dsc(&s_tourb_dsc[i], &clk_theme.tourbillon[i], blob());
    s_tourb_shown = -1;
    s_tourb_t0 = lv_tick_get();


    /* The wall pair is the broker's to seed now: the first orb_time_now()
     * below stamps the anchor, exactly where the deleted s_t0_us line did. */
    compose_now();
#if CLK_BG_DIRECT_PUSH && DISPLAY_BENCH
    /* The flush weld point is the scheduler's to register since Commit 4
     * (orb_sched.c's sched_flush_commit); only the bench probe init below is
     * still this file's. */
    bench_init();
#endif

    static const orb_sched_policy_t clock_policy = {
        .tick     = clock_tick_policy,
#if DISPLAY_BENCH
        .bench    = bench_plan,
#endif
    };
    uint32_t start_period;
    if (orb_bg_gif_mode())                               start_period = orb_bg_gif_tick_ms();
    else if (orb_bg_bg_live() && clk_theme.bg_loop)      start_period = orb_sched_bg_rate(orb_sched_tick_base_ms(), orb_bg_every_p());
    else                                          { *orb_bg_every_p() = 1;
                                                     start_period = orb_sched_tick_base_ms(); }
    orb_sched_start(start_period, &clock_policy);   /* parked until the dial is on the glass */

    /* Report the cadence the bake actually recorded instead of a literal that
     * drifts out of date: the GIF and the tick share clk_gif_delays[] (the
     * GIF's own per-frame delays), and the sweeping hand beats at
     * tickRate when the theme declares one. */
    /* 96, not the 64 this shipped with: gif_frames, tick_rate and the delays are
     * runtime values now that a theme can arrive on an SD card, so the compiler can
     * no longer prove the worst case fits and -Werror=format-truncation says so. The
     * strings are still short — this is headroom for the widest digits, not a guess. */
    char bgdesc[96];
    if (orb_bg_gif_mode()) {
        uint32_t d0 = clk_gif_delays[0] > 0 ? (uint32_t)clk_gif_delays[0]
                                            : (uint32_t)CLK_GIF_TICK_MS;
        snprintf(bgdesc, sizeof bgdesc,
                 "gif delta bank: %u frames @ %u ms (%u.%u fps), tick %u ms",
                 (unsigned)clk_theme.gif_frames, (unsigned)d0,
                 (unsigned)(1000 / d0), (unsigned)((1000 % d0) * 10 / d0),
                 (unsigned)orb_bg_gif_tick_ms());
    } else if (orb_bg_bg_live()) {
        snprintf(bgdesc, sizeof bgdesc, "plate anim");
    } else {
        snprintf(bgdesc, sizeof bgdesc, "still");
    }

    char sweepdesc[48] = "off";   /* 32 -> 48 for the same reason as bgdesc above */
    const int sw_bps = clk_theme.tick_rate > 0 ? (int)clk_theme.tick_rate : 1;
    if (clk_theme.second_sweep) {
        const uint32_t tpb = orb_bg_gif_mode() ? hand_beat_ticks() : 1;
        if (orb_bg_gif_mode() && tpb > 1)
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, every %u ticks)",
                     (unsigned)sw_bps, (unsigned)tpb);
        else if (orb_bg_gif_mode())
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, sweep, every tick)",
                     (unsigned)sw_bps);
        /* A plate or still dial beats on the unified ladder, so the theme's beat is
         * an ASK and the running `clock %u fps` line is the answer. Print both the
         * ask and the base the ladder starts from: "on" alone hid the Station's
         * tick_rate 1 behind a sweep that looked broken on the glass and healthy
         * in the pane, and a reader who sees `1 bps, base 1000 ms` next to a dial
         * running at 4 fps knows at once which of the two clamped. */
        else
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, base %u ms)",
                     (unsigned)sw_bps, (unsigned)orb_sched_tick_base_ms());
    } else if (sw_bps > 1) {
        /* A ticking dial names its own beat now, on every art class. */
        snprintf(sweepdesc, sizeof sweepdesc, "off(%u bps, base %u ms)",
                 (unsigned)sw_bps, (unsigned)orb_sched_tick_base_ms());
    }

    ESP_LOGI(TAG, "analog clock: %d hand(s), shadow %s(%d,%d), sweep %s, bg %s",
             (clk_theme.hand[0].show ? 1 : 0) + (clk_theme.hand[1].show ? 1 : 0)
                 + (clk_theme.hand[2].show ? 1 : 0),
             clk_theme.shadow_on ? "on" : "off", clk_theme.shadow_dx, clk_theme.shadow_dy,
             sweepdesc, bgdesc);
    /* Which compositing method this art was baked for. A set bit means that
     * static layer is already folded into the plate/bank and costs no
     * gfx_blend_px on any commit (tools/bake_clock.py --matte, which only ever
     * sets a bit it PROVED pixel-identical); mask 0 is the ordinary dial where
     * every layer is composited by the layer-table commit each tick. */
    ESP_LOGI(TAG, "composite: %s",
             clk_theme.matte_mask ? "baked static matte" : "runtime layers");
    if (clk_theme.tourbillon_n)
        ESP_LOGI(TAG, "tourbillon: %u frames @ %u fps on the second-hand slot, orbit about (%d,%d)",
                 (unsigned)clk_theme.tourbillon_n, (unsigned)clk_theme.tourbillon_fps,
                 (int)clk_theme.hand[2].centerx, (int)clk_theme.hand[2].centery);
    return ESP_OK;
}

lv_obj_t *clock_view_screen(void)
{
    return s_scr;
}

void clock_view_set_bounce(bool on)
{
    /* Repurposed from the old digital label's bounce: this is the router's
     * "clock is the visible screen" gate. Park the tick timer when the clock is
     * not on the glass so a hidden dial burns no flush work; on re-show, repaint
     * the whole face cleanly and resume.
     *
     * C3 (§5.3) adds the one case a re-show must refuse: a store parked the dial
     * and unmapped its pixels, and the reboot that re-establishes the map has
     * not run. Waking the pose pass here would re-pose the layer table from
     * cached image descriptors into a mapping that no longer exists — a load
     * fault, not a repaint. The dial stays exactly as the glass holds it. */
    if (on && orb_theme_store_torn()) {
        ESP_LOGW(TAG, "bounce refused: the store took the bank down, reboot pending");
        orb_sched_set_running(false);
        return;
    }
    orb_sched_set_running(on);
    if (!orb_sched_started()) return;
    if (on) {
        if (orb_bg_gif_mode()) {
            /* The canvas still mirrors exactly what the glass held when we
             * parked — repaint it whole and keep cycling from the current
             * frame; resetting the counter would desync canvas vs deltas. */
            orb_sched_reanchor_gif();
            s_bt_ctr = 0;                   /* first tick back is a beat */
            orb_sched_reset_fps();
        } else {
            orb_bg_reset_play();
        }
        /* Tourbillon: force a whole first draw and restart the frame clock,
         * same discipline as the angle reset — nothing was composited while
         * parked, so whatever s_tourb_shown claims is stale. */
        s_tourb_shown = -1;
        s_tourb_t0 = lv_tick_get();
        /* The broker wipes this dial's rows when it goes behind the glass
         * (orb_svc_app_suspended), so the layer table is re-claimed on the
         * way back: a dial that returns to a table that no longer exists is
         * a dial that silently stops feeding the ledger. Only an emptied
         * table rebuilds — an activation that finds its rows still standing
         * (the boot ladder's first show) leaves them and their handles alone.
         * (Migration 2, commit 3.) */
        if (s_lay_app && orb_svc_layers_live(s_lay_app) == 0)
            clock_layers_build(s_lay_app);
        lv_obj_invalidate(s_scr);
        if (!orb_bg_gif_mode()) orb_bg_show_plate_frame(orb_bg_frame_now());
        compose_now();
        /* Pose the rebuilt table BEFORE the resume: a row woken on this
         * activation's first tick would leave one flush — this one, from the
         * bounce invalidate — welding a dial with missing hands. The wake
         * marks land in the same window the bounce dirty is flushing. */
        clock_layers_refresh();
        orb_sched_resume();
    } else {
        orb_sched_pause();
    }
}

bool clock_view_bounce(void)
{
    return orb_sched_running();
}


