/**
 * clock_view.c — themed ANALOG clock (Steam Punk et al.), ported from the
 * orb-os (Arduino) theme compositor onto LVGL 9 / ESP-IDF.
 *
 * The dial is composed as LVGL sibling objects rather than one CPU canvas:
 *
 *   plate (static RGB565 lv_image, never invalidated)
 *     -> shadows (planar RGB565A8 lv_image, rotated about the hand pivot)
 *     -> hands   (planar RGB565A8 lv_image, rotated about the hand pivot)
 *     -> overlay (hub cap), drawn last / on top
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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "display.h"

#include "clock_assets.h"   /* clk_theme, clk_sprite_t, clk_bg_frames, blob syms */

static const char *TAG = "clock";

/* Free-running time base: starts at a fixed epoch, ticks in wall-clock real
 * time. Replace the base with SNTP sync when WiFi is added. */
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
 * the affected rows and the L4 commit hook (paint_layer) rebuilds them straight
 * in the panel mirror: background restored out of the canvas, then the
 * hands/shadows/glass composited over it. 0 = old all-through-LVGL path
 * (A/B fallback). */
#define CLK_BG_DIRECT_PUSH 1

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
static uint32_t         s_fps_bench_f0;
static uint64_t         s_fps_bench_t0;
static uint64_t         s_fps_pix0;
static uint32_t         s_fps_chunk0;
static uint64_t         s_fps_sub0;

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

static const uint8_t *blob(void) { return clock_assets_bin_start; }

static uint32_t sprite_bytes(const clk_sprite_t *s)
{
    return (uint32_t)s->w * (uint32_t)s->h * 3u;   /* colour w*h*2 + alpha w*h */
}

static bool sprite_drawable(const clk_sprite_t *s)
{
    return s->show && s->w > 0 && s->h > 0 && s->color_off != 0;
}

/* Build an LVGL RGB565A8 descriptor for a cropped sprite: planar colour plane
 * (w*h*2, stride w*2) with the alpha plane (w*h, stride w) appended in the blob. */
static void fill_sprite_dsc(lv_image_dsc_t *d, const clk_sprite_t *s)
{
    memset(d, 0, sizeof(*d));
    d->header.magic      = LV_IMAGE_HEADER_MAGIC;
    d->header.cf         = LV_COLOR_FORMAT_RGB565A8;
    d->header.flags      = 0;
    d->header.w          = s->w;
    d->header.h          = s->h;
    d->header.stride     = (uint32_t)s->w * 2u;
    d->data_size         = sprite_bytes(s);
    d->data              = blob() + s->color_off;
}

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

static void swept_box(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out);
static void area_union(lv_area_t *a, const lv_area_t *b);
#if CLK_BG_DIRECT_PUSH
static inline void mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1);
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
 * the L4 commit hook (see paint_layer): the pushed rows are marked, and just
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
            mark_rows(rc->y, rc->y + rc->h - 1, rc->x, rc->x + rc->w - 1);
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
    if (!sprite_drawable(spr)) return NULL;

    fill_sprite_dsc(dsc, spr);

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

/* Seconds 0..60 (fractional while sweeping), minutes 0..60 (fractional unless a
 * railway dial sits it on a mark), hour 0..12 fractional. Mirrors orb-os
 * compose_custom: minute_hand_mins() and the p_min/p_hr derivation. */
