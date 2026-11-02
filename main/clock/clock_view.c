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
#include "orb_sound.h"      /* D39: the engine the sound policy below schedules into */

static const char *TAG = "clock";

static lv_obj_t *s_scr;
static lv_obj_t *s_overlay;
static lv_image_dsc_t s_overlay_dsc;

/* ---- D42: the moving rows (clk_row_t, docs/element-model-plan.md) ----------
 * Every hand, pointer, ring, still layer, shadow twin and the carriage is one row,
 * and this is each row's tick state: its widget for the classic path, the pose it
 * shows now (an angle in tenths and the pivot's travel in px), the sprite at that
 * travel, and its ease. The L4 hook paints from the poses, the classic path from
 * the widgets, and both read ONE truth per tick. PSRAM, once: 24 rows of widget
 * descriptors and poses would sit on the internal heap boundary httpd_start needs
 * (the 2026-10-24 lesson the layer keys below record), and a row is read a handful
 * of times a frame. ROW_HIDDEN is the nothing-drawn mark; no pose reaches it. */
typedef struct {
    float    shown, from, to, amp;
    uint32_t t0;
    uint8_t  kind;
    bool     init;
} ease_t;

typedef struct {
    lv_obj_t      *obj;
    lv_image_dsc_t dsc;
    int32_t        ang_10;     /* the angle shown, tenths; ROW_HIDDEN = not drawn */
    int16_t        ox, oy;     /* the pivot's travel shown, px                    */
    clk_sprite_t   pose;       /* the row's sprite moved by (ox, oy)              */
    ease_t         ease;
    bool           logged;     /* "no input" printed for this row                 */
} row_state_t;
static row_state_t *s_row;
#define ROW_HIDDEN INT32_MIN

/* CLK_BG_DIRECT_PUSH, the L3/L4 switch the block below and the scheduler's delta
 * push both key on, lives in orb_sched.h: one number, both TUs, so the push and
 * the hook can never disagree about who owns a row. */


/* GIF-mode beat state (compose_now): tick counter, the true seconds it was last
 * re-anchored to, and the wall fraction held between beats. */
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



/* Hand AA by capability, read from the bake's own motion words (mirrored and
 * truth-tabled by tools/host_hand_aa_test.py). The dial affords bilinear on a
 * layer when that layer's 4-tap cost cannot land on a tick that is already
 * busy: an animated carrier (plate bank, delta bank, carriage) re-composites
 * the wedge every tick, and 4-tap sampling over that re-composite is the
 * ~40 ms/frame the Oct-10 findings measured, so those dials keep every layer
 * nearest exactly as before. On a still dial the tick idles between wall
 * beats, and a layer that re-poses at most once a second costs one 4-tap
 * wedge per second inside a flush budget built for the whole page — so its
 * hands may take the filter. The exception is the layer whose beat IS the hot
 * tick: a sweeping second hand stays nearest so a sweeping still dial (the
 * Station case) never pays the filter on the tick that drives its fps.
 *
 * A dial the painter owns answers from the bake instead (the unified render path's
 * slice 5): the row's EF_SMOOTH bit, which the baker sets where the commit's price
 * WITH the AA term still holds 12 fps and never on the hot-tick seconds. That one
 * answer drives both renderers — make_layer's LVGL widget here and the painter's
 * sampling through the row's layer descriptor (clk_row_desc) — because the widgets
 * stay live as the painter's fallback: if the two disagreed, any LVGL-initiated
 * repaint (the activation bounce, an overlay closing) would redraw a hand with the
 * other edge until it next moved. The test is the restore source, not the L4 gate,
 * so a painter-owned dial whose table the gate refused still reads its flag. No bit
 * (an older theme, the base face) reads as nearest. A CLK_BG_DIRECT_PUSH 0 build
 * has no painter and keeps the motion-word rule above whole. `src` names which rule
 * answered, for the boot line. */
#define CLK_EF_SMOOTH 0x80u    /* theme_schema EF_SMOOTH */
static bool row_smooth(uint8_t cls, uint16_t slot)
{
    const orb_bundle_sec_t *sec = NULL;
    uint32_t off = 0;
    if (!orb_theme_live_sec(ORB_CLOCK3_SLUG, &sec, &off)) return false;
    for (uint32_t i = 0; i < sec->n_elem; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == cls && e.slot == slot) return (e.flags & CLK_EF_SMOOTH) != 0;
    }
    return false;
}

static bool row_aa(const clk_theme_t *t, const clk_row_t *r, const char **src)
{
#if CLK_BG_DIRECT_PUSH
    if (orb_sched_restore_src() != NULL) {
        if (src) *src = "flag";
        return row_smooth(ORB_ASSET_AUX, r->tag);
    }
#endif
    if (src) *src = "classic-rule";
    if (t->bg_mode != 0 || t->bg_frames != 0 || t->gif_frames != 0 ||
        t->tourbillon_n != 0)
        return false;
    if (r->src == CLK_SRC_SECOND &&
        (r->cadence == CLK_CAD_SWEEP || r->cadence == CLK_CAD_STOP_TO_GO))
        return false;
    return true;
}

/* Create one rotated-hand/shadow layer from a sprite record. The object is
 * placed so its stored pivot (in cropped coords) lands on the sprite's screen
 * centre; LVGL then rotates it about that pivot. `aa` carries the sampling
 * decision for THIS layer from row_aa() above: LVGL defaults rotation to
 * 4-tap bilinear, ~4x the per-px cost of the nearest path on the S3, and the
 * old build forced nearest on every dial because the hands-over-animated-orb
 * re-composite could not carry it. The bake's motion words now say which
 * dials can, per layer. A layer that never rotates (the overlay) takes
 * nearest for free: LVGL skips the transform outright at angle 0. */
static lv_obj_t *make_layer(const clk_sprite_t *spr, lv_image_dsc_t *dsc,
                            bool aa)
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
    lv_image_set_antialias(o, aa);
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

/* ---- D41: ease -------------------------------------------------------------
 * A settle: after a jump (or a step slower than the second) the row overshoots its
 * new pose by a small amount and comes back, sin(pi*tau) over `ease` ms. A flyback:
 * when a flyback row returns to P0, it travels back over `ease` ms (smoothstep)
 * instead of landing in one frame. Both run on frames the dial asks for
 * (orb_sched_burst) only while they last; a row without an ease costs nothing.
 *
 * ease_pose is pure: the caller passes the clock in, so the host bench drives it. The
 * rows ease their fraction t (row_ease), so `turn` is 1 on a wrapping row, whose
 * fraction wraps, and 0 on every other. */
enum { EASE_NONE = 0, EASE_SETTLE, EASE_FLY };

static float ease_wrap(float d, float turn)
{
    if (turn <= 0.0f) return d;
    while (d >  turn / 2) d -= turn;
    while (d <= -turn / 2) d += turn;
    return d;
}

static float ease_pose(ease_t *e, float target, uint32_t now_ms, uint16_t ms, bool stepping,
                       bool retro, int span_sign, float turn, float amp, uint32_t *busy_ms)
{
    *busy_ms = 0;
    if (!ms) { e->init = false; return target; }
    if (!e->init) {
        e->init = true; e->to = e->shown = target; e->kind = EASE_NONE;
        return target;
    }
    if (target != e->to) {
        const float d = ease_wrap(target - e->to, turn);
        const bool back = retro && (span_sign >= 0 ? d < 0.0f : d > 0.0f);
        if (back) { e->kind = EASE_FLY; e->from = e->shown; }
        else if (stepping) { e->kind = EASE_SETTLE; e->amp = d > 0.0f ? amp : -amp; }
        else e->kind = EASE_NONE;
        e->t0 = now_ms; e->to = target;
    }
    if (e->kind == EASE_NONE) { e->shown = target; return target; }
    const uint32_t gone = now_ms - e->t0;
    if (gone >= ms) { e->kind = EASE_NONE; e->shown = target; return target; }
    *busy_ms = ms - gone;
    const float tau = (float)gone / (float)ms;
    if (e->kind == EASE_SETTLE) {
        e->shown = target + e->amp * sinf((float)M_PI * tau);
    } else {
        /* back along the fan, the way the span runs reversed -- never the short way
         * round the dial, which on a wide fan would sweep through the empty side */
        float d = target - e->from;
        if (turn > 0.0f) {
            if (span_sign >= 0) { while (d > 0.0f) d -= turn; }
            else                { while (d < 0.0f) d += turn; }
        }
        const float sm = tau * tau * (3.0f - 2.0f * tau);
        e->shown = e->from + d * sm;
    }
    return e->shown;
}

