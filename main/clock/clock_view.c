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
 * clock_layer_commit, which must and do agree on the stack.)
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
 *   1 = GIF DELTA BANK: one full keyframe + per-frame changed-region rects
 *       (packed offline against the simulated device canvas, perceptual
 *       threshold). The keyframe is copied into a PSRAM canvas at init; the
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
#include "orb_net.h"   /* orb_net_time_synced(): the dial's second time source */

#include "clock_assets.h"   /* clk_theme, clk_sprite_t, clk_bg_frames, CLK_BLOB_BYTES */
/* Second, deliberately: this header #defines the names clock_assets.h just defined
 * as objects, so including it before would expand a macro inside the generated
 * struct definition. Everything below this line reads the theme through
 * orb_theme_geo() — which is the whole firmware change for a theme that arrives on
 * an SD card instead of in the build. */
#include "orb_theme_geo.h"
#include "art_bank.h"       /* the bank the above offsets index into */

static const char *TAG = "clock";

/* Free-running time base: starts at a fixed epoch, ticks in wall-clock real
 * time until SNTP lands. compose_now() takes time() from that point on, so this
 * is the unsynced source only — see the dual-source note there. */
#define CLOCK_EPOCH 1700000000  /* Nov 2023 baseline */

static lv_obj_t *s_scr;
static lv_obj_t *s_plate;
static lv_obj_t *s_overlay;
static lv_obj_t *s_hand[5];
static lv_obj_t *s_shadow[5];
static lv_timer_t *s_tick_timer;

static lv_image_dsc_t s_plate_dsc;
static lv_image_dsc_t s_overlay_dsc;
static lv_image_dsc_t s_hand_dsc[5];
static lv_image_dsc_t s_shadow_dsc[5];

/* ---- the sub-dial pointers (the theme's clk_aux_t rows) -------------------
 * One widget, descriptor, and last-shown angle per row — the same shape of
 * state s_hand/s_shadow/s_ang_10 are for the five compiled-in slots, and for
 * the same reason: the L4 hook paints from the angles, the classic path from
 * the widgets, and both read ONE truth per tick. A hand and its blur/offset
 * twin are two rows (see the aux block for the pairing rule), which is why the
 * arrays are indexed by row and not by pointer. */
static lv_obj_t *s_aux_obj[CLK_AUX_MAX];
static lv_image_dsc_t s_aux_dsc[CLK_AUX_MAX];
static int32_t   s_aux_ang_10[CLK_AUX_MAX];   /* INT32_MIN = nothing drawn */
static uint32_t  s_aux_logged;                /* bit i: "no input" printed for row i */
static int       s_aux_at = -1;               /* order index the block rides above */

/* bgAnim plate frames (baked in assets/clock_assets.h -> clk_bg_frames). */
#define CLK_BG_MAX 8
static lv_image_dsc_t s_bg_dsc[CLK_BG_MAX];
static uint32_t s_bg_tick      = 0;   /* ticks since bounce-on, for the loop */
static uint32_t s_bg_every     = 1;   /* ticks each bg frame is held for     */
static int      s_bg_shown     = -1;  /* plate frame on the glass now (0=still) */
static uint32_t s_bg_play_start = 0;  /* ms; 0 = no non-loop play running */
static uint32_t s_bg_last_play  = 0;  /* ms; start of the last finished play    */

/* ---- GIF delta bank (bg_mode == 1) --------------------------------------
 * s_canvas mirrors the pixels the panel currently holds under the plate.
 * The plate widget's image data points AT the canvas, so LVGL re-renders the
 * plate from it whenever an area is invalidated — blitting a frame's delta
 * rects into the canvas and invalidating exactly those areas (plus whatever
 * the hand rotations dirty themselves) is all it takes to animate the
 * background with no full-page repaint. */
#define CLK_GIF_MAX 32
#define CLK_GIF_TICK_MS 40            /* 25 fps target; flush self-throttles */

/* Two motions share the one fast tick in GIF mode: the second hand sweeping
 * (which wants every tick to look smooth) and the GIF background advancing
 * (which does not -- a frame held a few ms longer is imperceptible). Flush cost
 * scales with the number of marked ROWS a commit DMA's, and the two motions
 * mark overlapping-but-not-identical row sets: gif_apply_frame marks the
 * changed rects, mark_all_layers marks the hands' swept boxes. Because flush
 * (~51 ms) exceeds the GIF's own ~40 ms delay, the GIF is perpetually behind and
 * advances EVERY tick, so every frame pays GIF-rows UNION hand-rows. Advancing
 * the GIF only every Nth tick lets the intervening ticks DMA just the hands.
 * Whether that actually drops tiles/frame (i.e. whether the GIF marks rows the
 * hands do not already cover) is the question this flag is here to answer --
 * read chunks/s in the fps line: 135 = GIF marks nothing extra, lower = it does.
 * N=1 = no throttle (original behaviour).
 *
 * Measured (tiles/frame = chunks/s / fps): /1 = 9.0 tiles, 15 fps; /2 = 8.0
 * tiles, 18 fps; /3 = 8.3 tiles, 18 fps. So the GIF contributes only ~1 tile/
 * frame; /2 was the perf sweet spot (the same 18 fps as /3 but a smoother
 * background, and fewer tiles/frame). The plateau at 18 fps is NOT the GIF -- it
 * is mark_all_layers() re-marking all three hands' FULL swept boxes (+shadows
 * +overlay) every tick, even the hour/minute that did not move. That ~8-tile
 * hand-box floor is what keeps flush >= ~47 ms; breaking past it needs a
 * stationary hand to mark only the rows it shares with a moving layer, not its
 * whole box -- which must be done without re-opening the freeze bug (see
 * mark_all_layers).
 *
 * SHIPPED DEFAULT IS 1: visual fidelity of the background wins over the +3 fps
 * that /2's half-speed GIF buys. Set to 2 only if throughput is prioritised
 * over background smoothness. */
#define CLK_GIF_THROTTLE 1

/* L3/L4: rather than invalidating the plate through LVGL whenever a GIF delta
 * rect or a hand rotation dirties it — the software image blit measured
 * ~250-300 cycles/px (~85 ms/frame on the orb ticks; experiment A: dropping the
 * plate invalidation took render~ 100->14 ms and fps 8->23) — the tick marks
 * the affected rows and the L4 commit hook (clock_layer_commit/paint_row)
 * rebuilds them straight
 * in the panel mirror: background restored out of the canvas, then the
 * hands/shadows/glass composited over it. 0 = old all-through-LVGL path
 * (A/B fallback). */
#define CLK_BG_DIRECT_PUSH 1

/* Re-mark every layer's full swept box each tick on L4/GIF themes (see
 * mark_all_layers). SHIPPED = 0: a layer marks only the rows it actually
 * dirties (apply_angles wedges, gif_apply_frame pushes, tourbillon diff
 * boxes), and the commit hook repaints every layer across each marked row.
 * The blanket re-mark made a STANDING hand pay its whole swept box every
 * tick -- measured 134.9 ms flush for a static needle at 15 fps, dropping to
 * 54.5 ms with the re-mark off (docs/rows-capacity-2026-10-10.md, batch 6:
 * 10 flashed points, panel byte-identical under the flush gate). The classic
 * freeze bug (mark_all_layers header) cannot return: restore and repaint are
 * per-row x-span scoped, so an unmarked row is never touched. 1 restores the
 * old discipline for A/B. */
#ifndef CLOCK_L4_MARK_STATIC_BOXES
#define CLOCK_L4_MARK_STATIC_BOXES 0
#endif

static uint16_t        *s_canvas;      /* PSRAM, CLK_SCR_W*CLK_SCR_H RGB565 */
static lv_image_dsc_t   s_canvas_dsc;
static int              s_gif_shown = 0;   /* frame the canvas holds now */
static uint32_t         s_gif_due;         /* lv_tick ms the next frame is due */
static uint32_t         s_fps_t0, s_fps_ticks;
/* Deadline ladder for the tick (see tick_timer_cb): lv_timer measures its
 * period from when the LAST RUN STARTED but we re-set the period from the
 * callback's END, so anything shorted from the requested period shows up
 * as schedule, not drift. 0 = not yet anchored (fresh start / unpark). */
static uint32_t         s_tick_due;
/* GIF-mode hand beat state (hand_angles): tick counter, the true seconds it
 * was last re-anchored to, and the wall fraction held between beats. */