static void hand_angles(double wall_frac, int hour, int min, int sec,
                        float ang[5])
{
    const bool sweep   = clk_theme.second_sweep;
    const bool railway = clk_theme.second_railway;

    /* High-beat escapement in GIF mode: the tick rides the GIF's baked delay
     * (gif_tick_ms), faster than a smooth sweep needs, so let the theme's
     * tickRate (beats per second) quantise the second hand to a 1/tickRate
     * grid instead of sampling the wall clock every tick. 28,800 vph = 8
     * bps: the seconds hand advances 0.75 deg 8x a second while the spring
     * background runs at the GIF's own ~16 fps. Between beats the angle is
     * unchanged, so apply_angles' eps gate skips re-posing the hand and the
     * tick's commit carries GIF rows only. Non-GIF themes are untouched —
     * there the tick already runs at tickRate, and sampling the wall clock
     * once per beat is the same quantisation by another route. */
    if (sweep && clk_theme.tick_rate > 1 && gif_mode()) {
        const double bps = (double)clk_theme.tick_rate;
        wall_frac = floor(wall_frac * bps) / bps;
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

/* ---- rotated-layer invalidation ------------------------------------------
 * A hand rotates about a pivot that sits far outside its own box (the second
 * hand's ink is a slim sliver whose box is nowhere near the sub-dial centre it
 * swings around), so the pixels it paints this tick and uncovers next tick form
 * a wedge, and that wedge's bounding box walks tens of pixels outside the
 * widget's box as it sweeps.
 *
 * lv_image_set_rotation() does invalidate both the old and the new transformed
 * box, but every one of those calls routes through lv_obj_invalidate_area(),
 * which clips the area to the widget's own coords expanded by ext_draw_size --
 * and ext_draw_size is refreshed for the NEW angle only. The outgoing wedge's
 * anti-aliased fringe therefore lands outside the clip and is never repainted
 * from the plate. On the glass that is exactly the two artifacts we had:
 * sparkle pixels stranded where the hand used to be, and a hairline running
 * along the edge of the hand's rectangle.
 *
 * Pushing the union of both wedges in through the PARENT sidesteps the child
 * clip (the screen's box is the whole panel), so LVGL re-renders the plate under
 * the entire swept region before drawing the hand back over it. Padded by a few
 * pixels because the software transform blends its edge AA past the geometric
 * box, which is precisely the band the clip was dropping. */
#define SWEPT_PAD 3

static void swept_box(const clk_sprite_t *spr, int32_t ang_10, lv_area_t *out)
{
    /* ang_10 is TENTHS of a degree: deg = ang_10/10, rad = ang_10 * PI/1800.
     * (The divisor here was 3600, i.e. the box was built for HALF the angle:
     * right at 12 o'clock, progressively wrong as the hand swept, which is why
     * the hour hand lost its tip and the second hand looked clipped at a straight
     * edge wherever the box stopped short of the ink paint_layer maps onto.) */
    const double rad = (double)ang_10 * (M_PI / 1800.0);
    const double c = cos(rad), s = sin(rad);
    int32_t x1 = 0x7fffffff, y1 = 0x7fffffff;
    int32_t x2 = -0x7fffffff, y2 = -0x7fffffff;

    for (int i = 0; i < 4; i++) {
        const double cx = (i & 1) ? (double)(spr->w - 1) : 0.0;
        const double cy = (i & 2) ? (double)(spr->h - 1) : 0.0;
        const double dx = cx - (double)spr->pivotx;
        const double dy = cy - (double)spr->pivoty;
        /* lv_point_transform()'s convention, with the pivot pinned to the
         * sprite's screen centre (obj->coords = center - pivot). */
        const double px = (double)spr->centerx + (c * dx - s * dy);
        const double py = (double)spr->centery + (s * dx + c * dy);
        const int32_t ix = (int32_t)floor(px), iy = (int32_t)floor(py);
        if (ix < x1) x1 = ix;
        if (iy < y1) y1 = iy;
        if (ix > x2) x2 = ix;
        if (iy > y2) y2 = iy;
    }

    x1 -= SWEPT_PAD; y1 -= SWEPT_PAD;
    x2 += SWEPT_PAD; y2 += SWEPT_PAD;
    out->x1 = (lv_coord_t)(x1 < 0 ? 0 : x1);
    out->y1 = (lv_coord_t)(y1 < 0 ? 0 : y1);
    out->x2 = (lv_coord_t)(x2 > CLK_SCR_W - 1 ? CLK_SCR_W - 1 : x2);
    out->y2 = (lv_coord_t)(y2 > CLK_SCR_H - 1 ? CLK_SCR_H - 1 : y2);
}

static void area_union(lv_area_t *a, const lv_area_t *b)
{
    if (b->x1 < a->x1) a->x1 = b->x1;
    if (b->y1 < a->y1) a->y1 = b->y1;
    if (b->x2 > a->x2) a->x2 = b->x2;
    if (b->y2 > a->y2) a->y2 = b->y2;
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

        if (prev != INT32_MIN) {
            /* Union of the old and new wedge of every layer sharing this angle,
             * in both their un-shifted (hand) and shifted (shadow) positions. */
            if (s_hand[i]) {
                swept_box(&clk_theme.hand[i],   prev, &u);
                lv_area_t n; swept_box(&clk_theme.hand[i], a, &n);
                area_union(&u, &n);
                have = true;
            }
            if (s_shadow[i]) {
                lv_area_t su; swept_box(&clk_theme.shadow[i], prev, &su);
                lv_area_t sn; swept_box(&clk_theme.shadow[i], a, &sn);
                area_union(&su, &sn);
                if (have) area_union(&u, &su);
                else { u = su; have = true; }
            }
        }
        else if (s_hand[i]) {
            swept_box(&clk_theme.hand[i], a, &u);
            if (s_shadow[i]) {
                lv_area_t sn; swept_box(&clk_theme.shadow[i], a, &sn);
                area_union(&u, &sn);
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
            if (l4_active()) mark_rows(u.y1, u.y2, u.x1, u.x2);
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
                if (l4_active()) mark_rows(u.y1, u.y2, u.x1, u.x2);
                else
#endif
                    lv_obj_invalidate_area(s_scr, &u);
            }
        }
        s_ang_10[i] = a;
    }
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

/* Rows this tick has dirtied and that the commit hook must re-composite from
 * scratch: background restored out of s_canvas, then every layer painted over
 * it in z-order.
 *
 * Two things are essential and were the source of the "hands missing /
 * trailing" bug:
 *  - the rows must ALSO be marked in the display's band set (display_mark_rows),
 *    or the mirror we repaint is never DMA'd and the glass keeps the old rows;
 *  - each marked row's whole painted span must be restored from the canvas
 *    first, or a layer that moved out of a pixel leaves it behind (ghost) and
 *    a layer that is re-painted blends over its own previous copy.
 * Restoring first also makes the hook idempotent, so the marks stay set for
 * the duration of the tick and are cleared at tick start, not at commit.
 *
 * Each row remembers the x-extent that must be restored: the union of every
 * pushed rect and every layer box that touched it. Narrower than the row, and
 * it can only grow within a tick, so a later mark never loses coverage. */
static uint8_t   s_layer_need_row[CLK_SCR_H];
static int32_t   s_layer_row_x0[CLK_SCR_H];
static int32_t   s_layer_row_x1[CLK_SCR_H];
static bool      s_need_commit;      /* something marked rows since last tick  */

static inline void mark_rows(int32_t y0, int32_t y1, int32_t x0, int32_t x1)
{
    if (y0 < 0) y0 = 0;
    if (y1 >= CLK_SCR_H) y1 = CLK_SCR_H - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= CLK_SCR_W) x1 = CLK_SCR_W - 1;
    if (y1 < y0 || x1 < x0) return;

    bool any = false;
    for (int32_t y = y0; y <= y1; y++) {
        if (!s_layer_need_row[y]) {
            s_layer_need_row[y] = 1;
            s_layer_row_x0[y] = x0;
            s_layer_row_x1[y] = x1;
        } else {
            if (x0 < s_layer_row_x0[y]) s_layer_row_x0[y] = x0;
            if (x1 > s_layer_row_x1[y]) s_layer_row_x1[y] = x1;
        }
        any = true;
    }
    if (any) {
        s_need_commit = true;
        display_mark_rows(y0, y1);
    }
}

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
    if (!s_need_commit || !l4_active()) return;
    s_need_commit = false;
    static const lv_area_t nudge = { 0, 0, 0, 0 };
    lv_obj_invalidate_area(s_plate, &nudge);
}