/* The settle's overshoot: 1.5 degrees on a turn, 3 px on a travel. */
#define EASE_SETTLE_DEG   1.5f
#define EASE_SETTLE_PX    3.0f

static bool cad_steps_slow(uint8_t cad, uint8_t src)
{
    return (cad == CLK_CAD_JUMP || cad == CLK_CAD_STEP) && src != CLK_SRC_SECOND;
}

/* D41 §3: one reading of the wall clock, handed to the drive function for every row
 * a frame poses. frac is the wall fraction, beat-held on a delta-bank dial. */
typedef struct {
    const struct tm *ti;
    time_t           t;
    double           frac;
} drive_now_t;

/* Sweep marking by the carriage's rotated ink chords; defined with the tourbillon
 * block below. Returns true when it placed the marks for both poses, false when the
 * caller must fall back to the swept-box union (no bank, no prior composite, or the
 * classic path). */
static bool tourbillon_ink_mark(int f, int32_t prev_ang_10, int32_t new_ang_10);
static void tourb_mark(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out);

/* A row's sprite with its pivot moved by a travel: the record every mark and every
 * paint of a travelled pose reads, so a mark and a paint never disagree. */
static void pose_spr(const clk_sprite_t *s, int16_t ox, int16_t oy, clk_sprite_t *out)
{
    *out = *s;
    out->centerx = (int16_t)(out->centerx + ox);
    out->centery = (int16_t)(out->centery + oy);
}

/* ---- tourbillon: pick the wall-clock frame and swap it in -----------------
 * The only pixels that can differ after a frame swap are the changed rects of
 * the OUTGOING and INCOMING frame at the current angle (both are chained
 * against neighbours by the baker, like the delta bank); a first-ever draw has
 * no prior composite of this layer, so it takes the full swept box. The angle
 * changes (the orbit step) are marked by rows_update itself, exactly as for
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
    const uint8_t cr = clk_theme.car_row;
    if (n <= 0 || cr >= CLK_ROW_MAX || !s_row || !s_row[cr].obj) return;
    row_state_t *st = &s_row[cr];

    const uint32_t fps = clk_theme.tourbillon_fps ? clk_theme.tourbillon_fps : 15;
    const int f = (int)(((uint64_t)(lv_tick_get() - s_tourb_t0) * fps)
                        / 1000u % (uint32_t)n);
    if (f == s_tourb_shown) return;

    /* The carriage stands at its own row's pose — the one rows_update just set on
     * it, whatever that was when the frame clock started and between its ticks. */
    const int32_t a = st->ang_10;
    clk_sprite_t fo, fn;
    pose_spr(&clk_theme.tourbillon[f], st->ox, st->oy, &fn);
    lv_area_t u;
    if (s_tourb_shown < 0 || a == ROW_HIDDEN) {
        /* First draw of this layer: no prior composite exists to diff against,
         * so the whole (rotation-0 if not posed yet) swept box goes dirty. */
        gfx_swept_box(&fn, a == ROW_HIDDEN ? 0 : a, &u);
    } else {
        pose_spr(&clk_theme.tourbillon[s_tourb_shown], st->ox, st->oy, &fo);
        tourb_mark(&fo, a, &u);
        lv_area_t v;
        tourb_mark(&fn, a, &v);
        gfx_area_union(&u, &v);
    }

    /* Mirror rows_update's etiquette: the src setter would invalidate the
     * widget's whole transformed box; the marks below cover exactly what
     * needs rebuilding. Under L4 the commit painter owns the rows outright. */
    lv_display_t *disp = lv_obj_get_display(s_scr);
    lv_display_enable_invalidation(disp, false);
    lv_image_set_src(st->obj, &s_tourb_dsc[f]);
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
    if (s_tourb_shown >= 0 && a != ROW_HIDDEN && !st->ox && !st->oy)
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
 * Since D43 §5.4 both sides ROUND that blend: orb_gfx.c's mix_16_16 and
 * LVGL's RGB565 image blends through main/gfx/orb_lv_blend.c's hooks.
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
 * ghost the hands paid for before the redraw gate learned to hold its angles. */
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
 * same widgets the L4 hook mirrors. clock_view_init calls this at the comp's place
 * in the depth walk, because creation order is the classic path's only notion of
 * z-order and a window must sit where its depth says: under the live layer that
 * frames it, over the plate it is cut into. */
