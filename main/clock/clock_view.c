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
static uint16_t        *s_canvas;      /* PSRAM, CLK_SCR_W*CLK_SCR_H RGB565 */
static lv_image_dsc_t   s_canvas_dsc;
static int              s_gif_shown = 0;   /* frame the canvas holds now */
static uint32_t         s_gif_due;         /* lv_tick ms the next frame is due */
static uint32_t         s_fps_t0, s_fps_ticks;
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

/* Apply frame `tgt`'s delta rects into the PSRAM canvas and invalidate ONLY
 * those screen areas. The canvas already holds frame tgt-1 (baked that way),
 * so copying the changed rects makes it hold tgt exactly where tgt differs
 * perceptibly; unchanged pixels are left untouched and stay on-screen without
 * a flush. LVGL then re-renders + flushes the union of these rects and whatever
 * the hand rotations dirtied this same tick. */
static void gif_apply_frame(int tgt)
{
    const uint16_t lo = clk_bg_rect_first[tgt];
    const uint16_t hi = clk_bg_rect_first[tgt + 1];
    const uint8_t *base = blob();

    for (uint16_t r = lo; r < hi; r++) {
        const clk_bg_rect_t *rc = &clk_bg_rects[r];
        if (rc->w == 0 || rc->h == 0) continue;
        const uint16_t *src = (const uint16_t *)(base + rc->off);
        for (int y = 0; y < rc->h; y++) {
            uint16_t *dst = &s_canvas[(size_t)(rc->y + y) * CLK_SCR_W + rc->x];
            memcpy(dst, src + (size_t)y * rc->w, (size_t)rc->w * sizeof(uint16_t));
        }
        lv_area_t a = { rc->x, rc->y,
                        (lv_coord_t)(rc->x + rc->w - 1),
                        (lv_coord_t)(rc->y + rc->h - 1) };
        lv_obj_invalidate_area(s_plate, &a);
    }
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
    const double rad = (double)ang_10 * (M_PI / 3600.0);   /* 0.1 deg -> rad */
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
 * hour/minute are re-pose no-ops and the plate is never touched. */
static void apply_angles(const float ang[5])
{
    for (int i = 0; i < 5; i++) {
        const int32_t a = (int32_t)lroundf(ang[i] * 10.0f);
        if (a == s_ang_10[i]) continue;
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

        /* Through the parent: no child-box clip. */
        if (have) lv_obj_invalidate_area(s_scr, &u);

        if (s_hand[i])   lv_image_set_rotation(s_hand[i],   a);
        if (s_shadow[i]) lv_image_set_rotation(s_shadow[i], a);
        s_ang_10[i] = a;
    }
}

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
    if (gif_mode()) {
        /* Pace the GIF by its OWN delays: the tick runs at CLK_GIF_TICK_MS so
         * the sweeping hand stays smooth, but the background frame only
         * advances once the frame it is about to show has been due for its
         * native duration (40 ms here, 50 on the GIF's held frames). */
        const uint32_t now  = lv_tick_get();
        const int      next = (s_gif_shown + 1) % (int)clk_theme.gif_frames;
        if ((int32_t)(now - s_gif_due) >= 0) {
            gif_apply_frame(next);
            s_gif_due = lv_tick_get() + clk_gif_delays[next];
        }
    } else {
        s_bg_tick++;
        show_plate_frame(bg_frame_now());
    }
    compose_now();
    report_fps_if_due();

    /* The GIF delta bank always runs on its own fastest beat; a non-looping
     * plate background only needs the fast beat while a play is running (the
     * check in bg_frame_now() may have just started one). */
    uint32_t period;
    if (gif_mode())                              period = CLK_GIF_TICK_MS;
    else if (bg_live() && (clk_theme.bg_loop || s_bg_play_start != 0))
                                                 period = bg_rate(tick_base_ms(), &s_bg_every);
    else                                         { s_bg_every = 1; period = tick_base_ms(); }
    lv_timer_set_period(timer, period);
}

/* The plain hand beat: 1 s ticking, or 1000/tickRate (>= 33 ms) sweeping.
 * In GIF delta mode the whole dial runs on the fastest shared beat. */
static uint32_t tick_base_ms(void)
{
    if (gif_mode()) return CLK_GIF_TICK_MS;
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

    uint32_t start_period;
    if (gif_mode())                            start_period = CLK_GIF_TICK_MS;
    else if (bg_live() && clk_theme.bg_loop)   start_period = bg_rate(tick_base_ms(), &s_bg_every);
    else                                       { s_bg_every = 1; start_period = tick_base_ms(); }
    s_tick_timer = lv_timer_create(tick_timer_cb, start_period, NULL);
    lv_timer_pause(s_tick_timer);   /* parked until the clock is on the glass */

    ESP_LOGI(TAG, "analog clock: %d hand(s), shadow %s(%d,%d), sweep %s, bg %s",
             (clk_theme.hand[0].show ? 1 : 0) + (clk_theme.hand[1].show ? 1 : 0)
                 + (clk_theme.hand[2].show ? 1 : 0),
             clk_theme.shadow_on ? "on" : "off", clk_theme.shadow_dx, clk_theme.shadow_dy,
             clk_theme.second_sweep ? "on" : "off",
             gif_mode() ? "gif delta bank (20 fps target)"
                        : (bg_live() ? "plate anim" : "still"));
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