/* Every layer the painter owns, at the angle it is currently held at.
 *
 * Called once per tick, and it is what makes the hook the complete owner of the
 * rows it rebuilds. Marking only the layers whose angle CHANGED was the bug: a
 * hand that stands still (the hour hand for an hour at a time, the minute hand
 * under the 0.2-degree re-pose gate) never marked its own box, so the rows it
 * lives on carried nothing but the GIF rect's narrow x-extent. restore_marked_rows
 * then wiped that row back to plain background and paint_layer's per-row x-clip
 * refused to draw the hand outside it -- the hour hand lost its tip and the
 * minute hand was painted once and froze outside the animated area. */
static void mark_all_layers(void)
{
    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (s_ang_10[k] == INT32_MIN) continue;
        lv_area_t b;
        if (s_hand[k])   { swept_box(&clk_theme.hand[k],   s_ang_10[k], &b);
                           mark_rows(b.y1, b.y2, b.x1, b.x2); }
        if (s_shadow[k]) { swept_box(&clk_theme.shadow[k], s_ang_10[k], &b);
                           mark_rows(b.y1, b.y2, b.x1, b.x2); }
    }
    if (s_overlay && sprite_drawable(&clk_theme.overlay)) {
        lv_area_t b; swept_box(&clk_theme.overlay, 0, &b);
        mark_rows(b.y1, b.y2, b.x1, b.x2);
    }
}