static uint32_t         s_bt_ctr = 0;
static int              s_bt_sec = -1;
static double           s_bt_frac = 0.0;
static uint32_t         s_fps_bench_f0;
static uint64_t         s_fps_bench_t0;
static uint64_t         s_fps_pix0;
static uint32_t         s_fps_chunk0;
static uint64_t         s_fps_sub0;
static uint32_t         s_fps_te0;        /* display_te_wait_us() last read */
/* Commit-hook phase breakdown (per-second deltas on the fps line, ms). The
 * flush is one number in display's own bookkeeping; these say which of the
 * hook's three passes eats it, so an optimization is chosen by measurement and
 * not by which loop looks expensive: rest = take the mirror row + wipe the
 * staging row back to plain canvas, paint = every layer's gfx_blend_px traffic,
 * cmp = the flush gate (compare the staged row against the mirror and write
 * back what changed). Per-layer blend counts live next to gfx_paint_row_ctx. */
static uint64_t         s_hk_rest_us0, s_hk_paint_us0, s_hk_cmp_us0;
/* Previous-second baselines of display's commit-phase totals (mirror/hook/te/dma). */
static uint64_t         s_fps_ph0, s_fps_ph1, s_fps_ph2, s_fps_ph3;

/* ---- tourbillon: an animated second hand (frame bank + device rotation) ----
 * The kit's seconds layer is N full-canvas frames that share geometry and
 * differ only in internal motion (an oscillating balance); the ORBIT around
 * the dial centre is the same rotation every hand already gets, driven by the
 * seconds angle (the kit manifest's seconds/60 x 360 formula). Between frame
 * swaps only the pixels inside the baked diff rect vs the PREVIOUS frame can
 * differ, so the per-tick re-mark is that rotated diff box — not the
 * carriage's full swept box, which would re-composite ~40% of the page every
 * tick and re-create the mark_all_layers plateau this dial must not hit. */
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
 * — marks are as wide as the ink and no wider. The commit hook's flush gate
 * (clock_layer_commit) then skips DMA for rows whose rebuild came out
 * byte-identical anyway. If the chord tables cannot be built the caller
 * falls back to the swept-box union. */
static uint16_t *s_tourb_cx0[CLK_TOURB_MAX];
static uint16_t *s_tourb_cx1[CLK_TOURB_MAX];

/* One stamp pass's device-row span accumulation (x1 < x0 = untouched row). */
static int16_t  s_wp_x0[CLK_SCR_H], s_wp_x1[CLK_SCR_H];
static int32_t  s_wp_lo, s_wp_hi;



static bool gif_mode(void)
{
    return clk_theme.bg_mode == 1 && clk_theme.gif_frames >= 2
           && clk_theme.gif_frames <= CLK_GIF_MAX && s_canvas != NULL;
}

/* GIF-mode beat: the tick rides the GIF's OWN baked delay, so one tick is one
 * GIF frame and every frame is up for exactly the same number of ticks. A tick
 * faster than the GIF's delay does not yield that delay — the frame still
 * only flips on a tick, so a 67 ms GIF on a 40 ms tick lands on 2-tick holds
 * (~9 fps, and uneven when flush jitter crosses the due boundary). Slower is
 * impossible; equal is the only clean answer. The delay table is baked
 * uniform per theme (--bg-fps), so frame 0's delay is the tempo. The
 * sweeping hand composes on every tick but reads the wall clock, not the
 * tick count, so it stays time-accurate at any sampling rate — and with a
 * tickRate declared it quantises to 1/tickRate steps, so extra ticks between
 * beats cost no hand redraw (see hand_angles()). */
static uint32_t gif_tick_ms(void)
{
    const uint32_t d = clk_gif_delays[0];
    if (d < 20) return 20;              /* sanity floor: never spin past 50 fps */
    return d;
}

static bool bg_live(void)
{
    if (clk_theme.bg_mode == 1)
        return gif_mode();
    return clk_theme.bg_frames > 0 && clk_theme.bg_frames <= CLK_BG_MAX;
}

static bool     s_active = false;     /* clock is the on-screen view */
static int64_t  s_t0_us  = 0;         /* epoch anchor for the wall clock */
static bool     s_warned_screen = false;

/* Wall-clock base compose_now() ticks from, and the esp_timer instant it was
 * sampled at. ONE pair, not two sources: tm_sec and the sub-second fraction
 * must come from the same clock, or the second hand steps backward once per
 * second by the phase difference between time()'s seconds and the esp_timer
 * sawtooth. Starts on the free-running baseline and is overwritten the first
 * time the system clock looks credible (see compose_now). */
static time_t s_epoch_base   = CLOCK_EPOCH;
static bool   s_anchor_is_real = false;   /* has s_epoch_base ever come from time()? */
#define CLOCK_RESYNC_SLACK 2.0            /* s of drift that re-triggers a re-anchor */

/* Mapped base of the art bank, filled in by clock_view_init() (NULL until then).
 * Kept as a file static rather than calling art_bank_ptr() on every sprite read:
 * blob() is on the per-frame paint path and this keeps it a single load. */
static const uint8_t *s_art;

/* Every pixel of the dial comes from here: the bank now lives in the `assets`
 * flash partition and is mapped once at boot by art_bank_init() below, so this
 * stays a bare pointer return and sprite reads cost exactly what they did when
 * the bank was linked into .rodata. See main/art/art_bank.h. */
static const uint8_t *blob(void) { return s_art; }


static void fill_plate_dsc(lv_image_dsc_t *d)
{
    memset(d, 0, sizeof(*d));
    d->header.magic      = LV_IMAGE_HEADER_MAGIC;
    d->header.cf         = LV_COLOR_FORMAT_RGB565;
    d->header.flags      = 0;
    d->header.w          = CLK_SCR_W;
    d->header.h          = CLK_SCR_H;
    d->header.stride     = (uint32_t)CLK_SCR_W * 2u;
    d->data_size         = (uint32_t)CLK_SCR_W * (uint32_t)CLK_SCR_H * 2u;
    d->data              = blob() + clk_theme.plate_off;
}

/* The sprite the given layer index draws RIGHT NOW: a tourbillon frame when
 * the theme has a frame bank on the second hand, the static hand otherwise.
 * Geometry (bbox, pivot, centre) is identical across the bank — the frame
 * swap changes pixels, never the rotation maths. */
static const clk_sprite_t *layer_spr(const int k)
{
    if (k == 2 && clk_theme.tourbillon_n
        && s_tourb_shown >= 0 && s_tourb_shown < (int32_t)clk_theme.tourbillon_n)
        return &clk_theme.tourbillon[s_tourb_shown];
    return &clk_theme.hand[k];
}

/* ---- bgAnim: port of orb-os bg_anim_frame() / bg_rate() ------------------

 * A looping theme cycles the still plate plus its baked extra frames forever,
 * advancing off the tick count so every frame is up for exactly the same
 * number of ticks; a non-looping theme holds the still plate until bg_every
 * seconds have passed, plays the extras through once at bg_fps, and holds
 * again. The tick period is chosen to divide the animation's frame period
 * (bg_rate) so the hand and the background share one beat instead of beating
 * against each other. */
static uint32_t tick_base_ms(void);
static uint32_t bg_rate(uint32_t base, uint32_t *every);

static void build_bg_dscs(void)
{
    for (int i = 0; i < (int)clk_theme.bg_frames && i < CLK_BG_MAX; i++) {
        s_bg_dsc[i]     = s_plate_dsc;            /* same w/h/stride/cf */
        s_bg_dsc[i].data = blob() + clk_bg_frames[i];
    }
}

static int bg_frame_now(void)
{
    if (!bg_live()) return 0;

    if (clk_theme.bg_loop)
        return (int)((s_bg_tick / (s_bg_every ? s_bg_every : 1))
                     % (uint32_t)(clk_theme.bg_frames + 1));

    const uint32_t now  = lv_tick_get();
    const uint32_t step = 1000u / (uint32_t)(clk_theme.bg_fps < 1 ? 1 : clk_theme.bg_fps);
    if (s_bg_play_start == 0) {
        if (s_bg_last_play == 0) { s_bg_last_play = now; return 0; }
        if (now - s_bg_last_play < (uint32_t)clk_theme.bg_every * 1000u) return 0;
        s_bg_play_start = now ? now : 1;
        ESP_LOGI(TAG, "bg play starts: %u frames at %u fps",
                 (unsigned)clk_theme.bg_frames, (unsigned)clk_theme.bg_fps);
    }
    const uint32_t into = now - s_bg_play_start;
    const int idx = (int)(into / (step ? step : 1));
    if (idx > (int)clk_theme.bg_frames) {       /* played through: hold the still */
        ESP_LOGI(TAG, "bg play ends after %lu ms, holding %u s",
                 (unsigned long)into, (unsigned)clk_theme.bg_every);
        s_bg_play_start = 0;
        s_bg_last_play  = now;
        return 0;
    }
    return idx;
}