static void complication_create(const orb_theme_geo_t *g, uint16_t i)
{
    const clk_comp_t *c = &g->comps[i];
    if (!c->show || (c->flags & CLK_FLAG_DISABLED)) return;
    if (s_comp_obj[i][0]) return;                     /* created already */
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

static void complications_reset(void)
{
    memset(s_comp_obj,   0,    sizeof s_comp_obj);
    memset(s_comp_frame, 0xFF, sizeof s_comp_frame);      /* -1: nothing drawn yet */
}


/* ---- D42: the moving rows -------------------------------------------------------
 *
 * One clk_row_t is a sprite plus how it moves (docs/element-model-plan.md §3): its
 * art sits at P0 — the row's own placement, turned to `zero` — and across its domain
 * it turns a further `span` and its pivot travels (dx, dy) to P1. The value comes
 * from comp_value(), so a date ring and the date WINDOW can never disagree about
 * what day it is, and every row reads the one struct tm compose_now sampled.
 *
 * A shadowed row is TWO rows: the row, then its blur/offset twin flagged
 * CLK_FLAG_AUX_SHADOW, adjacent in slot order (orbtheme-baker.js packs the pair;
 * row flag bit 4 — AUX_F_SHADOW3, mapped in orb_clock3.c — carries it into this
 * vocabulary). The twin owns pixels and a centre (its row's, moved by the light
 * vector) and no input of its own — the tick gives it its row's whole pose — so it
 * can never come loose from the pixels it exists to darken: row and twin turn as
 * one rigid pair, and the light's offset stays fixed in screen space.
 *
 * Depth is one number per row, on the scale the complication windows share, and
 * both paint paths stack by it (clock_view_init's creation walk and the layer
 * table's sort read the same key). */

/* How far into the current unit of `src` the wall clock is, 0..1, and that unit's
 * length in minutes (0 for a source with no finer field: battery, reserve, a moon
 * age, a week number — on those every cadence lands whole). */
static double drive_finer(uint8_t src, const struct tm *ti, double frac, double *unit_min)
{
    const double day = ((double)ti->tm_hour * 3600.0 + ti->tm_min * 60.0 + ti->tm_sec + frac)
                       / 86400.0;
    switch (src) {
    case CLK_SRC_SECOND:  *unit_min = 1.0 / 60.0; return frac;
    case CLK_SRC_MINUTE:  *unit_min = 1.0;        return (ti->tm_sec + frac) / 60.0;
    case CLK_SRC_HOUR12:
    case CLK_SRC_HOUR24:  *unit_min = 60.0;
                          return (ti->tm_min * 60.0 + ti->tm_sec + frac) / 3600.0;
    case CLK_SRC_DATE:
    case CLK_SRC_WEEKDAY:
    case CLK_SRC_DAY_OF_YEAR: *unit_min = 1440.0; return day;
    case CLK_SRC_MONTH: {
        static const uint8_t DIM[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int dim = DIM[ti->tm_mon % 12];
        if (ti->tm_mon == 1 && comp_year_leap(ti->tm_year + 1900)) dim = 29;
        *unit_min = 1440.0 * dim;
        return (ti->tm_mday - 1 + day) / dim; }
    default:              *unit_min = 0.0;        return 0.0;
    }
}

/* The value a row shows, in units of its source: day 5 swept to noon is 5.5. A digit
 * row reads one decimal digit (D42 §4): the units ring turns with the whole value's
 * finer field, and the tens ring's finer field is the units it is counting toward.
 * False when this orb has no input for the source. */
static bool drive_units(const clk_row_t *r, const drive_now_t *n, double *out)
{
    int32_t v;
    if (!comp_value(r->src, n->ti, n->t, &v)) return false;
    double unit_min;
    double f = drive_finer(r->src, n->ti, n->frac, &unit_min);
    if (r->digit == CLK_DIGIT_UNITS) {
        v %= 10;
    } else if (r->digit == CLK_DIGIT_TENS) {
        f = ((v % 10) + f) / 10.0;
        unit_min *= 10.0;
        v = (v / 10) % 10;
    }
    double shown = 0.0;
    switch (r->cadence) {
    case CLK_CAD_SWEEP:
        shown = f;
        break;
    case CLK_CAD_STEP: {
        const unsigned steps = r->cad_arg ? r->cad_arg : 1;   /* refused 0 at the door */
        shown = floor(f * steps) / steps;
        break; }
    case CLK_CAD_STOP_TO_GO: {
        /* The whole dial in P tenths of a second, then a rest at the top until the
         * minute releases it: the value runs fast and stops at value_max. */
        const double p = r->cad_arg ? r->cad_arg : 585;
        const double run = (v + f) * 600.0 / p;
        *out = run < r->value_max ? run : r->value_max;
        return true; }
    case CLK_CAD_DRAG: {
        /* Held, then a creep across the last cad_arg minutes of the unit. */
        const double w = r->cad_arg ? r->cad_arg : 120;
        if (unit_min > 0.0) {
            const double into = f * unit_min - (unit_min - (w < unit_min ? w : unit_min));
            shown = into > 0.0 ? into / (w < unit_min ? w : unit_min) : 0.0;
        }
        break; }
    default:                                    /* JUMP: the boundary only */
        break;
    }
    *out = v + shown;
    return true;
}

/* Units carried to the fraction t of the way from P0 to P1 (D42 §3): t = (u - vmin) /
 * (vmax - vmin). A wrap takes the fraction modulo one — the next unit starts at P0 —
 * and a flyback or a hold clamps it, the source's own reset bringing it home. */
static double drive_t(const clk_row_t *r, double units)
{
    const double w = (double)r->value_max - (double)r->value_min;
    double t = w > 0.0 ? (units - (double)r->value_min) / w : 0.0;
    if (r->end == CLK_END_WRAP) {
        t -= floor(t);
    } else {
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
    }
    return t;
}

/* The pose at fraction t: an angle in tenths (half-up, wrapped into the turn) and
 * the pivot's travel in px. */
static void row_pose(const clk_row_t *r, double t, int32_t *ang_10, int16_t *ox, int16_t *oy)
{
    const double a = (double)r->zero + t * (double)r->span;
    int32_t ai = (int32_t)(a >= 0.0 ? floor(a + 0.5) : -floor(-a + 0.5));
    ai %= 3600;
    if (ai < 0) ai += 3600;
    *ang_10 = ai;
    const double x = t * (double)r->dx, y = t * (double)r->dy;
    *ox = (int16_t)(x >= 0.0 ? floor(x + 0.5) : -floor(-x + 0.5));
    *oy = (int16_t)(y >= 0.0 ? floor(y + 0.5) : -floor(-y + 0.5));
}

/* D41's ease, in fraction space: a settle overshoots by 1.5 degrees (or 3 px on a
 * row that only travels) after a jump or a step slower than the second; a flyback
 * runs back along the path when a flyback row's source resets. A wrapping row's
 * fraction wraps, so a minute hand's 59 -> 0 settles forward, never back round. */
static double row_ease(const clk_row_t *r, ease_t *e, double t, uint32_t now, uint32_t *busy)
{
    *busy = 0;
    if (!r->ease) { e->init = false; return t; }
    const int32_t trav = abs(r->dx) > abs(r->dy) ? abs(r->dx) : abs(r->dy);
    const float amp = r->span ? EASE_SETTLE_DEG * 10.0f / (float)abs(r->span)
                              : (trav ? EASE_SETTLE_PX / (float)trav : 0.0f);
    float v = ease_pose(e, (float)t, now, r->ease, cad_steps_slow(r->cadence, r->src),
                        r->end == CLK_END_FLYBACK, 1, r->end == CLK_END_WRAP ? 1.0f : 0.0f,
                        amp, busy);
    if (r->end == CLK_END_WRAP) v -= floorf(v);
    return v;
}

/* The fps bucket a row's blends count under on the scheduler's line: the hour,
 * minute and seconds slots the report has always named, the still layers, and the
 * rest as pointers. A label only — nothing paints by it. */
static uint8_t row_role(const clk_row_t *r)
{
    switch (r->src) {
    case CLK_SRC_HOUR12: case CLK_SRC_HOUR24: return 0;
    case CLK_SRC_MINUTE: return 1;
    case CLK_SRC_SECOND: return 2;
    case CLK_SRC_NONE:   return 3;
    default:             return CLK_ROLE_AUX;
    }
}

/* The sprite a row draws: the carriage's shown frame, or the row's own plate. */
static const clk_sprite_t *row_spr(uint16_t i)
{
    const clk_row_t *r = &orb_theme_geo()->rows[i];
    if (i == clk_theme.car_row && clk_theme.tourbillon_n)
        return &clk_theme.tourbillon[s_tourb_shown >= 0 ? s_tourb_shown : 0];
    return &r->spr;
}

/* The box a row covers at a pose. */
static void row_mark_box(uint16_t i, int32_t ang_10, int16_t ox, int16_t oy, lv_area_t *out)
{
    clk_sprite_t p;
    pose_spr(row_spr(i), ox, oy, &p);
    gfx_swept_box(&p, ang_10, out);
}

#define ROW_REDRAW_EPS_10 2

/* The per-tick pass. Every row's own pose first, then the poses it rides (a parent's,
 * two links at most — the projection refused deeper), then each twin takes its row's,
 * then each row that moved marks the box it vacates and the one it takes, and its
 * widget follows. A row that stands still costs one compare.
 *
 * HARD GATE for sweeping dials (the hands' rule since the first port): re-posing a
 * row invalidates its whole rotated box, so a slow sweep — a minute hand advancing a
 * fraction of a pixel a tick — re-poses only once its angle has left the drawn one by
 * 0.2 degrees (~0.4 px at the longest tip), catching up exactly. The carriage's orbit
 * marks its rotated ink chords instead of the box when it can (tourbillon_ink_mark). */
static void rows_update(const struct tm *ti, time_t t, double frac)
{
    const orb_theme_geo_t *g = orb_theme_geo();
    if (!s_row || !g->rows) return;
    const uint16_t n = g->n_rows < CLK_ROW_MAX ? g->n_rows : CLK_ROW_MAX;
    int32_t ang[CLK_ROW_MAX];
    int16_t ox[CLK_ROW_MAX], oy[CLK_ROW_MAX];
    const drive_now_t now = { .ti = ti, .t = t, .frac = frac };
    const uint32_t tick = lv_tick_get();
    uint32_t want = 0;

    for (uint16_t i = 0; i < n; i++) {
        const clk_row_t *r = &g->rows[i];
        ox[i] = oy[i] = 0;
        if (!r->spr.show && i != clk_theme.car_row) { ang[i] = ROW_HIDDEN; continue; }
        if (r->src == CLK_SRC_NONE) { ang[i] = 0; continue; }   /* a still row: unturned */
        double u;
        if (!drive_units(r, &now, &u)) {
            ang[i] = ROW_HIDDEN;
            if (!s_row[i].logged) {
                s_row[i].logged = true;
                ESP_LOGW(TAG, "row %u: source %u has no input on this device — not drawn",
                         (unsigned)r->tag, (unsigned)r->src);
            }
            continue;
        }
        uint32_t busy = 0;
        const double tf = row_ease(r, &s_row[i].ease, drive_t(r, u), tick, &busy);
        if (busy > want) want = busy;
        row_pose(r, tf, &ang[i], &ox[i], &oy[i]);
    }
    if (want) orb_sched_burst(want + 40);
    /* The poses rows ride: a child adds its parent's own pose — eased, as drawn — and
     * the grandparent's, two links at most (the projection refused deeper), so a
     * needle on the carriage turns with the carriage. Every row's art is stored
     * unturned, so the parent's whole angle is what the child rides, not its turn
     * since P0. */
    int32_t own_a[CLK_ROW_MAX];
    int16_t own_x[CLK_ROW_MAX], own_y[CLK_ROW_MAX];
    memcpy(own_a, ang, sizeof(int32_t) * n);
    memcpy(own_x, ox, sizeof(int16_t) * n);
    memcpy(own_y, oy, sizeof(int16_t) * n);
    for (uint16_t i = 0; i < n; i++) {
        if (!g->rows[i].parent_row || ang[i] == ROW_HIDDEN) continue;
        for (uint16_t p = g->rows[i].parent_row, hops = 0; p && hops < 2; hops++) {
            const uint16_t pi = (uint16_t)(p - 1u);
            if (pi >= n || own_a[pi] == ROW_HIDDEN) break;
            ang[i] = (ang[i] + own_a[pi]) % 3600;
            ox[i] = (int16_t)(ox[i] + own_x[pi]);
            oy[i] = (int16_t)(oy[i] + own_y[pi]);
            p = g->rows[pi].parent_row;
        }
    }
    for (uint16_t i = 1; i < n; i++)
        if (g->rows[i].flags & CLK_FLAG_AUX_SHADOW) {
            ang[i] = ang[i - 1]; ox[i] = ox[i - 1]; oy[i] = oy[i - 1];
        }

    lv_display_t *disp = lv_obj_get_display(s_scr);
    for (uint16_t i = 0; i < n; i++) {
        row_state_t *st = &s_row[i];
        const int32_t a = ang[i], prev = st->ang_10;
        if (a == prev && ox[i] == st->ox && oy[i] == st->oy) continue;
        if (a != ROW_HIDDEN && prev != ROW_HIDDEN && ox[i] == st->ox && oy[i] == st->oy) {
            int32_t d = a - prev;
            if (d > 1800) d -= 3600;
            if (d < -1800) d += 3600;
            if (abs((int)d) < ROW_REDRAW_EPS_10) continue;
        }

        /* The wedge to rebuild: where the row was and where it is going. The carriage
         * alone on its arbour marks its rotated ink instead of the box when it can. */
        lv_area_t u;
        bool have = false;
        const bool car = i == clk_theme.car_row && clk_theme.tourbillon_n;
        const bool inked = car && prev != ROW_HIDDEN && a != ROW_HIDDEN &&
                           !ox[i] && !oy[i] && !st->ox && !st->oy &&
                           tourbillon_ink_mark((int)s_tourb_shown, prev, a);
        if (!inked) {
            if (prev != ROW_HIDDEN) { row_mark_box(i, prev, st->ox, st->oy, &u); have = true; }
            if (a != ROW_HIDDEN) {
                lv_area_t nn;
                row_mark_box(i, a, ox[i], oy[i], &nn);
                if (have) gfx_area_union(&u, &nn); else { u = nn; have = true; }
            }
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
        /* Stage the INCOMING pose before anything reads it: the commit hook and the
         * classic widget both draw from it, and a mark computed from one travel with a
         * paint reading another is the ghost the swept-box contract exists to prevent. */
        st->ang_10 = a;
        st->ox = ox[i]; st->oy = oy[i];
        if (a != ROW_HIDDEN) pose_spr(&g->rows[i].spr, ox[i], oy[i], &st->pose);
        if (!st->obj) continue;
        /* One pose, two setters, with LVGL's own invalidation off: a rotation setter
         * invalidates the FULL transformed box of both poses (the 36-48 ms a rotating
         * layer used to cost every tick), and the marks above already cover the change.
         * The first pose ever needs LVGL's own render, because nothing has painted
         * these pixels yet. */
        if (a == ROW_HIDDEN) { lv_obj_set_hidden(st->obj, true); continue; }
        if (prev == ROW_HIDDEN) lv_obj_set_hidden(st->obj, false);
        else lv_display_enable_invalidation(disp, false);
        const clk_sprite_t *sp = row_spr(i);
        lv_obj_set_pos(st->obj, (lv_coord_t)(sp->centerx - sp->pivotx + ox[i]),
                       (lv_coord_t)(sp->centery - sp->pivoty + oy[i]));
        lv_image_set_rotation(st->obj, a);
        if (prev != ROW_HIDDEN) lv_display_enable_invalidation(disp, true);
    }
}

/* ---- D39: the clock's sound policy (docs/sound-plan.md §5) --------------------
 *
 * The theme carries the sounds (orb_theme_sounds), the OS plays them (orb_sound),
 * and this is the part that is the clock's own: which sound, and when. It runs off
 * compose_now's one wall reading, so a chime and the hands agree on the minute.
 *
 *   - The tick loop runs while the dial ticks, anchored to a wall second so beat k
 *     lands with step k of the seconds hand (§4.2). The projection already refused
 *     a loop whose beat disagrees with a stepping hand.
 *   - At a minute boundary: :15/:30/:45 sound the quarter chimes, :00 the hour
 *     chime and then the hour pattern (§5.1), each queued for its exact instant.
 *   - Every minute, hour and date change sounds its jump slot when the theme
 *     carries one (D41's step events).
 *
 * Every voice is LEASED and the lease is renewed each tick, so when the scheduler
 * parks this dial the sounds stop with it. A boundary seen more than two seconds
 * late, or a clock that jumped (an SNTP step), sounds nothing: a chime belongs to
 * the instant, not to the catching-up.
 *
 * State lives in PSRAM behind one pointer: this board has no internal RAM to spare
 * (orb_sound.c's header has the measurement). */
typedef struct {
    int     loop;            /* the tick loop's handle, 0 = not running */
    int64_t loop_anchor;
    int     min, hour, mday; /* the last boundary seen; -1 = none yet */
    time_t  last_t;
    int64_t queued[3];       /* SND_STEP_*: the step already queued, by wall identity */
    uint8_t steps[3];        /* SND_STEP_*: steps per unit of a row that steps, 0 = none */
    bool    steps_known;
} snd_state_t;
static snd_state_t *s_snd;

/* D41 G5: a jump slot is the sound of a row stepping (docs/sound-plan.md §3.1), so it
 * plays only on a dial where some row reading that source steps, and on the instant
 * it does. Three classes, one per slot: minute, hour, calendar. */
enum { SND_STEP_MIN, SND_STEP_HOUR, SND_STEP_CAL };

static int snd_step_class(uint8_t src)
{
    switch (src) {
    case CLK_SRC_MINUTE: return SND_STEP_MIN;
    case CLK_SRC_HOUR12: case CLK_SRC_HOUR24: return SND_STEP_HOUR;
    case CLK_SRC_DATE: case CLK_SRC_WEEKDAY: case CLK_SRC_MONTH: case CLK_SRC_DAY_OF_YEAR:
        return SND_STEP_CAL;
    default: return -1;
    }
}

/* Steps per unit a cadence makes: jump is one, step N is N, everything else (sweep,
 * stop-to-go, drag) has no single instant to sound. */
static uint8_t snd_cad_steps(uint8_t cad, uint16_t arg)
{
    if (cad == CLK_CAD_JUMP) return 1;
    if (cad == CLK_CAD_STEP) return (uint8_t)(arg ? (arg > 60 ? 60 : arg) : 1);
    return 0;
}

static void snd_note_step(uint8_t out[3], int cls, uint8_t n)
{
    if (cls >= 0 && n > out[cls]) out[cls] = n;
}

/* What steps on this dial: every drawn row's cadence, and every calendar plate, which
 * turns whole values at midnight by construction. A digit row steps with its source:
 * a date's units ring jumps at midnight like the date. */
static void snd_steps_scan(uint8_t out[3])
{
    memset(out, 0, 3);
    const orb_theme_geo_t *g = orb_theme_geo();
    for (uint16_t i = 0; i < g->n_rows && i < CLK_ROW_MAX; i++) {
        const clk_row_t *r = &g->rows[i];
        if ((r->flags & CLK_FLAG_AUX_SHADOW) || !r->spr.show) continue;
        snd_note_step(out, snd_step_class(r->src), snd_cad_steps(r->cadence, r->cad_arg));
    }
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++)
        if (snd_step_class(g->comps[i].src) == SND_STEP_CAL) out[SND_STEP_CAL] = 1;
}
static orb_app_t   *s_lay_app;   /* the clock's row (defined with the layer table below) */

static const clk_sound_t *snd_find(const clk_sound_t *t, uint32_t n, int slot)
{
    for (uint32_t i = 0; i < n; i++)
        if (t[i].slot == slot) return &t[i];
    return NULL;
}

static int64_t snd_us(const clk_sound_t *s)      /* a sound's length in µs */
{
    return (int64_t)s->n * 1000000 / CLK_SND_RATE;
}

/* The clock's voices are owned by its row, so the registry's suspend silences them
 * (orb_svc_app_suspended), and leased, so a parked tick does too. */
static void snd_at_flags(const clk_sound_t *s, int64_t at_us, uint32_t extra)
{
    if (!s) return;
    orb_sound_play_owned(s_lay_app, (const int16_t *)(const void *)(blob() + s->pcm_off),
                         s->n, ORB_SOUND_GAIN_1, ORB_SOUND_LEASED | extra, at_us, 0);
}

static void snd_at(const clk_sound_t *s, int64_t at_us) { snd_at_flags(s, at_us, 0); }

/* D19 for sound: the clock declares the cap, and the owner may revoke it on the
 * dial like any other. Read the grant word, not the counting gate: this runs every
 * tick, and a revoked cap is a state, not a stream of refusals. */
static bool snd_granted(void)
{
    return s_lay_app && (orb_svc_grants(s_lay_app) & (1u << CAP_SOUND));
}

/* The pattern the owner hears: theirs when the theme carries its strokes, else the
 * theme's own. */
static int snd_pattern(const clk_sound_t *t, uint32_t n, uint8_t theme_pat)
{
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    const int p = c.pattern;
    const bool strike = snd_find(t, n, CLK_SND_STRIKE_HOUR) != NULL;
    switch (p) {
    case CLK_PAT_COUNT: case CLK_PAT_SHIPS_BELLS:
        return strike ? p : theme_pat;
    case CLK_PAT_MORSE:
        return (snd_find(t, n, CLK_SND_MORSE_DIT) && snd_find(t, n, CLK_SND_MORSE_DAH))
               ? p : theme_pat;
    case CLK_PAT_REPEATER:
        return (strike && snd_find(t, n, CLK_SND_STRIKE_QUARTER) &&
                snd_find(t, n, CLK_SND_STRIKE_MINUTE)) ? p : theme_pat;
    case CLK_PAT_NONE:
        return p;
    default:
        return theme_pat;
    }
}

/* The hour pattern from `t0`. h12 is 1..12; minute is 0 or 30 (ship's bells only). */
static void snd_hour_pattern(const clk_sound_t *t, uint32_t n, int pat,
                             int64_t t0, int h12, int hour24, int minute)
{
    const clk_sound_t *stroke = snd_find(t, n, CLK_SND_STRIKE_HOUR);
    switch (pat) {
    case CLK_PAT_COUNT:
        for (int i = 0; i < h12; i++) snd_at(stroke, t0 + (int64_t)i * 2000000);
        break;
    case CLK_PAT_SHIPS_BELLS: {
        /* Four-hour watches: 00:30 is one bell, 04:00 eight, 04:30 one again. */
        int bells = (hour24 % 4) * 2 + (minute >= 30 ? 1 : 0);
        if (!bells) bells = 8;
        for (int i = 0; i < bells; i++)
            snd_at(stroke, t0 + (int64_t)(i / 2) * 1600000 + (i % 2) * 450000);
        break; }
    case CLK_PAT_MORSE: {
        const clk_sound_t *dit = snd_find(t, n, CLK_SND_MORSE_DIT);
        const clk_sound_t *dah = snd_find(t, n, CLK_SND_MORSE_DAH);
        static const char *const DIGIT[10] = {
            "-----", ".----", "..---", "...--", "....-",
            ".....", "-....", "--...", "---..", "----.",
        };
        const int64_t unit = snd_us(dit);
        char num[12];
        snprintf(num, sizeof num, "%d", h12);
        int64_t at = t0;
        for (const char *d = num; *d; d++) {
            for (const char *e = DIGIT[*d - '0']; *e; e++) {
                const clk_sound_t *sym = *e == '.' ? dit : dah;
                snd_at(sym, at);
                at += snd_us(sym) + unit;              /* one unit between symbols */
            }
            at += 2 * unit;                            /* three between digits */
        }
        break; }
    case CLK_PAT_REPEATER:
        /* At the hour a repeater has no quarters and no minutes to sound. */
        for (int i = 0; i < h12; i++) snd_at(stroke, t0 + (int64_t)i * 900000);
        break;
    default:
        break;
    }
}

static void sound_policy(const struct tm *ti, time_t t, double frac)
{
    if (!orb_sound_ready()) return;
    if (!s_snd) {
        s_snd = heap_caps_calloc(1, sizeof *s_snd, MALLOC_CAP_SPIRAM);
        if (!s_snd) return;
        s_snd->min = s_snd->hour = s_snd->mday = -1;

    }
    orb_sound_lease_renew();
    if (!snd_granted()) {
        if (s_snd->loop) { orb_sound_stop(s_snd->loop); s_snd->loop = 0; }
        return;
    }

    uint32_t n = 0;
    uint8_t theme_pat = CLK_PAT_NONE;
    const clk_sound_t *tab = orb_theme_sounds(&n, &theme_pat);
    orb_sound_cfg_t c;
    orb_sound_cfg_get(&c);
    const bool quiet = c.volume == 0 || orb_sound_quiet_at(ti->tm_hour);
    const int64_t sec_us = esp_timer_get_time() - (int64_t)(frac * 1e6);   /* this second began */

    /* The tick loop. */
    const clk_sound_t *loop = snd_find(tab, n, CLK_SND_TICK_LOOP);
    const bool want = loop && c.tick && !quiet;
    if (s_snd->loop && (!want || !orb_sound_active(s_snd->loop))) {
        orb_sound_stop(s_snd->loop);
        s_snd->loop = 0;
    }
    if (want) {
        /* Sample first_beat sounds on a wall second; the loop is a whole number of
         * seconds long, so any second is the anchor. Re-anchor when the wall clock
         * itself was re-anchored under the loop (orb_time_now's drift rule). */
        const int64_t anchor = sec_us - (int64_t)loop->first_beat * 1000000 / CLK_SND_RATE;
        if (!s_snd->loop) {
            s_snd->loop = orb_sound_play_owned(s_lay_app,
                                         (const int16_t *)(const void *)(blob() + loop->pcm_off),
                                         loop->n, ORB_SOUND_GAIN_1,
                                         ORB_SOUND_LOOP | ORB_SOUND_ANCHORED | ORB_SOUND_LEASED,
                                         0, anchor);
            if (s_snd->loop < 0) s_snd->loop = 0;
            s_snd->loop_anchor = anchor;
        } else {
            int64_t d = (anchor - s_snd->loop_anchor) % 1000000;
            if (d < 0) d += 1000000;
            if (d > 2000 && d < 998000) {
                orb_sound_reanchor(s_snd->loop, anchor);
                s_snd->loop_anchor = anchor;
            }
        }
    }

    /* D41 G5: a jump slot is queued AHEAD of its step, for the step's own instant.
     * Queueing wakes the engine, so the amp is up and settled long before the sound:
     * started at the boundary instead, a dial with no tick loop woke the amp under
     * the clunk's first 10 ms -- all of its energy -- and the owner heard nothing
     * (orb-B, Station, 2026-11-01). The lead is 1.5 s; the instant is exact. */
    if (!s_snd->steps_known) { snd_steps_scan(s_snd->steps); s_snd->steps_known = true; }
    if (!quiet && n && c.jumps) {
        const int64_t minute_us = sec_us - (int64_t)ti->tm_sec * 1000000;
        const double into = ti->tm_sec + frac;
        const int64_t next_min_us = minute_us + 60000000;
        const int nmin = (ti->tm_min + 1) % 60;
        /* A step is named by wall time, never by its microsecond instant: sec_us is
         * re-read every tick, so an instant compared by value would queue anew on
         * every frame. */
        const int64_t wall_min = (int64_t)(t - ti->tm_sec);   /* this minute's epoch second */
        const uint8_t mn = s_snd->steps[SND_STEP_MIN], hn = s_snd->steps[SND_STEP_HOUR];
        if (mn) {
            const int idx = (int)(into * mn / 60.0) + 1;          /* the next step, 1..mn */
            const double at_s = idx * 60.0 / mn;
            const int64_t at_us = minute_us + (int64_t)(at_s * 1e6);
            const int64_t key = wall_min * 64 + idx;
            if (at_s - into <= 1.5 && s_snd->queued[SND_STEP_MIN] != key) {
                s_snd->queued[SND_STEP_MIN] = key;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_MINUTE), at_us);
            }
        }
        if (into >= 58.5) {
            if (hn && (nmin == 0 || (nmin * hn) / 60 != (ti->tm_min * hn) / 60) &&
                s_snd->queued[SND_STEP_HOUR] != wall_min) {
                s_snd->queued[SND_STEP_HOUR] = wall_min;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_HOUR), next_min_us);
            }
            if (s_snd->steps[SND_STEP_CAL] && ti->tm_hour == 23 && ti->tm_min == 59 &&
                s_snd->queued[SND_STEP_CAL] != wall_min) {
                s_snd->queued[SND_STEP_CAL] = wall_min;
                snd_at(snd_find(tab, n, CLK_SND_JUMP_DATE), next_min_us);
            }
        }
    }

    /* Boundaries. The first reading after boot, and any reading that is not the
     * very next minute, only records where the clock is. */
    const bool first = s_snd->min < 0;
    const bool next = !first && t - s_snd->last_t < 120 &&
                      ti->tm_min == (s_snd->min + 1) % 60;
    const bool changed = first || ti->tm_min != s_snd->min;
    const int prev_hour = s_snd->hour, prev_mday = s_snd->mday;
    s_snd->last_t = t;
    if (!changed) return;
    s_snd->min = ti->tm_min; s_snd->hour = ti->tm_hour; s_snd->mday = ti->tm_mday;
    if (!next || ti->tm_sec > 2 || quiet || !n) return;

    const int64_t at = sec_us - (int64_t)ti->tm_sec * 1000000;   /* the minute's instant */
    /* The jump slots were queued ahead of this boundary (above). */
    (void)prev_hour; (void)prev_mday;
    const int pat = snd_pattern(tab, n, theme_pat);
    const int h12 = ti->tm_hour % 12 ? ti->tm_hour % 12 : 12;
    if (pat == CLK_PAT_SHIPS_BELLS) {
        /* Ship's bells replace the quarters: every half hour, nothing between. */
        if (c.strikes && (ti->tm_min == 0 || ti->tm_min == 30))
            snd_hour_pattern(tab, n, pat, at, h12, ti->tm_hour, ti->tm_min);
        return;
    }
    if (c.chimes) {
        if (ti->tm_min == 15) snd_at(snd_find(tab, n, CLK_SND_CHIME_Q1), at);
        if (ti->tm_min == 30) snd_at(snd_find(tab, n, CLK_SND_CHIME_HALF), at);
        if (ti->tm_min == 45) snd_at(snd_find(tab, n, CLK_SND_CHIME_Q3), at);
    }
    if (ti->tm_min == 0) {
        const clk_sound_t *ch = c.chimes ? snd_find(tab, n, CLK_SND_CHIME_HOUR) : NULL;
        snd_at(ch, at);
        /* The strokes follow the chime's own length and a breath. */
        const int64_t t0 = at + (ch ? snd_us(ch) + 400000 : 0);
        if (c.strikes) snd_hour_pattern(tab, n, pat, t0, h12, ti->tm_hour, 0);
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

    /* Delta-bank cadence: the tick delivers exactly one bank frame per tick, so a
     * sweeping dial's beat rides the TICK COUNTER, not a wall-clock grid. The dial's
     * beat (tick_rate, derived from its seconds rows) becomes ticks-per-beat
     * (hand_beat_ticks): 4 bps on a 12 fps tick = sample every 3rd tick, always the
     * 3rd — evenly spaced beats even though 250 ms is not a multiple of the ~83 ms
     * the pipe delivers. On each beat the fraction is sampled from the wall clock,
     * so the rows read true time (re-anchored at every second boundary); between
     * beats it holds, the redraw gate skips the re-pose, and the tick's commit
     * carries bank rows only. Every row reads the held fraction, so a sub-dial
     * seconds hand beats with the main one. */
    const double wall_frac = frac;
    if (clk_theme.second_sweep && clk_theme.tick_rate > 0 && orb_bg_gif_mode()) {
        const uint32_t tpb = hand_beat_ticks();
        if (tpb > 1) {
            if (ti.tm_sec != s_bt_sec) {        /* true seconds re-anchor the phase */
                s_bt_sec = ti.tm_sec;
                s_bt_ctr = 0;
            }
            if ((s_bt_ctr % tpb) == 0) s_bt_frac = frac;
            s_bt_ctr++;
            frac = s_bt_frac;
        }
    }

    /* Every moving row, off one reading: a date ring and the date window are two
     * views of one tm_mday, and the tick that moved one has moved the other. */
    rows_update(&ti, t, frac);

    /* The complication pass: every window re-reads its value from the SAME local
     * minute the rows were sampled from, and only the comps whose selected frame
     * actually changed dirty their rows. */
    comps_update(&ti, t);

    /* D39: the sounds, off the same reading the rows just took. */
    sound_policy(&ti, t, wall_frac);
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
 * and a row for it would blend the background over its own restore), a sound,
 * and a switched-off row (EF_HIDDEN), which is a theme's own off switch and not a
 * refusal. A still layer the bake folded into the plate is simply not shipped.
 */