/* lv_trigo_sin(), copied from lv_math.c: bit-exact table + 32767 clamp. */
static const uint16_t s_sin0_90[91] = {
    0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,
    5690,  6252,  6813,  7371,  7927,  8481,  9032,  9580,  10126, 10668,
    11207, 11743, 12275, 12803, 13328, 13848, 14365, 14876, 15384, 15886,
    16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174, 20622,
    21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730,
    25102, 25466, 25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088,
    28378, 28660, 28932, 29197, 29452, 29698, 29935, 30163, 30382, 30592,
    30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928, 32052, 32166,
    32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763,
    32768
};

static inline int32_t trigo_sin(int32_t angle)
{
    while (angle < 0)    angle += 360;
    while (angle >= 360) angle -= 360;
    int32_t ret;
    if (angle < 90)        ret =  s_sin0_90[angle];
    else if (angle < 180)  ret =  s_sin0_90[180 - angle];
    else if (angle < 270)  ret = -s_sin0_90[angle - 180];
    else                   ret = -s_sin0_90[360 - angle];
    return (ret == 32767) ? 32768 : (ret == -32767) ? -32768 : ret;
}

/* lv_color_16_16_mix_inlined(), copied from lv_draw_sw_blend_private.h. */
static inline uint16_t mix_16_16(uint16_t c1, uint16_t c2, uint8_t mix)
{
    if (mix == 255) return c1;
    if (mix == 0)   return c2;
    if (c1 == c2)   return c1;
    uint32_t mix5 = ((uint32_t)mix + 4) >> 3;
    uint32_t bg = (uint32_t)(c2 | ((uint32_t)c2 << 16)) & 0x7E0F81F;
    uint32_t fg = (uint32_t)(c1 | ((uint32_t)c1 << 16)) & 0x7E0F81F;
    uint32_t res = ((((fg - bg) * mix5) >> 5) + bg) & 0x7E0F81F;
    return (uint16_t)((res >> 16) | res);
}

/* Paint one rotated RGB565A8 layer over the rows flagged this frame, inside
 * the x-span flagged for each row (never outside: those pixels already hold a
 * correct composite and re-blending them would double-darken). */