static void show_plate_frame(int idx)
{
    if (!bg_live() || idx == s_bg_shown) return;
    s_bg_shown = idx;
    lv_image_set_src(s_plate, idx == 0 ? &s_plate_dsc : &s_bg_dsc[idx - 1]);
    lv_obj_invalidate(s_plate);
}


#if CLK_BG_DIRECT_PUSH
static bool l4_active(void);
static void request_commit(void);
#endif

/* Apply frame `tgt`'s delta rects into the PSRAM canvas and get them onto the
 * glass. The canvas already holds frame tgt-1 (baked that way), so copying
 * the changed rects makes it hold tgt exactly where tgt differs perceptibly.
 *
 * L3 (CLK_BG_DIRECT_PUSH): the glass update is a straight push into the panel
 * mirror — LVGL's plate re-blit cost those pixels ~250-300 cycles each. The
 * layers that PAINT OVER the background (hand/shadow/glass) are restored by
 * the L4 commit hook (see paint_row): the pushed rows are marked, and just
 * before the bands DMA the mirror out, the affected layers are composited
 * directly into it with LVGL's own nearest-neighbour rotation maths.
 *
 * If the mirror is unavailable (no PSRAM) or a push is rejected, that rect
 * falls back to the classic path: invalidate the plate area and let LVGL
 * re-render plate and layers over it. */