/* One claimed row: the identity the bundle named, what the pose pass needs
 * to derive its current pose, and the descriptor as last pushed through the
 * gate — the pose shadow the per-tick diff reads. Commit 4 grew the record
 * for exactly that; Commit 3 only remembered (cls, slot). */
typedef struct {
    uint16_t cls, slot;        /* the identity, echoed back for logs          */
    uint16_t frame0, n_frames; /* the element's own frame window into the bank */
    int16_t  x, y;             /* theme placement — what a slide displaces     */
    uint8_t  kind;             /* BANK: CLK_KIND_*; AUX: CLK_SHAPE_*; else 0   */
    uint8_t  gidx;             /* BANK/AUX: this row's index in the geo table
                                * the tick state is indexed by; 0xFF = none */
    orb_layer_desc_t d;        /* the pose the table holds right now          */
} clk_layer_key_t;
#define CLK_EF_AUX_TWIN   0x10u  /* theme_schema EF_AUX_SHADOW, the twin bit */
#define CLK_LAYER_KEYS_MAX (2 + CLK_ROW_MAX + CLK_COMP_MAX)
                                /* the clock's whole drawn vocabulary: 2 plates, the
                                 * moving rows, 16 complications. An ADOPTED table
                                 * cannot exceed it — orb_clock3.c refuses a row
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
/* s_lay_app — the row the table is claimed under — is declared above sound_policy. */
/* The section and bank base the rows' frame offsets resolve against. The
 * adopted bundle changes only at boot — the store door stages, the flip
 * worker reboots — and this pass rebuilds on any re-show, so caching what
 * clock_layers_build opened is safe. NULL with s_lay_n stays louder than a
 * wrong answer: the pass keeps the glass it has. */