static void paint_layer(const clk_sprite_t *spr, int32_t ang_10)
{
    if (!sprite_drawable(spr)) return;
    uint16_t *fb = display_fb_mirror();
    if (!fb) return;

    const uint8_t *base = blob();
    const uint16_t *col = (const uint16_t *)(base + spr->color_off);
    const uint8_t *alp  = base + spr->color_off
                          + (uint32_t)spr->w * (uint32_t)spr->h * 2u;

    /* The (padded) swept box is a superset of LVGL's transformed area;
     * out-of-image samples fall through the bounds test below. */
    lv_area_t box;
    swept_box(spr, ang_10, &box);

    /* Inverse-transform setup, copied from lv_draw_sw_transform(): scale is
     * fixed 256, so sxx..syy reduce to trig*64; x0_ups/y0_ups keep LVGL's
     * exact operation order (int32 products, truncating /256). */
    int32_t tr_ang = -(ang_10 % 3600);
    int32_t sinma, cosma;
    if (tr_ang == 0) { sinma = 0; cosma = 1024; }   /* LVGL's bit-exact case */
    else {
        const int32_t low = tr_ang / 10, rem = tr_ang - (tr_ang / 10) * 10;
        const int32_t s1 = trigo_sin(low),      s2 = trigo_sin(low + 1);
        const int32_t c1 = trigo_sin(low + 90), c2 = trigo_sin(low + 91);
        sinma = ((s1 * (10 - rem) + s2 * rem) / 10) >> 5;
        cosma = ((c1 * (10 - rem) + c2 * rem) / 10) >> 5;
    }
    const int32_t sxx =  cosma * 64, sxy = -(sinma * 64);
    const int32_t syx =  sinma * 64, syy =  cosma * 64;
    const int32_t xin0 = -(int32_t)spr->pivotx, yin0 = -(int32_t)spr->pivoty;
    const int32_t x0_ups =
        (int32_t)(((cosma * xin0 - sinma * yin0) * 64) / 256)
        + (int32_t)spr->pivotx * 256 + 0x80;
    const int32_t y0_ups =
        (int32_t)(((sinma * xin0 + cosma * yin0) * 64) / 256)
        + (int32_t)spr->pivoty * 256 + 0x80;

    const int32_t posx = (int32_t)spr->centerx - (int32_t)spr->pivotx;
    const int32_t posy = (int32_t)spr->centery - (int32_t)spr->pivoty;

    const int bmode = spr->blend;

    for (int32_t y = box.y1; y <= box.y2; y++) {
        if (!s_layer_need_row[y]) continue;
        const int32_t y_abs = y - posy;
        const int32_t xs_r = x0_ups + (int32_t)(((int64_t)sxy * y_abs) >> 8);
        const int32_t ys_r = y0_ups + (int32_t)(((int64_t)syy * y_abs) >> 8);
        uint16_t *fbrow = fb + (size_t)y * CLK_SCR_W;

        const int32_t xa = box.x1 > s_layer_row_x0[y] ? box.x1 : s_layer_row_x0[y];
        const int32_t xb = box.x2 < s_layer_row_x1[y] ? box.x2 : s_layer_row_x1[y];

        for (int32_t x = xa; x <= xb; x++) {
            const int32_t x_abs = x - posx;
            const int32_t xs_int = (xs_r + ((sxx * x_abs) >> 8)) >> 8;
            const int32_t ys_int = (ys_r + ((syx * x_abs) >> 8)) >> 8;
            if (xs_int < 0 || xs_int >= (int32_t)spr->w ||
                ys_int < 0 || ys_int >= (int32_t)spr->h) continue;

            const uint32_t sidx = (uint32_t)ys_int * spr->w + (uint32_t)xs_int;
            const uint8_t a = alp[sidx];
            if (!a) {
                continue;                   /* 3/4 of the box: skip  */
            }

            const uint16_t c = col[sidx];
            uint16_t *d = &fbrow[x];
            const uint16_t dest = __builtin_bswap16(*d);
            uint16_t out;

            if (bmode == 1) {          /* MULTIPLY (shadow layers) */
                if (c == 0xffff) continue;
                const uint16_t res =
                    (uint16_t)((((dest >> 11) * (c >> 11) >> 5) << 11) |
                              ((((dest >> 5) & 0x3F) * ((c >> 5) & 0x3F) >> 6) << 5) |
                              (((dest & 0x1F) * (c & 0x1F)) >> 5));
                out = mix_16_16(res, dest, a);
            }
            else if (bmode == 2) {     /* SCREEN (mapped to ADDITIVE) */
                if (c == 0x0000) continue;
                const uint32_t dr = dest >> 11, dg = (dest >> 5) & 0x3F, db = dest & 0x1F;
                const uint32_t sr = c >> 11,    sg = (c >> 5) & 0x3F,    sb = c & 0x1F;
                const uint32_t r = (dr + sr < 31 ? dr + sr : 31) << 11;
                const uint32_t g = (dg + sg < 63 ? dg + sg : 63) << 5;
                const uint32_t b = db + sb < 31 ? db + sb : 31;
                out = mix_16_16((uint16_t)(r | g | b), dest, a);
            }
            else {                     /* NORMAL */
                out = (a >= 255) ? c : mix_16_16(c, dest, a);
            }
            *d = __builtin_bswap16(out);
        }
    }
}