static void gif_apply_frame(int tgt)
{
    const uint16_t lo = clk_bg_rect_first[tgt];
    const uint16_t hi = clk_bg_rect_first[tgt + 1];
    const uint8_t *base = blob();

    /* Bounding box of everything pushed this tick (screen coords). */
    bool any_pushed = false;

    for (uint16_t r = lo; r < hi; r++) {
        const clk_bg_rect_t *rc = &clk_bg_rects[r];
        if (rc->w == 0 || rc->h == 0) continue;
        const uint16_t *src = (const uint16_t *)(base + rc->off);
        for (int y = 0; y < rc->h; y++) {
            uint16_t *dst = &s_canvas[(size_t)(rc->y + y) * CLK_SCR_W + rc->x];
            memcpy(dst, src + (size_t)y * rc->w, (size_t)rc->w * sizeof(uint16_t));
        }
        bool pushed = false;
#if CLK_BG_DIRECT_PUSH
        /* L4: do NOT write the mirror here. The canvas is now up to date and the
         * commit hook restores exactly these rows out of it at the one safe
         * moment (every chunk mirrored, before the bands DMA). Pushing straight
         * into the mirror here was the bug: a tick can run while an earlier
         * frame's flush is still in flight, so the push overwrote rows whose
         * hand pixels a previous commit had already composited, and the marks
         * for them had been consumed -- hands vanished over precisely the
         * pushed rects. One writer per row, and it is the hook. */
        if (l4_active()) {
            gfx_mark_rows(rc->y, rc->y + rc->h - 1, rc->x, rc->x + rc->w - 1, true);
            pushed = true;
        }
#endif
        if (!pushed) {
            lv_area_t a = { rc->x, rc->y,
                            (lv_coord_t)(rc->x + rc->w - 1),
                            (lv_coord_t)(rc->y + rc->h - 1) };
            lv_obj_invalidate_area(s_plate, &a);
        }
    }
#if CLK_BG_DIRECT_PUSH
    (void)any_pushed;
    /* Rows were painted outside LVGL: give LVGL a 1 px job so a commit (and
     * with it our hook, and the band DMA that ships the rows) actually runs. */
    request_commit();
#endif
    s_gif_shown = tgt;
}

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
    const uint32_t t = gif_tick_ms();
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
    if (sweep && clk_theme.tick_rate > 0 && gif_mode()) {
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

    float secF = (float)sec + (sweep ? (float)wall_frac : 0.0f);
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

#define HAND_REDRAW_EPS_10 2
static void apply_angles(const float ang[5])
{
    lv_display_t * disp = lv_obj_get_display(s_scr);

    for (int i = 0; i < 5; i++) {
        const int32_t a = (int32_t)lroundf(ang[i] * 10.0f);
        if (a == s_ang_10[i]) continue;
        if (s_ang_10[i] != INT32_MIN &&
            abs((int)(a - s_ang_10[i])) < HAND_REDRAW_EPS_10) continue;
        if (!s_hand[i] && !s_shadow[i]) { s_ang_10[i] = a; continue; }

        const int32_t prev = s_ang_10[i];
        lv_area_t u;
        bool have = false;

        /* Sweep tourbillon: the orbit rotates the whole carriage, so the
         * box union below is most of the page on EVERY tick and the commit
         * hook re-composites the full dial+glass across it (measured: sweep
         * ran at half tick's frame rate, the whole gap in those marks).
         * Mark the two poses' rotated ink instead — each source row's ink
         * extent rotated as a segment, accumulated per device row — so the
         * rebuild is confined to what the carriage actually covered or
         * vacated. The frame swap's changed pixels are added by
         * tourbillon_advance's diff-box marks; the chord model tracks only
         * the hand's own art, so a shadowed second hand keeps the box. */
        const bool inked = (i == 2 && prev != INT32_MIN) &&
                           tourbillon_ink_mark((int)s_tourb_shown, prev, a);

        if (!inked && prev != INT32_MIN) {
            /* Union of the old and new wedge of every layer sharing this angle,
             * in both their un-shifted (hand) and shifted (shadow) positions. */
            if (s_hand[i]) {
                gfx_swept_box(&clk_theme.hand[i],   prev, &u);
                lv_area_t n; gfx_swept_box(&clk_theme.hand[i], a, &n);
                gfx_area_union(&u, &n);
                have = true;
            }
            if (s_shadow[i]) {
                lv_area_t su; gfx_swept_box(&clk_theme.shadow[i], prev, &su);
                lv_area_t sn; gfx_swept_box(&clk_theme.shadow[i], a, &sn);
                gfx_area_union(&su, &sn);
                if (have) gfx_area_union(&u, &su);
                else { u = su; have = true; }
            }
        }
        else if (!inked && s_hand[i]) {
            gfx_swept_box(&clk_theme.hand[i], a, &u);
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
            if (l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
            else
#endif
                lv_obj_invalidate_area(s_scr, &u);
        }

        if (prev == INT32_MIN) {
            /* First pose ever: nothing painted yet, the setter's own
             * (full-box) invalidation is the one that paints it. */
            if (s_hand[i])   lv_image_set_rotation(s_hand[i],   a);
            if (s_shadow[i]) lv_image_set_rotation(s_shadow[i], a);
        }
        else {
            lv_display_enable_invalidation(disp, false);
            if (s_hand[i])   lv_image_set_rotation(s_hand[i],   a);
            if (s_shadow[i]) lv_image_set_rotation(s_shadow[i], a);
            lv_display_enable_invalidation(disp, true);
            /* The setter early-returns without applying its angle if it
             * already equals the widget's — impossible here since s_ang_10 is
             * only updated together with the setters, but re-mark the swept
             * box once more so a lost invalidation can never stick. */
            if (have) {
#if CLK_BG_DIRECT_PUSH
                if (l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
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
    if (!l4_active()) return false;
#endif
    if (clk_theme.tourbillon_n <= 0 || s_shadow[2]) return false;
    if (f < 0 || f >= (int)clk_theme.tourbillon_n || f >= CLK_TOURB_MAX)
        return false;                          /* no composite on the glass yet */
    if (!tourb_chords(f)) return false;        /* fall back to the box union */

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
    if (n <= 0 || !s_hand[2]) return;

    const uint32_t fps = clk_theme.tourbillon_fps ? clk_theme.tourbillon_fps : 15;
    const int f = (int)(((uint64_t)(lv_tick_get() - s_tourb_t0) * fps)
                        / 1000u % (uint32_t)n);
    if (f == s_tourb_shown) return;

    const int32_t a = s_ang_10[2];
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
    lv_image_set_src(s_hand[2], &s_tourb_dsc[f]);
    lv_display_enable_invalidation(disp, true);
#if CLK_BG_DIRECT_PUSH
    if (l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
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


/* L4 only replaces LVGL when there is a canvas to restore background from;
 * without it (non-GIF themes, or the PSRAM canvas alloc failed) the classic
 * all-through-LVGL path stays in charge. */
static bool l4_active(void)
{
    return CLK_BG_DIRECT_PUSH && s_canvas != NULL && gif_mode();
}

/* LVGL starts a commit only when it has something of its own to render, so a
 * tick that pushed background and moved hands purely through the painter has
 * to give it a reason: a 1 px plate nudge whose chunk mirrors a correct pixel
 * and whose is_last drives the commit our hook hangs on. */
static void request_commit(void)
{
    if (!gfx_commit_needed() || !l4_active()) return;
    gfx_commit_consumed();
    static const lv_area_t nudge = { 0, 0, 0, 0 };
    lv_obj_invalidate_area(s_plate, &nudge);
}

/* Every layer the painter owns, at the angle it is currently held at.
 *
 * Called once per tick, and it is what makes the hook the complete owner of the
 * rows it rebuilds. Marking only the layers whose angle CHANGED was the bug: a
 * hand that stands still (the hour hand for an hour at a time, the minute hand
 * under the 0.2-degree re-pose gate) never marked its own box, so the rows it
 * lives on carried nothing but the GIF rect's narrow x-extent. The hook's
 * restore then wiped that row back to plain background and paint_row's per-row
 * x-clip
 * refused to draw the hand outside it -- the hour hand lost its tip and the
 * minute hand was painted once and froze outside the animated area. */
static void mark_all_layers(void)
{
    /* Rows-capacity bench experiment (docs/rows-capacity-2026-10-10.md): the
     * unconditional full-box re-mark makes a STANDING hand pay its box's
     * restore+re-blend every tick (measured: a plain sweeping needle costs
     * ~85 ms/frame on top of the GIF, and ticking it at 1 Hz does NOT help).
     * Under the L4 hook every marked row is re-composited from the canvas
     * across ALL layers anyway, so layers that did not move need no mark:
     * apply_angles marks moving hands, gif_apply_frame marks pushed rows.
     * Default 1 = the shipped discipline, byte-for-byte. */
#if !CLOCK_L4_MARK_STATIC_BOXES
    return;
#endif
    /* Full-coverage art (tourbillon kits — dial/glass reach every row) cannot
     * afford this discipline at all: re-marking four static boxes unions
     * ~430 rows of full-page restore+repaint per tick and caps the dial at
     * 5 fps (measured). It is also provably redundant under the current hook:
     * the hook's restore wipes each marked row to plain canvas and the layer
     * loop below repaints EVERY layer across the marked span, so a static
     * layer's pixels are rebuilt by whoever actually dirtied its rows — and
     * when nothing moved at all there is nothing to flush. Layers mark
     * themselves on the ticks they change: apply_angles (rotation, incl. the
     * carriage orbit step) and tourbillon_advance (frame swap) both do.
     * Classic themes keep the shipped discipline byte-for-byte — the
     * "hands missing / trailing" freeze bug predates the per-row x-span
     * restore and is not re-litigated for art that runs fine today. */
    if (clk_theme.tourbillon_n > 0) return;

    for (int oi = 0; oi < clk_theme.order_n; ++oi) {

        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (s_ang_10[k] == INT32_MIN) continue;
        lv_area_t b;
        if (s_hand[k])   {
            /* A live tourbillon re-marks ONLY its current frame's changed rect:
             * every other pixel of the shown frame is, row for row, exactly
             * what the last composite produced (nothing else writes them), and
             * the frame swap / orbit step mark their own boxes themselves
             * (tourbillon_advance, apply_angles). The full box would be ~40%
             * of the page re-marked EVERY tick. */
            if (k == 2 && clk_theme.tourbillon_n && s_tourb_shown >= 0)
                tourb_mark(&clk_theme.tourbillon[s_tourb_shown], s_ang_10[k], &b);
            else
                gfx_swept_box(&clk_theme.hand[k], s_ang_10[k], &b);
            gfx_mark_rows(b.y1, b.y2, b.x1, b.x2, false);
        }
        if (s_shadow[k]) {
            /* Its OWN box, never the hand's fallthrough: the shadow record is
             * the hand's shifted by dx,dy, so the old reuse under-marked the
             * vacated shadow edge on themes with shadows, and re-marked a
             * tourbillon diff box twice at hand coords on themes without. */
            lv_area_t sb;
            gfx_swept_box(&clk_theme.shadow[k], s_ang_10[k], &sb);
            gfx_mark_rows(sb.y1, sb.y2, sb.x1, sb.x2, false);
        }
    }

    /* The overlay deliberately does NOT mark its own box any more. Its pixels
     * never move, so the only rows needing a re-composite under/over it are
     * the ones some OTHER layer already flagged (gif pushes, hand wedges, the
     * tourbillon's diff box) — the commit hook restores those rows from the
     * canvas and repaints every layer, overlay included, over them. Marking
     * the overlay's own box was harmless while it was a hub cap, but a kit
     * "glass" overlay covers the whole page, and self-marking it made every
     * tick restore and re-blend the ENTIRE dial (measured 2.4 pages/tick,
     * 3-4 fps). Same rule as a still hand's box: never re-dirty what did not
     * change. */
}






#define CLK_L4_MAX_JOBS 12           /* order[5] x (shadow+hand) + glass */
#define CLK_COMP_JOBS_MAX 32         /* blits for the baked complications: 16
                                      * windows, or ~3 strips' glyphs + window
                                      * pieces. Overflow drops the rest with a
                                      * one-line log (see comp_add_jobs). */
#define CLK_AUX_JOBS_MAX CLK_AUX_MAX /* one blit per aux row (a row is a hand OR
                                      * its twin, never both); overflow is the
                                      * same one-line log as the comp list */
#define CLK_JOB_MAX (CLK_L4_MAX_JOBS + CLK_COMP_JOBS_MAX + CLK_AUX_JOBS_MAX)

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
static uint64_t  s_bench_px, s_bench_px0;             /* ink px, + last report */

static void bench_paint_band(int32_t y0, int nr);   /* gfx band hook, below */

static void bench_init(void)
{
    if (DISPLAY_BENCH_PX == 0 || s_bench_solid) return;
    /* The commit loop moved to gfx; the synthetic ink still has to ride inside
     * its timed paint window, so it is registered as the band hook rather than
     * called from the hook that no longer exists here. */
    gfx_set_band_hook(bench_paint_band);
    const size_t n = (size_t)BENCH_BLK * BENCH_BLK * sizeof(uint16_t);
    s_bench_solid = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bench_solid) return;
    uint16_t *p = s_bench_solid;
    for (size_t i = 0; i < n / sizeof(uint16_t); i++) p[i] = 0xF81F;   /* mag */
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
            const uint16_t *src = &s_bench_solid[(size_t)(y - by0) * BENCH_BLK];
            uint16_t *row = gfx_band(r);
            for (int32_t x = xa; x <= xb; x++)
                gfx_blend_px(&row[x], src[x - bx0], DISPLAY_BENCH_ALPHA, 0);
            s_bench_px += (uint32_t)(xb - xa + 1);
        }
    }
}
#endif /* DISPLAY_BENCH */

/* The app's half of the composite: decide WHICH layers go down, in which order,
 * and hand the list to gfx_commit() (main/gfx/orb_gfx.c), which owns the restore,
 * the band walk, the flush gate and the mark clearing. The row-major layout and
 * the measurement behind it are documented there. */
static void clock_layer_commit(void);
static void comp_add_jobs(const orb_theme_geo_t *g, gfx_paint_ctx_t *jc, int *nj,
                          int bucket);
static void aux_add_jobs(const orb_theme_geo_t *g, gfx_paint_ctx_t *jc, int *nj);

static void clock_layer_commit(void)
{
    if (!l4_active()) return;
    uint16_t *fb = display_fb_mirror();
    if (!fb) return;

    /* Resolve the live layer list once, in the same z-stack as the LVGL
     * sibling order built in clock_view_init(): a shadow goes down directly
     * under its OWN hand, not under every hand. The two paths must agree
     * exactly — a row LVGL paints itself has to come out identical to one this
     * hook paints — so the interleave lives here too. All the row-independent
     * maths (including gfx_swept_box's double-precision libm) is hoisted into the
     * ctx: one build per layer per commit, not per layer per row. */
    /* One ctx per live layer, built once for this commit. Static rather than
     * stack: the hook has exactly one caller (the LVGL task's flush) and the
     * LVGL task stack is not ours to spend 900 B of. */
    static gfx_paint_ctx_t jc[CLK_JOB_MAX];
    int nj = 0;
    /* Bucket 0 first: the tiles that belong below EVERY live layer, which is
     * every window the bake has ever placed except a hosted one. A tile with a
     * bucket (a WINDOW's `first`: the KINDS slot it must stay under) joins the
     * list at that slot's own turn, so the plate's frame composites over its
     * edge. The moon under the c4 plate only reaches the glass from UNDER the
     * plate, and the plate only covers it if the tile went down first. */
    comp_add_jobs(orb_theme_geo(), jc, &nj, 0);
    int buckets_done = 1;                                 /* bit b = bucket drained */
    bool aux_done = false;
    for (int oi = 0; oi < clk_theme.order_n && nj < CLK_JOB_MAX - 1; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (clk_theme.matte_mask & (1u << k)) {           /* baked into the bg */
            if (!aux_done && oi == s_aux_at) {
                aux_add_jobs(orb_theme_geo(), jc, &nj); aux_done = true;
            }
            continue;
        }
        /* Tiles that must stay under THIS live layer, and only statics ever
         * host one: buckets 1 and 2 would name a hand slot, which no bake
         * emits, and bucket 0 is already down below the whole list — calling
         * it again with k == 0 would blend every dial-hosted window a SECOND
         * time, right under the hour hand. */
        if (k >= 3) { comp_add_jobs(orb_theme_geo(), jc, &nj, k); buckets_done |= 1 << k; }
        if (s_shadow[k] && s_ang_10[k] != INT32_MIN)
            if (gfx_paint_ctx_init(&jc[nj], &clk_theme.shadow[k], blob(), s_ang_10[k], (uint8_t)k)) nj++;
        if (s_hand[k] && s_ang_10[k] != INT32_MIN)
            if (gfx_paint_ctx_init(&jc[nj], layer_spr(k), blob(), s_ang_10[k], (uint8_t)k)) nj++;  /* tourbillon frame if live */
        /* The sub-dial pointers join above the last live static, and below
         * whatever the theme ordered after it (see aux_insert_at). */
        if (!aux_done && oi == s_aux_at) {
            aux_add_jobs(orb_theme_geo(), jc, &nj); aux_done = true;
        }
    }
    if (!aux_done) aux_add_jobs(orb_theme_geo(), jc, &nj);  /* no live static at all */
    /* A bucket whose layer never joined the stack — hidden, or folded into the
     * bank by a --matte force — still has to draw its window: it rides above
     * what does exist. The kit's plate is in the bank either way, so this is the
     * same reading the classic path reaches for, and the bake printed the forced
     * fold's pixel cost when it took it. */
    for (int b = 1; b < 5; b++)
        if (!(buckets_done & (1 << b))) comp_add_jobs(orb_theme_geo(), jc, &nj, b);
    if (s_overlay && !(clk_theme.matte_mask & 0x20u) && nj < CLK_JOB_MAX)
        if (gfx_paint_ctx_init(&jc[nj], &clk_theme.overlay, blob(), 0, CLK_ROLE_GLASS)) nj++;  /* the glass */

    /* The layers this dial asked for, in the order it asked: gfx_commit()
     * owns the restore, the band walk, the flush gate and the clearing.
     * Building the list above is the whole of the app's side. */
    gfx_commit(nj, jc, s_canvas);

}
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
 * drawn. Sole truth for both paths: the L4 commit hook builds its jobs from
 * these, and the classic path reads the LVGL widgets comp_slot_show keeps in
 * step. Any drift between the two is the one-tick ghost the hands paid for
 * before apply_angles learned to hold its angles. */
static int32_t         s_comp_frame[CLK_COMP_MAX][CLK_COMP_SLOTS];
static uint32_t        s_comp_logged;        /* bit i: hide reason for comp i printed */
static bool            s_comp_jobs_warned;   /* job-list overflow is a one-liner */

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
    if (l4_active()) { gfx_mark_rows(y0, y1, x0, x1, false); return; }
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

#if CLK_BG_DIRECT_PUSH
static void comp_jobs_overflow(void)
{
    if (s_comp_jobs_warned) return;
    s_comp_jobs_warned = true;
    ESP_LOGW(TAG, "complication/aux blits exceed the %d-slot job list — later rows not drawn",
             CLK_JOB_MAX);
}

/* One paint ctx per currently-drawn slot of ONE z bucket, in record order.
 * Every comp blit is static (ang == 0) — the hook's cheapest path — and the
 * ctxs rebuild each commit from s_comp_frame, exactly how the hands stay honest
 * between the two paths. A bank row without pixels (w or h 0) is dropped inside
 * gfx_paint_ctx_init's drawable test, matching the reader's rule.
 *
 * A WINDOW's `first` IS the bucket: the KINDS slot whose live layer this tile
 * belongs under (0 = below every live layer, the case for every theme baked
 * before a sub-dial ever hosted a window, and the reason 0 needs no plumbing).
 * A STRIP's `first` is its bank index, so a strip cannot be bucketed and rides
 * bucket 0 — the only place any strip has ever been baked to sit. */
static void comp_add_jobs(const orb_theme_geo_t *g, gfx_paint_ctx_t *jc, int *nj,
                          int bucket)
{
    for (uint16_t i = 0; i < g->n_comps && i < CLK_COMP_MAX; i++) {
        const clk_comp_t *c = &g->comps[i];
        if (!c->show || (c->flags & CLK_FLAG_DISABLED)) continue;

        if (c->kind == CLK_KIND_WINDOW) {
            if ((int)c->first != bucket) continue;
            const int32_t f = s_comp_frame[i][0];
            if (f < 0) continue;
            if (*nj >= CLK_JOB_MAX) { comp_jobs_overflow(); return; }
            const clk_sprite_t spr = {
                .w = c->w, .h = c->h,
                .pivotx = 0, .pivoty = 0,
                .centerx = c->x, .centery = c->y,
                .blend = c->blend, .show = 1,
                .color_off = c->off + (uint32_t)f * c->frame_bytes,
            };
            if (gfx_paint_ctx_init(&jc[*nj], &spr, blob(), 0, CLK_ROLE_COMP)) (*nj)++;
            continue;
        }
        if (c->kind != CLK_KIND_STRIP || bucket != 0) continue;

        const int npos = comp_strip_positions(c);
        for (int p = 0; p < npos + 2 && p < CLK_COMP_SLOTS; p++) {
            const int32_t f = s_comp_frame[i][p];
            if (f < 0) continue;
            uint16_t bidx;
            int32_t x, y = c->y;
            if      (p <  npos) { bidx = (uint16_t)(c->first + (uint16_t)f);
                                  x = c->x + c->glyph_lead + (int32_t)p * c->glyph_step; }
            else if (p == npos) { bidx = c->edge_left;  x = c->x; }
            else                { bidx = c->edge_right; x = 0;    }
            if (bidx >= g->n_bank) continue;
            const clk_sprite_t *b = &g->bank[bidx];
            if (p > npos) x = c->x + (int32_t)c->w - (int32_t)b->w;
            if (*nj >= CLK_JOB_MAX) { comp_jobs_overflow(); return; }
            const clk_sprite_t spr = {
                .w = b->w, .h = b->h,
                .pivotx = 0, .pivoty = 0,
                .centerx = (int16_t)x, .centery = (int16_t)y,
                .blend = c->blend, .show = 1,
                .color_off = b->color_off,
            };
            if (gfx_paint_ctx_init(&jc[*nj], &spr, blob(), 0, CLK_ROLE_COMP)) (*nj)++;
        }
    }
}

/* One ctx per pointer currently showing an angle, appended where the commit
 * loop put the block: rotated, so the hook's transform path — the same one the
 * hands pay for, and why an unchanged pointer is not in the list at all. */
static void aux_add_jobs(const orb_theme_geo_t *g, gfx_paint_ctx_t *jc, int *nj)
{
    for (uint16_t i = 0; i < g->n_aux && i < CLK_AUX_MAX; i++) {
        if (s_aux_ang_10[i] == INT32_MIN) continue;       /* nothing drawn */
        if (*nj >= CLK_JOB_MAX) { comp_jobs_overflow(); return; }
        if (gfx_paint_ctx_init(&jc[*nj], &g->aux[i].spr, blob(), s_aux_ang_10[i], CLK_ROLE_AUX))
            (*nj)++;
    }
}
#endif /* CLK_BG_DIRECT_PUSH */

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
 * and tools/theme_schema.py packs that pair). The twin owns pixels and a centre
 * (the hand's, moved by the light vector) and no input of its own — the tick
 * gives it the row before it's angle — so it can never come loose from the
 * pixels it exists to darken. The bake packed it with the SAME pivot as its
 * hand, which is what keeps the light direction fixed while the pointer sweeps.
 *
 * z-order: the block rides the stack above the last live static (a pointer
 * belongs on top of the plate it points at) and below the dial's own hands,
 * which is the kit's own reading — the date pointer is z=1, the hour z=2.
 * s_aux_at is that order index, computed once per view in clock_view_init and
 * honoured by BOTH paint paths, because a row LVGL renders itself has to come
 * out identical to a row the L4 hook paints. */

/* Where the block joins clk_theme.order: the index of the last live static, so
 * the pointers paint after it. -1 when the theme has no live static at all (both
 * folded into the bank, or a plain classic dial), which puts the block at the
 * front of the list — still above the bank, and below every hand. */
static int aux_insert_at(void)
{
    int at = -1;
    for (int oi = 0; oi < (int)clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 3 || k > 4) continue;
        if (clk_theme.matte_mask & (1u << k)) continue;   /* folded into the bg */
        at = oi;
    }
    return at;
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

/* ang = zero + value * span / value_max, in tenths of a degree — the formula
 * tools/theme_schema.py states on the span field and the bake laid the ring out
 * against. Half-up, not truncated: the bake placed day 5 at 58.1 degrees and a
 * truncated read parks the pointer a tenth off the mark the art was drawn to. */
static int32_t aux_angle_10(const clk_aux_t *a, int64_t v1000)
{
    const int64_t den = (int64_t)a->value_max * 1000;
    const int64_t num = (int64_t)a->zero * den + v1000 * (int64_t)a->span;
    int64_t ang = (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
    ang %= 3600;
    if (ang < 0) ang += 3600;
    return (int32_t)ang;
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
            a_new = (i > 0) ? s_aux_ang_10[i - 1] : INT32_MIN;   /* follow the hand */
        } else if (!a->spr.show) {
            a_new = INT32_MIN;
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
                a_new = aux_angle_10(a, v1000);
            }
        }
        if (a_new == s_aux_ang_10[i]) continue;               /* nothing moved */

        /* The wedge to rebuild: where the pointer was and where it is going,
         * about the point the device pins its pivot. Same discipline as
         * apply_angles() — under L4 the commit hook owns those rows. */
        const int32_t prev = s_aux_ang_10[i];
        if (s_aux_obj[i]) {
            lv_area_t u;
            bool have = false;
            if (prev != INT32_MIN) {
                gfx_swept_box(&a->spr, prev, &u); have = true;
                if (a_new != INT32_MIN) {
                    lv_area_t nn; gfx_swept_box(&a->spr, a_new, &nn);
                    gfx_area_union(&u, &nn);
                }
            } else if (a_new != INT32_MIN) {
                gfx_swept_box(&a->spr, a_new, &u); have = true;
            }
            if (have) {
                if (u.x1 < 0) u.x1 = 0;
                if (u.y1 < 0) u.y1 = 0;
                if (u.x2 > CLK_SCR_W - 1) u.x2 = CLK_SCR_W - 1;
                if (u.y2 > CLK_SCR_H - 1) u.y2 = CLK_SCR_H - 1;
#if CLK_BG_DIRECT_PUSH
                if (l4_active()) gfx_mark_rows(u.y1, u.y2, u.x1, u.x2, false);
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
            if (a_new == INT32_MIN) {
                lv_obj_set_hidden(s_aux_obj[i], true);
            } else if (prev == INT32_MIN) {
                lv_obj_set_hidden(s_aux_obj[i], false);
                lv_image_set_rotation(s_aux_obj[i], (uint16_t)a_new);
            } else {
                lv_display_enable_invalidation(disp, false);
                lv_image_set_rotation(s_aux_obj[i], (uint16_t)a_new);
                lv_display_enable_invalidation(disp, true);
            }
        }
        s_aux_ang_10[i] = a_new;
    }
}

static void compose_now(void)
{
    /* Two time sources, one anchor pair (see s_epoch_base). Sampled HERE, on the
     * LVGL task, so time() and esp_timer_get_time() are taken back-to-back and
     * the pair is consistent; the SNTP callback deliberately only sets a flag.
     *
     * The system clock is trusted when it is either freshly synced or already
     * past the dial's own baseline:
     *   - orb_net_time_synced(): SNTP stepped it during this boot.
     *   - time() >= CLOCK_EPOCH: IDF keeps the boot time in RTC slow memory, so
     *     an epoch set by the previous boot SURVIVES a warm reboot (measured:
     *     after POST /api/v1/reboot the epoch is already right ~1.5 s before the
     *     next SNTP reply lands). Trusting it is what stops the dial from sliding
     *     back to Nov-2023 for a few seconds every time the orb reboots.
     * Anything lower is the 1970 default after a power cut, so the dial keeps
     * free-running from CLOCK_EPOCH instead — a stale dial beats a 1970 dial, and
     * the hourly CONFIG_LWIP_SNTP_UPDATE_DELAY re-sync then only re-anchors when
     * the drift is big enough to be worth the visible jump. */
    const int64_t now_us = esp_timer_get_time();
    const time_t  real   = time(NULL);
    if (orb_net_time_synced() || real >= (time_t)CLOCK_EPOCH) {
        const double drift = (double)(real - s_epoch_base)
                           - (double)(now_us - s_t0_us) / 1e6;
        if (!s_anchor_is_real || fabs(drift) >= CLOCK_RESYNC_SLACK) {
            s_epoch_base     = real;
            s_t0_us          = now_us;
            s_anchor_is_real = true;
        }
    }

    const double  el   = (double)(now_us - s_t0_us) / 1e6;
    time_t  t   = s_epoch_base + (time_t)floor(el);
    double  frac = el - floor(el);

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

/* Per-second throughput report while the clock is live: how many composited
 * ticks we actually got, and what the flush path booked for the flushes they
 * produced (display_bench_snapshot counts every last-chunk DMA completion). */
static void report_fps_if_due(void)
{
    const uint32_t now = lv_tick_get();
    if (now - s_fps_t0 < 1000) return;
    uint32_t bf; uint64_t bt; uint32_t bm;
    display_bench_snapshot(&bf, &bt, &bm);
    uint64_t bpix; uint32_t bchunk;
    display_bench_stats(&bpix, &bchunk);
    const uint32_t df = bf - s_fps_bench_f0;
    const uint64_t dt = bt - s_fps_bench_t0;
    const uint64_t dpx = bpix - s_fps_pix0;
    const uint32_t dch = bchunk - s_fps_chunk0;
    const uint64_t sub = display_bench_submit_us();
    uint64_t pm, ph, pt, pd;
    display_bench_phases_us(&pm, &ph, &pt, &pd);
    const uint64_t dpm = pm - s_fps_ph0, dph = ph - s_fps_ph1;
    const uint64_t dpt = pt - s_fps_ph2, dpd = pd - s_fps_ph3;
    uint64_t rest, paint, cmp;
    gfx_hook_phases(&rest, &paint, &cmp);
    const uint64_t drest = rest - s_hk_rest_us0;
    const uint64_t dpant = paint - s_hk_paint_us0;
    const uint64_t dcmp  = cmp - s_hk_cmp_us0;
    uint32_t drb[CLK_PAINT_ROLES];
    uint64_t dru[CLK_PAINT_ROLES];
    gfx_role_snapshot(drb, dru);
    uint32_t dblend = 0;
    for (int i = 0; i < CLK_PAINT_ROLES; i++)
        dblend += drb[i];
    const uint32_t te_now = display_te_wait_us();
    ESP_LOGI(TAG, "clock %u fps | flush frames %u avg %llu ms | px/s %llu (%llu%% page) chunks/s %u te/s %llu ms submit/s %llu ms",
             (unsigned)s_fps_ticks, (unsigned)df,
             df ? (unsigned long long)(dt / df / 1000) : 0ULL,
             (unsigned long long)dpx,
             (unsigned long long)(dpx * 100 / ((uint64_t)CLK_SCR_W * CLK_SCR_H)),
             (unsigned)dch,
             (unsigned long long)((uint32_t)(te_now - s_fps_te0) / 1000),
             (unsigned long long)((sub - s_fps_sub0) / 1000));
    ESP_LOGI(TAG, "  phases ms/s: mirror %llu te %llu dma %llu | hook %llu (rest %llu paint %llu cmp %llu) | blend/s %u",
             (unsigned long long)(dpm / 1000), (unsigned long long)(dpt / 1000),
             (unsigned long long)(dpd / 1000), (unsigned long long)(dph / 1000),
             (unsigned long long)(drest / 1000),
             (unsigned long long)(dpant / 1000), (unsigned long long)(dcmp / 1000),
             (unsigned)dblend);
    /* Which layer owns the paint phase. Slots are the theme's: hr, min,
     * sec/carriage, dial, counterweight, glass — each counting its own shadow
     * too, as blends/s and the ms/s that took. Decides the next optimization by
     * measurement: a glass/dial-heavy sweep whose per-blend cost matches the
     * carriage's wants those static passes memoized per row; a carriage sweep
     * far dearer per blend is paying for scattered inverse-map fetches out of
     * PSRAM and wants its texel rows staged in internal RAM. */
    ESP_LOGI(TAG, "  blend/s by layer: hr %u/%llu min %u/%llu sec %u/%llu dial %u/%llu cw %u/%llu glass %u/%llu comp %u/%llu aux %u/%llu",
             (unsigned)drb[0], (unsigned long long)(dru[0] / 1000),
             (unsigned)drb[1], (unsigned long long)(dru[1] / 1000),
             (unsigned)drb[2], (unsigned long long)(dru[2] / 1000),
             (unsigned)drb[3], (unsigned long long)(dru[3] / 1000),
             (unsigned)drb[4], (unsigned long long)(dru[4] / 1000),
             (unsigned)drb[5], (unsigned long long)(dru[5] / 1000),
             (unsigned)drb[6], (unsigned long long)(dru[6] / 1000),
             (unsigned)drb[7], (unsigned long long)(dru[7] / 1000));
#if DISPLAY_BENCH
    /* Proof the synthetic ink is really on the glass: ink px/s must read
     * blocks x 3600 x fps, and the CAD bands/px fields must not collapse. */
    ESP_LOGI(TAG, "  bench: blocks %u ink px/s %llu",
             (unsigned)DISPLAY_BENCH_PX,
             (unsigned long long)(s_bench_px - s_bench_px0));
    s_bench_px0 = s_bench_px;
#endif
#if DISPLAY_CADENCE_PROBE
    display_cadence_drain();
#endif
    s_fps_sub0 = sub;
    s_fps_te0 = te_now;
    s_fps_ph0 = pm; s_fps_ph1 = ph; s_fps_ph2 = pt; s_fps_ph3 = pd;
    s_hk_rest_us0 = rest;
    s_hk_paint_us0 = paint; s_hk_cmp_us0 = cmp;
    s_fps_ticks = 0;
    s_fps_t0 = now;
    s_fps_bench_f0 = bf;
    s_fps_bench_t0 = bt;
    s_fps_pix0 = bpix;
    s_fps_chunk0 = bchunk;
}

static void tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_active) return;      /* parked: clock not on the glass */
    s_fps_ticks++;
    const uint32_t t_start_ms = lv_tick_get();
    if (gif_mode()) {
        /* Pace the GIF by its OWN delays: the tick rides the baked GIF delay
         * (gif_tick_ms), so the background advances exactly one frame per tick
         * at the authoring tempo — no due-time quantisation, no uneven holds.
         * The sweeping hand is sampled on the same beat (time-accurate angles
         * from the wall clock, just stepped at the GIF's rate). */
        const uint32_t now  = lv_tick_get();
        const int      next = (s_gif_shown + 1) % (int)clk_theme.gif_frames;
        if ((int32_t)(now - s_gif_due) >= 0) {
            gif_apply_frame(next);
            const uint32_t d = (uint32_t)clk_gif_delays[next] * CLK_GIF_THROTTLE;
            /* Anchor to the previous deadline so tick jitter cannot inflate
             * the tempo; if we fell more than a frame behind, drop missed
             * deadlines (no burst-catch-up: deltas are chained per frame). */
            s_gif_due += d;
            if ((int32_t)(now - s_gif_due) >= (int32_t)d)
                s_gif_due = now + d;
        }
    } else {
        s_bg_tick++;
        show_plate_frame(bg_frame_now());
    }
    compose_now();
    tourbillon_advance();          /* no-op unless the theme has a frame bank */
#if CLK_BG_DIRECT_PUSH
    /* Own every pixel any layer can reach, not just the ones that moved:
     * apply_angles marked the swept union (which clears the vacated wedge), this
     * marks the full box of each layer at its present pose (which is what a
     * hand that stands still or is clipped by the GIF rect's x-extent needs to
     * be restored and redrawn every tick). Marks union per row, so both
     * contributions apply, and the commit consumes them. */
    if (l4_active()) mark_all_layers();
#if DISPLAY_BENCH
    if (l4_active()) bench_plan();     /* walks the blocks + marks their rows */
#endif
    request_commit();
#endif
    report_fps_if_due();

    /* The GIF delta bank always runs on its own fastest beat; a non-looping
     * plate background only needs the fast beat while a play is running (the
     * check in bg_frame_now() may have just started one).
     * Deadline-anchored, not callback-anchored: lv_timer decides lateness
     * against a fixed 62.5 ms ladder we advance by the beat each tick, so
     * this callback's own work (and the LVGL refresh it queues) is schedule
     * to absorb, not drift to accumulate. Ask for a hair less than what is
     * left: the timer can only fire on a handler wake and lvgl_task's sleeps
     * quantise to ~5 ms, so an exact ask reliably lands one wake late.
     * Measured reality for this bank: the display pipeline books ~54 ms of
     * TE-wait + band submits per commit, so the tick still arrives ~67 ms
     * apart (~15 fps, loop ~1.07 s) — the ladder keeps the SCHEDULE honest;
     * true 16 needs ~5 ms shaved off the flush path, not the timer. */
    uint32_t period;
    if (gif_mode())                              period = gif_tick_ms();
    else if (bg_live() && (clk_theme.bg_loop || s_bg_play_start != 0))
                                                 period = bg_rate(tick_base_ms(), &s_bg_every);
    else                                         { s_bg_every = 1; period = tick_base_ms(); }
    if (gif_mode()) {
        if (!s_tick_due) s_tick_due = t_start_ms + period;
        else             s_tick_due += period;
        if ((int32_t)(t_start_ms - s_tick_due) >= (int32_t)period)
            s_tick_due = t_start_ms + period;       /* fell a beat behind */
        const int32_t left = (int32_t)(s_tick_due - t_start_ms) - 8;
        lv_timer_set_period(timer, (left < 2) ? 2 : (uint32_t)left);
#if DISPLAY_CADENCE_PROBE
        ESP_LOGI(TAG, "CAD tk now=%u due=%u gif_due=%u shown=%d",
                 (unsigned)t_start_ms, (unsigned)s_tick_due,
                 (unsigned)s_gif_due, s_gif_shown);
#endif
        return;
    }
    lv_timer_set_period(timer, period);
}

/* The plain hand beat: 1 s ticking, or 1000/tickRate (>= 33 ms) sweeping.
 * In GIF delta mode the whole dial runs on the fastest shared beat. */
static uint32_t tick_base_ms(void)
{
    if (gif_mode()) return gif_tick_ms();
    if (!clk_theme.second_sweep) return 1000;
    int rate = clk_theme.tick_rate;
    if (rate < 1) rate = 1;
    if (rate > 30) rate = 30;
    uint32_t p = 1000 / (uint32_t)rate;
    if (p < 33) p = 33;
    return p;
}

/* Port of bg_rate(): the fastest tick period that divides the background's
 * frame period evenly, holding `*every` ticks per frame. Never faster than
 * 33 ms (the panel ceiling) nor slower than the hand asked for. */
static uint32_t bg_rate(uint32_t base, uint32_t *every)
{
    const uint32_t need = 1000u / (uint32_t)(clk_theme.bg_fps < 1 ? 1 : clk_theme.bg_fps);
    uint32_t k = (need + base - 1) / base;                 /* ceil(need / base) */
    if (k < 1) k = 1;
    uint32_t per = need / k;
    if (per < 33) { per = 33; k = (need + per - 1) / per; }
    if (per > base) per = base;
    *every = k < 1 ? 1 : k;
    return per;
}

esp_err_t clock_view_init(void)
{
    /* The bank was already found and mapped by orb_theme_boot(), which ran BEFORE
     * display_init() — a theme install must complete before anything reads pixels,
     * and only orb_theme knows which A/B slot the bank is in. What is checked here
     * is that the mapping exists and covers what the geometry says it must: a stale
     * bin/partition pair, or a theme that would not load, fails loudly at this line
     * rather than as a wild read past the end of the mapping. */
    const esp_err_t art_err = art_bank_ready() && art_bank_bytes() >= CLK_BLOB_BYTES
                            ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (art_err != ESP_OK) {
        ESP_LOGE(TAG, "clock art bank unavailable (%s, %u B mapped for %u B) — "
                      "re-flash the \"%s\" partition or load another theme?",
                 esp_err_to_name(art_err), (unsigned)art_bank_bytes(),
                 (unsigned)CLK_BLOB_BYTES, ART_BANK_PARTITION_NAME);
        return art_err;
    }
    s_art = art_bank_ptr();
    ESP_LOGI(TAG, "art bank mapped: %u B (theme needs %u B) from %s",
             (unsigned)art_bank_bytes(), (unsigned)CLK_BLOB_BYTES, orb_theme_source());

    s_scr = lv_obj_create(NULL);
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
    if (clk_theme.bg_mode == 1) {
        const size_t canvas_bytes = (size_t)CLK_SCR_W * CLK_SCR_H * sizeof(uint16_t);
        s_canvas = heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas) {
            memcpy(s_canvas, blob() + clk_theme.bg_key_off, canvas_bytes);
            memset(&s_canvas_dsc, 0, sizeof(s_canvas_dsc));
            s_canvas_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
            s_canvas_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
            s_canvas_dsc.header.w      = CLK_SCR_W;
            s_canvas_dsc.header.h      = CLK_SCR_H;
            s_canvas_dsc.header.stride = (uint32_t)CLK_SCR_W * 2u;
            s_canvas_dsc.data_size     = (uint32_t)canvas_bytes;
            s_canvas_dsc.data          = (const uint8_t *)s_canvas;
            plate_src = &s_canvas_dsc;
        } else {
            ESP_LOGE(TAG, "canvas alloc failed (%u B); background held on keyframe",
                     (unsigned)canvas_bytes);
        }
        if (clk_theme.gif_frames > CLK_GIF_MAX)
            ESP_LOGE(TAG, "gif_frames %u exceeds baked max %d; background held still",
                     (unsigned)clk_theme.gif_frames, CLK_GIF_MAX);
    }
    if (!plate_src) {
        fill_plate_dsc(&s_plate_dsc);   /* plate_off == key_off in gif mode */
        build_bg_dscs();
        if (clk_theme.bg_frames > CLK_BG_MAX)
            ESP_LOGE(TAG, "bg_frames %u exceeds baked max %d; background held still",
                     (unsigned)clk_theme.bg_frames, CLK_BG_MAX);
        plate_src = &s_plate_dsc;
    }
    s_plate = lv_image_create(s_scr);
    lv_obj_set_size(s_plate, CLK_SCR_W, CLK_SCR_H);
    lv_obj_set_pos(s_plate, 0, 0);
    lv_obj_set_scrollable(s_plate, false);
    lv_image_set_src(s_plate, plate_src);
    s_bg_shown = 0;
    s_gif_shown = 0;
    /* Fresh widgets are all at rotation 0, so forget the angles we last pushed;
     * otherwise a rebuilt view would skip the re-pose as "unchanged". */
    for (int i = 0; i < 5; i++) s_ang_10[i] = INT32_MIN;

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
        /* Folded into the background at bake time (tools/bake_clock.py
         * --matte): the plate/bank already carries this layer's pixels, so an
         * object here would paint a SECOND copy over them. The bake also ships
         * the sprite blank, so this is the belt to that pair of braces -- it
         * is what keeps a half-swapped bin+header pair from double-blending. */
        if (clk_theme.matte_mask & (1u << k)) {
            if (!aux_built && oi == s_aux_at) { aux_create(); aux_built = true; }
            continue;
        }
        complications_create_bucket(geo, k);      /* the tiles under layer k */
        if (k <= 2 && clk_theme.shadow_on)
            s_shadow[k] = make_layer(&clk_theme.shadow[k], &s_shadow_dsc[k]);
        s_hand[k] = make_layer(&clk_theme.hand[k], &s_hand_dsc[k]);
        if (!aux_built && oi == s_aux_at) { aux_create(); aux_built = true; }
    }
    if (!aux_built) aux_create();                 /* no live static: ride the bank */
    /* What the dial actually built for the sub-dials, in the line the owner reads
     * after a re-bake: the count the theme declared, and the order position the
     * block joined. A row with an src this build cannot drive is reported by
     * aux_update the first time it tries, and shows no blends/s here either. */
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

    /* Tourbillon frame bank: descriptors for the second-hand layer's swap.
     * Reset the frame state so the first tick (re)draws it whole. */
    for (int i = 0; i < (int)clk_theme.tourbillon_n && i < CLK_TOURB_MAX; i++)
        if (gfx_sprite_drawable(&clk_theme.tourbillon[i]))
            gfx_fill_sprite_dsc(&s_tourb_dsc[i], &clk_theme.tourbillon[i], blob());
    s_tourb_shown = -1;
    s_tourb_t0 = lv_tick_get();


    s_t0_us = esp_timer_get_time();
    compose_now();
#if CLK_BG_DIRECT_PUSH
    display_set_commit_hook(clock_layer_commit);
#if DISPLAY_BENCH
    bench_init();
#endif
#endif

    uint32_t start_period;
    if (gif_mode())                            start_period = gif_tick_ms();
    else if (bg_live() && clk_theme.bg_loop)   start_period = bg_rate(tick_base_ms(), &s_bg_every);
    else                                       { s_bg_every = 1; start_period = tick_base_ms(); }
    s_tick_timer = lv_timer_create(tick_timer_cb, start_period, NULL);
    lv_timer_pause(s_tick_timer);   /* parked until the clock is on the glass */

    /* Report the cadence the bake actually recorded instead of a literal that
     * drifts out of date: the GIF and the tick share clk_gif_delays[] (the
     * GIF's own per-frame delays), and the sweeping hand beats at
     * tickRate when the theme declares one. */
    /* 96, not the 64 this shipped with: gif_frames, tick_rate and the delays are
     * runtime values now that a theme can arrive on an SD card, so the compiler can
     * no longer prove the worst case fits and -Werror=format-truncation says so. The
     * strings are still short — this is headroom for the widest digits, not a guess. */
    char bgdesc[96];
    if (gif_mode()) {
        uint32_t d0 = clk_gif_delays[0] > 0 ? (uint32_t)clk_gif_delays[0]
                                            : (uint32_t)CLK_GIF_TICK_MS;
        snprintf(bgdesc, sizeof bgdesc,
                 "gif delta bank: %u frames @ %u ms (%u.%u fps), tick %u ms",
                 (unsigned)clk_theme.gif_frames, (unsigned)d0,
                 (unsigned)(1000 / d0), (unsigned)((1000 % d0) * 10 / d0),
                 (unsigned)gif_tick_ms());
    } else if (bg_live()) {
        snprintf(bgdesc, sizeof bgdesc, "plate anim");
    } else {
        snprintf(bgdesc, sizeof bgdesc, "still");
    }

    char sweepdesc[48] = "off";   /* 32 -> 48 for the same reason as bgdesc above */
    if (clk_theme.second_sweep) {
        const uint32_t tpb = gif_mode() ? hand_beat_ticks() : 1;
        if (gif_mode() && tpb > 1)
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps, every %u ticks)",
                     (unsigned)clk_theme.tick_rate, (unsigned)tpb);
        else if (gif_mode())
            snprintf(sweepdesc, sizeof sweepdesc, "on(sweep, every tick)");
        else
            snprintf(sweepdesc, sizeof sweepdesc, "on");
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
     * every layer is composited by clock_layer_commit each tick. */
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
     * the whole face cleanly and resume. */
    s_active = on;
    if (!s_tick_timer) return;
    if (on) {
        if (gif_mode()) {
            /* The canvas still mirrors exactly what the glass held when we
             * parked — repaint it whole and keep cycling from the current
             * frame; resetting the counter would desync canvas vs deltas. */
            s_gif_due = lv_tick_get();
            s_tick_due = 0;                 /* re-anchor the beat ladder */
            s_bt_ctr = 0;                   /* first tick back is a beat */
            s_fps_t0 = lv_tick_get();
            s_fps_ticks = 0;
            uint32_t bf; uint64_t bt; uint32_t bm;
            display_bench_snapshot(&bf, &bt, &bm);
            s_fps_bench_f0 = bf;
            s_fps_bench_t0 = bt;
        } else {
            s_bg_tick = 0;                     /* restart the beat, as orb-os does
                                                * when a theme is (re)applied */
            s_bg_shown = -1;
            s_bg_play_start = 0;
            s_bg_last_play  = 0;
        }
        /* Tourbillon: force a whole first draw and restart the frame clock,
         * same discipline as the angle reset — nothing was composited while
         * parked, so whatever s_tourb_shown claims is stale. */
        s_tourb_shown = -1;
        s_tourb_t0 = lv_tick_get();
        lv_obj_invalidate(s_scr);
        if (!gif_mode()) show_plate_frame(bg_frame_now());
        compose_now();
        lv_timer_resume(s_tick_timer);
    } else {
        lv_timer_pause(s_tick_timer);
    }
}

bool clock_view_bounce(void)
{
    return s_active;
}