static const orb_bundle_sec_t *s_lay_sec;
static uint32_t s_lay_bank_off;

#define CLK_EF_HIDDEN  0x08u    /* theme_schema EF_HIDDEN, the mirror orb_clock3.c keeps */

/* The one stacking key both paint paths sort by (D42): the row's depth, and at a tie
 * a shadow twin goes under every row at that depth, so it falls beneath the row
 * casting it. Equal keys keep table order. clock_view_init's creation walk and
 * clock_layers_build's sort both call this, so a row LVGL renders and a row the L4
 * hook paints can never stack differently. */
static uint16_t depth_key(uint8_t order, bool twin)
{
    return (uint16_t)((uint16_t)order * 2u + (twin ? 0u : 1u));
}

/* One element's row. The descriptor's own placement, pivot and blend stay
 * zero: the resolver's box and pivot answer them (orb_layers.c's recipe); the
 * FORMAT 3 dialect carries no blend column (a shadow is its own alpha plate);
 * and the angle starts at the theme's fixed degrees — the wall angle is what
 * Commit 4's tick adds, per row, per beat. */
static void clk_row_desc(const orb_elem_t *e, uint32_t bank_off, uint8_t role,
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
    d->art.prim    = (uint16_t)orb_asset_class_prim((orb_asset_class_t)e->cls);
    d->art.global  = 0;                               /* the clock's namespace  */
    d->art.off     = bank_off + e->off;               /* the fold — see above   */
    d->art.bytes   = e->size;
    d->ang_10      = (int16_t)(e->degrees / 10u);     /* hundredths -> tenths   */
    d->show        = 0;                               /* Commit 4 wakes it      */
    d->role        = role;
    d->gen_prim    = 0xFFu;                           /* the answer's own prim  */
    d->alpha       = 255;
    /* The painter's half of row_aa's answer: the bake's bit, on a moving row. */
    d->smooth      = e->cls == ORB_ASSET_AUX && (e->flags & CLK_EF_SMOOTH) ? 1u : 0u;
}