/* Restore the background of every marked row out of the canvas the plate
 * itself reads from, byte-swapped straight into the mirror. This is what makes
 * the whole pass idempotent: a layer that moved away has its pixels wiped back
 * to plain background, and a layer painted twice over the same row lands on
 * clean pixels each time. Doing it here (rather than leaning on LVGL to
 * re-render the plate) is what removes the re-composite cost. */
static void restore_marked_rows(uint16_t *fb)
{
    for (int32_t y = 0; y < CLK_SCR_H; y++) {
        if (!s_layer_need_row[y]) continue;
        const uint16_t *src = &s_canvas[(size_t)y * CLK_SCR_W + s_layer_row_x0[y]];
        uint16_t *d = fb + (size_t)y * CLK_SCR_W + s_layer_row_x0[y];
        for (int32_t x = s_layer_row_x0[y]; x <= s_layer_row_x1[y]; x++) {
            const uint16_t v = *src++;
            *d++ = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

/* display commit hook: rebuild the rows this tick dirtied from the ground up —
 * background out of the canvas, then every layer in the screen's z-order (all
 * shadows, then all hands per theme order, then the glass overlay) painted
 * over it.
 *
 * The marks are consumed HERE, not at the next tick. A commit only happens when
 * LVGL gets around to starting one, and at 47 ms per flush against a 40 ms GIF
 * beat the commit trails the tick that dirtied the rows: clearing at tick start
 * handed rows to a commit that never ran and dropped them, which is what made
 * hands vanish over freshly pushed background and leave a ghost at the pose the
 * first LVGL render had drawn. Rows that never get a commit simply stay marked
 * until one does. */
static void clock_layer_commit(void)
{
    if (!l4_active()) return;
    uint16_t *fb = display_fb_mirror();
    if (!fb) return;

    restore_marked_rows(fb);

    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (s_shadow[k] && s_ang_10[k] != INT32_MIN)
            paint_layer(&clk_theme.shadow[k], s_ang_10[k]);
    }
    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (s_hand[k] && s_ang_10[k] != INT32_MIN)
            paint_layer(&clk_theme.hand[k], s_ang_10[k]);
    }
    if (s_overlay) paint_layer(&clk_theme.overlay, 0);

    memset(s_layer_need_row, 0, sizeof(s_layer_need_row));
}
#endif /* CLK_BG_DIRECT_PUSH */

static void compose_now(void)
{
    const int64_t now  = esp_timer_get_time();
    const double  el   = (double)(now - s_t0_us) / 1e6;
    time_t  t   = (time_t)CLOCK_EPOCH + (time_t)floor(el);
    double  frac = el - floor(el);

    struct tm ti;
    localtime_r(&t, &ti);

    float ang[5];
    hand_angles(frac, ti.tm_hour, ti.tm_min, ti.tm_sec, ang);
    apply_angles(ang);
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
    ESP_LOGI(TAG, "clock %u fps | flush frames %u avg %llu ms | px/s %llu (%llu%% page) chunks/s %u te/s %llu ms submit/s %llu ms",
             (unsigned)s_fps_ticks, (unsigned)df,
             df ? (unsigned long long)(dt / df / 1000) : 0ULL,
             (unsigned long long)dpx,
             (unsigned long long)(dpx * 100 / ((uint64_t)CLK_SCR_W * CLK_SCR_H)),
             (unsigned)dch,
             (unsigned long long)(display_te_wait_us() / 1000),
             (unsigned long long)((display_bench_submit_us() - s_fps_sub0) / 1000));
    s_fps_sub0 = display_bench_submit_us();
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
#if CLK_BG_DIRECT_PUSH
    /* Own every pixel any layer can reach, not just the ones that moved:
     * apply_angles marked the swept union (which clears the vacated wedge), this
     * marks the full box of each layer at its present pose (which is what a
     * hand that stands still or is clipped by the GIF rect's x-extent needs to
     * be restored and redrawn every tick). Marks union per row, so both
     * contributions apply, and the commit consumes them. */
    if (l4_active()) mark_all_layers();
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
    const size_t have = (size_t)(clock_assets_bin_end - clock_assets_bin_start);

    /* The bake records the exact blob size in the generated header; a stale
     * bin/hdr pair (rebaked one but not the other) is caught loudly here
     * rather than as a wild read off the end of rodata. */
    if (have < CLK_BLOB_BYTES) {
        ESP_LOGE(TAG, "clock art too small: have %u B, need %u B (re-bake?)",
                 (unsigned)have, (unsigned)CLK_BLOB_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

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

    /* All shadows go down first, then all hands (orb-os: interleaving lets a
     * minute shadow smudge across the hour hand beneath it). Within each group,
     * respect the theme's own back-to-front order. */
    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        if (k <= 2 && clk_theme.shadow_on)
            s_shadow[k] = make_layer(&clk_theme.shadow[k], &s_shadow_dsc[k]);
    }
    for (int oi = 0; oi < clk_theme.order_n; ++oi) {
        const int k = clk_theme.order[oi];
        if (k < 0 || k > 4) continue;
        s_hand[k] = make_layer(&clk_theme.hand[k], &s_hand_dsc[k]);
    }

    /* Glass / hub cap last, on top of everything. */
    s_overlay = make_layer(&clk_theme.overlay, &s_overlay_dsc);

    s_t0_us = esp_timer_get_time();
    compose_now();
#if CLK_BG_DIRECT_PUSH
    display_set_commit_hook(clock_layer_commit);
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
    char bgdesc[64];
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

    char sweepdesc[24] = "off";
    if (clk_theme.second_sweep) {
        if (gif_mode() && clk_theme.tick_rate > 1)
            snprintf(sweepdesc, sizeof sweepdesc, "on(%u bps beat)",
                     (unsigned)clk_theme.tick_rate);
        else
            snprintf(sweepdesc, sizeof sweepdesc, "on");
    }

    ESP_LOGI(TAG, "analog clock: %d hand(s), shadow %s(%d,%d), sweep %s, bg %s",
             (clk_theme.hand[0].show ? 1 : 0) + (clk_theme.hand[1].show ? 1 : 0)
                 + (clk_theme.hand[2].show ? 1 : 0),
             clk_theme.shadow_on ? "on" : "off", clk_theme.shadow_dx, clk_theme.shadow_dy,
             sweepdesc, bgdesc);
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