/* The claim, handed to the OS, and the verdict it buys printed beside it: one
 * line per build pass — first boot and every re-apply — so a capture names the
 * path the dial drew through rather than leaving it to be inferred. */
static void clock_layers_claim(int rows, int refused)
{
    orb_sched_set_layer_claim(rows, refused);
#if CLK_BG_DIRECT_PUSH
    const bool l4 = orb_sched_l4_active();
    ESP_LOGI(TAG, "render path: l4=%s src=%s rows=%d refused=%d", l4 ? "yes" : "no",
             !l4 ? "lvgl" : (orb_bg_gif_mode() ? "psram" : "xip"), rows, refused);
#else
    ESP_LOGI(TAG, "render path: l4=no src=lvgl rows=%d refused=%d (direct push off)",
             rows, refused);
#endif
}

static void clock_layers_build(orb_app_t *self)
{
    /* Reset before either early exit below: a build that claims nothing must not
     * leave the previous dial's claim answering the gate. */
    orb_sched_set_layer_claim(0, 0);
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

    const orb_bundle_sec_t *sec = NULL;
    uint32_t bank_off = 0;
    if (!orb_theme_live_sec(ORB_CLOCK3_SLUG, &sec, &bank_off)) {
        clock_layers_claim(0, 0);   /* no adopted bundle: no rows. The base face claims none. */
        return;
    }
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
        clock_layers_claim(0, 0);
        return;
    }
    s_lay_sec = sec; s_lay_bank_off = bank_off;
    const orb_theme_geo_t *g_geo = orb_theme_geo();
    const int n_comps_geo = (int)g_geo->n_comps  < CLK_COMP_MAX
                            ? (int)g_geo->n_comps  : CLK_COMP_MAX;
    const int n_rows_geo  = (int)g_geo->n_rows    < CLK_ROW_MAX
                            ? (int)g_geo->n_rows   : CLK_ROW_MAX;

    /* Depth first, table order second (depth_key, the classic path's key too).
     * The insert sort is over at most CLK_LAYER_KEYS_MAX entries before anything
     * is claimed. The glass rides above everything. */
    uint16_t pick_cls[CLK_LAYER_KEYS_MAX], pick_slot[CLK_LAYER_KEYS_MAX];
    uint16_t pick_ord[CLK_LAYER_KEYS_MAX];
    int n = 0, hidden = 0;
    for (uint32_t i = 0; i < sec->n_elem && n < CLK_LAYER_KEYS_MAX; i++) {
        orb_bundle_elem_t e;
        orb_bundle_elem_at(sec, i, &e);
        if (e.cls == ORB_ASSET_PLATE && e.slot == 0u) continue;   /* the canvas */
        if (e.cls == ORB_ASSET_SOUND) continue;   /* D39: played by sound_policy, never drawn */
        if (e.flags & CLK_EF_HIDDEN) { hidden++; continue; }
        pick_cls[n] = e.cls; pick_slot[n] = e.slot;
        pick_ord[n] = e.cls == ORB_ASSET_PLATE ? 0xFFFFu
                    : depth_key(e.order, e.cls == ORB_ASSET_AUX && (e.flags & CLK_EF_AUX_TWIN));
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
        pick_ord[b + 1] = ko;
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
        /* The tick state — s_comp_frame, s_row — is indexed by the GEO table's
         * own slot, which a dense bake makes the element slot and a sparse one
         * not. Resolve it once here, against the same projection the update
         * passes walk, and the pose pass never guesses. */
        uint8_t gidx = 0xFFu, role = CLK_ROLE_AUX;
        if (pick_cls[k] == ORB_ASSET_BANK && el.kind == CLK_KIND_WINDOW) {
            role = CLK_ROLE_COMP;
            for (int c = 0; c < n_comps_geo; c++)
                if (g_geo->comps[c].tag == el.slot) { gidx = (uint8_t)c; break; }
        } else if (pick_cls[k] == ORB_ASSET_AUX) {
            for (int c = 0; c < n_rows_geo; c++)
                if (g_geo->rows[c].tag == el.slot) {
                    gidx = (uint8_t)c;
                    role = row_role(&g_geo->rows[c]);
                    break;
                }
        } else if (pick_cls[k] == ORB_ASSET_PLATE) {
            role = CLK_ROLE_GLASS;
        }
        orb_layer_desc_t d;
        memset(&d, 0, sizeof d);
        clk_row_desc(&el, bank_off, role, &d);
        const int h = svc->layer_add(self, &d);
        if (h < 0) { refused++; continue; }   /* full, malformed, or no heap: the
                                               * table counted it; the walk moves on */
        clk_layer_key_t *r = &s_lay_key[s_lay_n];
        r->cls = pick_cls[k]; r->slot = pick_slot[k];
        r->frame0 = el.frame0; r->n_frames = el.n_frames;
        r->x = el.x; r->y = el.y;
        r->kind = (uint8_t)el.kind;
        r->gidx = gidx;
        r->d = d;                               /* the pose the table holds now */
        s_lay_handle[s_lay_n] = h;
        s_lay_n++;
    }

    ESP_LOGI(TAG, "layer table: %d row%s, %d hidden, %d refused (base %lu)",
             s_lay_n, s_lay_n == 1 ? "" : "s", hidden, refused, (unsigned long)bank_off);
    clock_layers_claim(s_lay_n, refused);
}

/* The per-tick pose pass (Migration 2, Commit 4): what the deleted
 * clock_layer_commit's job-list walk did at the flush, done against the wall
 * state at the tick instead — once per row, on the numbers that pass already
 * kept, and nothing reaches the gate twice.
 *
 * Each row's pose is DERIVED, never stored twice: a moving row reads the pose
 * rows_update just staged in s_row (the carriage at the frame tourbillon_advance
 * just composited), a BANK row the frame the complication pass just selected,
 * and the glass is the static it was baked as. A row never posed (ROW_HIDDEN)
 * or a comp frame of -1 wakes hidden and stays that way.
 *
 * Diff first: an hour hand that stands still costs one integer compare. A 0->1
 * transition also TOUCHES: that is the one damage a mark site cannot have
 * computed, because until now the row drew nothing and its box never entered the
 * row bank. Vacated poses need no touch here — every move-mark (the row wedges,
 * the comp boxes, the tourbillon diff) marks old and new together, and the flush
 * weld restores and repaints both. */
static void clock_layers_refresh(void)
{
    if (s_lay_n == 0 || !orb_sched_l4_active())
        return;                       /* base face, classic path: nothing to feed */
    const orb_svc_t *svc = orb_svc_table();
    for (int i = 0; i < s_lay_n; i++) {
        clk_layer_key_t *r = &s_lay_key[i];
        orb_layer_desc_t d = r->d;    /* start from the pose the table holds */
        uint8_t show = 0;

        /* The visibility gates: a row whose widget the dial did not build, or whose
         * pose the update pass hid, stays hidden here for the same reason. */
        switch (r->cls) {
        case ORB_ASSET_PLATE:
            if (s_overlay) show = 1;  /* the glass: built and static */
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
            const row_state_t *st = (r->gidx != 0xFFu && s_row) ? &s_row[r->gidx] : NULL;
            if (st && st->obj && st->ang_10 != ROW_HIDDEN) {
                /* A travelled pose reaches the compositor as the plate at its placed
                 * origin moved by the update pass's own figure — the number the marks
                 * and the widget read — and turned about its moved pivot. */
                if (st->ox || st->oy) {
                    d.x = (int16_t)(r->x + st->ox);
                    d.y = (int16_t)(r->y + st->oy);
                } else {
                    d.x = 0; d.y = 0;   /* the resolver's own placement */
                }
                d.ang_10 = (int16_t)st->ang_10;
                if (r->gidx == clk_theme.car_row && clk_theme.tourbillon_n) {
                    /* The carriage, at the frame the wall clock just picked — the
                     * frame tourbillon_advance marks against. */
                    int f = s_tourb_shown;
                    if (f < 0 || f >= (int)r->n_frames) f = 0;
                    orb_bundle_spr_t sp;
                    orb_bundle_spr_at(s_lay_sec, (uint32_t)(r->frame0 + f), &sp);
                    d.art.off = s_lay_bank_off + sp.off;
                    d.art.bytes = sp.size;
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
    /* Fresh widgets: forget every pose last pushed, so the first tick poses each new
     * widget and marks its box — a rebuilt view must never compare its first pose
     * against a dead theme's last one and skip the setter as "unchanged", and
     * ROW_HIDDEN is the honest "nothing drawn yet". */
    if (!s_row)
        s_row = heap_caps_calloc(CLK_ROW_MAX, sizeof *s_row,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_row)
        for (int i = 0; i < CLK_ROW_MAX; i++) {
            memset(&s_row[i], 0, sizeof s_row[i]);
            s_row[i].ang_10 = ROW_HIDDEN;
        }
    else
        ESP_LOGE(TAG, "row state: PSRAM alloc failed — the dial draws its plate only");

    /* The stack, by one key (D42): every moving row and every complication window
     * in depth order, a twin under the rows at its depth, table order at a tie —
     * depth_key, the sort the layer table runs too, so the classic path's creation
     * order (its only notion of z) and the L4 job list agree. The plate is below it
     * all and the glass above. A row's widget is a turned layer (make_layer: the
     * pivot/centre contract, the blend mapping, the sampling row_aa picks); the
     * carriage's starts on its first frame. */
    complications_reset();
    const orb_theme_geo_t *geo = orb_theme_geo();
    const orb_bundle_sec_t *zsec = NULL;
    uint32_t zoff = 0;
    if (orb_theme_live_sec(ORB_CLOCK3_SLUG, &zsec, &zoff)) {
        uint16_t zkey[CLK_LAYER_KEYS_MAX], zcls[CLK_LAYER_KEYS_MAX], zslot[CLK_LAYER_KEYS_MAX];
        int zn = 0;
        for (uint32_t i = 0; i < zsec->n_elem && zn < CLK_LAYER_KEYS_MAX; i++) {
            orb_bundle_elem_t e;
            orb_bundle_elem_at(zsec, i, &e);
            if (e.cls != ORB_ASSET_AUX && e.cls != ORB_ASSET_BANK) continue;
            const uint16_t k = depth_key(e.order, e.cls == ORB_ASSET_AUX &&
                                                  (e.flags & CLK_EF_AUX_TWIN));
            int at = zn++;
            while (at > 0 && zkey[at - 1] > k) {          /* stable insert */
                zkey[at] = zkey[at - 1]; zcls[at] = zcls[at - 1]; zslot[at] = zslot[at - 1];
                at--;
            }
            zkey[at] = k; zcls[at] = e.cls; zslot[at] = e.slot;
        }
        for (int z = 0; z < zn; z++) {
            if (zcls[z] == ORB_ASSET_BANK) {
                for (uint16_t c = 0; c < geo->n_comps && c < CLK_COMP_MAX; c++)
                    if (geo->comps[c].tag == zslot[z]) { complication_create(geo, c); break; }
                continue;
            }
            for (uint16_t i = 0; s_row && i < geo->n_rows && i < CLK_ROW_MAX; i++) {
                if (geo->rows[i].tag != zslot[z]) continue;
                const clk_row_t *r = &geo->rows[i];
                const bool car = i == clk_theme.car_row && clk_theme.tourbillon_n;
                const clk_sprite_t *sp = car ? &clk_theme.tourbillon[0] : &r->spr;
                if (car ? !sp->show : !r->spr.show) break;
                /* A twin shares its row's sampling, so a filtered hand never stands on
                 * an unfiltered shadow; the carriage's frames never take the filter. */
                const clk_row_t *ar = ((r->flags & CLK_FLAG_AUX_SHADOW) && i > 0)
                                    ? &geo->rows[i - 1] : r;
                s_row[i].obj = make_layer(sp, &s_row[i].dsc,
                                          !car && row_aa(&clk_theme, ar, NULL));
                break;
            }
        }
    }

    /* Glass / hub cap last, on top of everything. */
    s_overlay = make_layer(&clk_theme.overlay, &s_overlay_dsc, false);

    /* The layer table, from the same bundle the widgets above were projected
     * from (Migration 2, Commits 3 and 4). Rows enter hidden and the tick's
     * pose pass wakes them — see clock_layers_refresh. A failure here is a
     * smaller ledger row on the owner's app page and a log line — never a
     * dial that fails to boot: the table only feeds L4, and L4 takes the glass
     * only on a claim with no refusal (orb_sched_l4_active), so a dial whose
     * table fell short still draws through the widgets above. That clause holds
     * because of the gate, not by default: without it a still dial would mark
     * rows nobody welds. */
    s_lay_app = self;
    clock_layers_build(self);

    /* The carriage's frame bank: descriptors for its widget's swap. Reset the frame
     * state so the first tick (re)draws it whole. */
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

    int n_moving = 0, n_still = 0, n_twin = 0;
    for (uint16_t i = 0; i < geo->n_rows && i < CLK_ROW_MAX; i++) {
        const clk_row_t *r = &geo->rows[i];
        if (r->flags & CLK_FLAG_AUX_SHADOW) n_twin++;
        else if (r->src == CLK_SRC_NONE) n_still++;
        else n_moving++;
    }
    ESP_LOGI(TAG, "analog clock: %d moving row%s, %d still, %d shadow%s, sweep %s, bg %s",
             n_moving, n_moving == 1 ? "" : "s", n_still, n_twin, n_twin == 1 ? "" : "s",
             sweepdesc, bgdesc);
    /* The sampling each turned layer was BUILT with, asked of the same function
     * make_layer and the layer table took it from, with the rule that answered:
     * `flag` is the bake's EF_SMOOTH on a painter-owned dial, `classic-rule` the
     * motion words on a dial LVGL draws. Never derived from theme fields alone —
     * that is how the old line reported the shipped rule under an ablation arm. */
    {
        char aab[240];
        int o = 0;
        for (uint16_t i = 0; i < geo->n_rows && i < CLK_ROW_MAX && o < (int)sizeof aab - 40; i++) {
            const clk_row_t *r = &geo->rows[i];
            if ((r->flags & CLK_FLAG_AUX_SHADOW) || r->src == CLK_SRC_NONE) continue;
            const char *why = "";
            const bool sm = row_aa(&clk_theme, r, &why);
            o += snprintf(aab + o, sizeof aab - (size_t)o, "%sptr%u %s=%s", o ? ", " : "",
                          (unsigned)r->tag, sm ? "smooth" : "nearest", why);
        }
        ESP_LOGI(TAG, "row AA: %s", o ? aab : "no turned rows");
    }
    if (clk_theme.tourbillon_n && clk_theme.car_row < geo->n_rows)
        ESP_LOGI(TAG, "carriage: ptr%u, %u frames @ %u fps, orbit about (%d,%d)",
                 (unsigned)geo->rows[clk_theme.car_row].tag,
                 (unsigned)clk_theme.tourbillon_n, (unsigned)clk_theme.tourbillon_fps,
                 (int)geo->rows[clk_theme.car_row].spr.centerx,
                 (int)geo->rows[clk_theme.car_row].spr.centery);
    return ESP_OK;
}

lv_obj_t *clock_view_screen(void)
{
    return s_scr;
}

/* When the dial last came onto the glass: the press that picked it from the
 * carousel also arms whatever lands (input_router's "press once, you are inside"),
 * and that press must not be read as a request for the repeater. */
static int64_t s_shown_us;

/* D39 §5.1's repeater, on demand: a press of the knob while the dial is on the glass
 * sounds the time — the hours on the low stroke, the quarters past as ting-tang
 * pairs, the minutes past the quarter on the high stroke — the way a minute
 * repeater does. The theme must carry all three strokes. It is the owner asking,
 * so quiet hours do not apply; the clock's own sound grant still does. */
void clock_view_repeater(void)
{
    if (!orb_sound_ready() || !snd_granted()) return;
    if (esp_timer_get_time() - s_shown_us < 800000) return;   /* the press that picked the dial */
    uint32_t n = 0;
    uint8_t pat = CLK_PAT_NONE;
    const clk_sound_t *tab = orb_theme_sounds(&n, &pat);
    const clk_sound_t *hr = snd_find(tab, n, CLK_SND_STRIKE_HOUR);
    const clk_sound_t *qt = snd_find(tab, n, CLK_SND_STRIKE_QUARTER);
    const clk_sound_t *mn = snd_find(tab, n, CLK_SND_STRIKE_MINUTE);
    if (!hr || !qt || !mn) {
        ESP_LOGI(TAG, "repeater: the theme lacks a stroke (hour %d, quarter %d, minute %d)",
                 hr != NULL, qt != NULL, mn != NULL);
        return;
    }
    time_t t; double frac; bool synced;
    orb_time_now(&t, &frac, &synced);
    struct tm ti;
    localtime_r(&t, &ti);
    const int h12 = ti.tm_hour % 12 ? ti.tm_hour % 12 : 12;
    const int quarters = ti.tm_min / 15, minutes = ti.tm_min % 15;
    int64_t at = esp_timer_get_time() + 150000;
    for (int i = 0; i < h12; i++, at += 900000)       snd_at_flags(hr, at, ORB_SOUND_ASKED);
    at += 600000;
    for (int i = 0; i < quarters; i++, at += snd_us(qt) + 250000) snd_at_flags(qt, at, ORB_SOUND_ASKED);
    if (quarters) at += 600000;
    for (int i = 0; i < minutes; i++, at += 450000)   snd_at_flags(mn, at, ORB_SOUND_ASKED);
    ESP_LOGI(TAG, "repeater: %d:%02d — %d, %d, %d strokes", h12, ti.tm_min, h12, quarters, minutes);
}

void clock_view_set_bounce(bool on)
{
    if (on) s_shown_us = esp_timer_get_time();
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


