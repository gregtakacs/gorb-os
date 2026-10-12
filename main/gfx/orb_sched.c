/* main/gfx/orb_sched.c — the OS-owned frame scheduler and delta bank.
 *
 * Extracted from main/clock/clock_view.c in P1 with the code unchanged; see
 * orb_sched.h for what moved and where the seam is. gfx_parity_test.py lifts
 * every routine below out of both the pinned pre-extraction clock_view.c and
 * this file and compares token streams, so "unchanged" is checked, not claimed.
 *
 * The one thing this TU must never learn is how the art bank is mapped: the app
 * hands its blob() to orb_bg_init() and the delta reads stay a single load, so
 * the compositor keeps no art_bank dependency of its own.
 */

#include "orb_sched.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "orb_gfx.h"          /* CLK_SCR_W/H, the row marks, the commit band's counters */

/* After orb_gfx.h, which brings in clock_assets.h: orb_theme_geo.h #undefs the
 * generated header's names to redefine them as runtime reads, and must not be
 * expanded inside them. */
#include "orb_theme_geo.h"

/* Kept deliberately: this TU's logs must land under the tag the flash-gate
 * protocol and the boot-log checks already match on, so the extraction moves no
 * observable output along with the code. */
static const char *TAG = "clock";

/* bgAnim plate frames (baked in assets/clock_assets.h -> clk_bg_frames). */
#define CLK_BG_MAX 8

/* ---- GIF delta bank (bg_mode == 1) ---------------------------------------
 * s_canvas mirrors the pixels the panel currently holds under the plate. The
 * plate widget's image data points AT the canvas, so LVGL re-renders the plate
 * from it whenever an area is invalidated — blitting a frame's delta rects into
 * the canvas and invalidating exactly those areas (plus whatever the hand
 * rotations dirty themselves) is all it takes to animate the background with no
 * full-page repaint. */
#define CLK_GIF_MAX 32

/* Two motions share the one fast tick in GIF mode: the second hand sweeping
 * (which wants every tick to look smooth) and the GIF background advancing
 * (which does not -- a frame held a few ms longer is imperceptible). Whether
 * advancing the GIF only every Nth tick drops tiles is the question this flag
 * is here to answer -- read chunks/s in the fps line. Measured on the shipped
 * flagship bake: /1 = 9.0 tiles and 15 fps, /2 = 8.0 and 18, /3 = 8.3 and 18,
 * so the GIF contributes ~1 tile/frame and the plateau is mark_all_layers()'s
 * hand-box floor, not the GIF.
 * SHIPPED DEFAULT IS 1: background fidelity wins over the +3 fps a half-speed
 * GIF buys. Set to 2 only if throughput is prioritised over smoothness. */
#define CLK_GIF_THROTTLE 1

/* Mapped base of the art bank, handed in by orb_bg_init() from the app's own
 * blob() (NULL until then), so the bank reads here cost what they cost there. */
static const uint8_t *s_art;

static lv_obj_t        *s_plate;
static lv_image_dsc_t   s_plate_dsc;
static lv_image_dsc_t   s_bg_dsc[CLK_BG_MAX];
static uint16_t        *s_canvas;      /* PSRAM, CLK_SCR_W*CLK_SCR_H RGB565 */
static lv_image_dsc_t   s_canvas_dsc;
static int              s_gif_shown = 0;   /* frame the canvas holds now */
static uint32_t         s_gif_due;         /* lv_tick ms the next frame is due */
static uint32_t         s_bg_tick      = 0;   /* ticks since bounce-on, for the loop */
static uint32_t         s_bg_every     = 1;   /* ticks each bg frame is held for     */
static int              s_bg_shown     = -1;  /* plate frame on the glass now (0=still) */
static uint32_t         s_bg_play_start = 0;  /* ms; 0 = no non-loop play running */
static uint32_t         s_bg_last_play  = 0;  /* ms; start of the last finished play    */

/* Deadline ladder for the tick (see tick_timer_cb): lv_timer measures its
 * period from when the LAST RUN STARTED but we re-set the period from the
 * callback's END, so anything shorted from the requested period shows up
 * as schedule, not drift. 0 = not yet anchored (fresh start / unpark). */
static uint32_t         s_tick_due;
static uint32_t         s_fps_t0, s_fps_ticks;
static uint32_t         s_fps_bench_f0;
static uint64_t         s_fps_bench_t0;
static uint64_t         s_fps_pix0;
static uint32_t         s_fps_chunk0;
static uint64_t         s_fps_sub0;
static uint32_t         s_fps_te0;        /* display_te_wait_us() last read */
/* Commit-hook phase breakdown (per-second deltas on the fps line, ms): rest =
 * take the mirror row + wipe the staging row back to plain canvas, paint = every
 * layer's gfx_blend_px traffic, cmp = the flush gate. Per-layer blend counts
 * live next to gfx_paint_row_ctx. */
static uint64_t         s_hk_rest_us0, s_hk_paint_us0, s_hk_cmp_us0;
/* Previous-second baselines of display's commit-phase totals (mirror/hook/te/dma). */
static uint64_t         s_fps_ph0, s_fps_ph1, s_fps_ph2, s_fps_ph3;
#if DISPLAY_BENCH
/* The synthetic ink the app's bench paint pass blends, counted here because the
 * report that checks it against the DMA totals lives beside it. */
static uint64_t         s_bench_px, s_bench_px0;
#endif

static lv_timer_t      *s_tick_timer;
static bool             s_running;            /* the view is on the glass */
static orb_sched_policy_t s_policy;

/* ---- the delta bank ------------------------------------------------------ */

void orb_bg_init(const uint8_t *base)
{
    s_art = base;
}

/* Every pixel of the dial comes from here: the bank lives in the `assets` flash
 * partition and is mapped once at boot by art_bank_init(), so this stays a bare
 * pointer return and sprite reads cost what they always did. */
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

void orb_bg_set_plate(lv_obj_t *plate) { s_plate = plate; }
const uint16_t *orb_bg_canvas(void) { return s_canvas; }
const lv_image_dsc_t *orb_bg_plate_dsc(void) { return &s_plate_dsc; }
uint32_t *orb_bg_every_p(void) { return &s_bg_every; }

void orb_bg_init_frames(void)
{
    s_bg_shown = 0;
    s_gif_shown = 0;
}

void orb_bg_reset_play(void)
{
    s_bg_tick = 0;                        /* restart the beat, as orb-os does
                                           * when a theme is (re)applied */
    s_bg_shown = -1;
    s_bg_play_start = 0;
    s_bg_last_play  = 0;
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
uint32_t orb_bg_gif_tick_ms(void)
{
    const uint32_t d = clk_gif_delays[0];
    if (d < 20) return 20;              /* sanity floor: never spin past 50 fps */
    return d;
}

bool orb_bg_gif_mode(void)
{
    return clk_theme.bg_mode == 1 && clk_theme.gif_frames >= 2
           && clk_theme.gif_frames <= CLK_GIF_MAX && s_canvas != NULL;
}

bool orb_bg_bg_live(void)
{
    if (clk_theme.bg_mode == 1)
        return orb_bg_gif_mode();
    return clk_theme.bg_frames > 0 && clk_theme.bg_frames <= CLK_BG_MAX;
}

#if CLK_BG_DIRECT_PUSH
/* L4 only replaces LVGL when there is a canvas to restore background from;
 * without it (non-GIF themes, or the PSRAM canvas alloc failed) the classic
 * all-through-LVGL path stays in charge. */
bool orb_sched_l4_active(void)
{
    return CLK_BG_DIRECT_PUSH && s_canvas != NULL && orb_bg_gif_mode();
}

/* LVGL starts a commit only when it has something of its own to render, so a
 * tick that pushed background and moved hands purely through the painter has
 * to give it a reason: a 1 px plate nudge whose chunk mirrors a correct pixel
 * and whose is_last drives the commit our hook hangs on. */
static void request_commit(void)
{
    if (!gfx_commit_needed() || !orb_sched_l4_active()) return;
    gfx_commit_consumed();
    static const lv_area_t nudge = { 0, 0, 0, 0 };
    lv_obj_invalidate_area(s_plate, &nudge);
}
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
        if (orb_sched_l4_active()) {
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


/* ---- bgAnim: port of orb-os bg_anim_frame() / bg_rate() ------------------ */
static void build_bg_dscs(void)
{
    for (int i = 0; i < (int)clk_theme.bg_frames && i < CLK_BG_MAX; i++) {
        s_bg_dsc[i]     = s_plate_dsc;            /* same w/h/stride/cf */
        s_bg_dsc[i].data = blob() + clk_bg_frames[i];
    }
}

int orb_bg_frame_now(void)
{
    if (!orb_bg_bg_live()) return 0;

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

void orb_bg_show_plate_frame(int idx)
{
    if (!orb_bg_bg_live() || idx == s_bg_shown) return;
    s_bg_shown = idx;
    lv_image_set_src(s_plate, idx == 0 ? &s_plate_dsc : &s_bg_dsc[idx - 1]);
    lv_obj_invalidate(s_plate);
}

const lv_image_dsc_t *orb_bg_gif_canvas_init(void)
{
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
        return &s_canvas_dsc;
    }
    ESP_LOGE(TAG, "canvas alloc failed (%u B); background held on keyframe",
             (unsigned)canvas_bytes);
    if (clk_theme.gif_frames > CLK_GIF_MAX)
        ESP_LOGE(TAG, "gif_frames %u exceeds baked max %d; background held still",
                 (unsigned)clk_theme.gif_frames, CLK_GIF_MAX);
    return NULL;
}

void orb_bg_classic_init(void)
{
    fill_plate_dsc(&s_plate_dsc);   /* plate_off == key_off in gif mode */
    build_bg_dscs();
    if (clk_theme.bg_frames > CLK_BG_MAX)
        ESP_LOGE(TAG, "bg_frames %u exceeds baked max %d; background held still",
                 (unsigned)clk_theme.bg_frames, CLK_BG_MAX);
}

/* ---- the tick cadence ---------------------------------------------------- */

/* The plain hand beat: 1 s ticking, or 1000/tickRate (>= 33 ms) sweeping.
 * In GIF delta mode the whole dial runs on the fastest shared beat. */
uint32_t orb_sched_tick_base_ms(void)
{
    if (orb_bg_gif_mode()) return orb_bg_gif_tick_ms();
    if (!clk_theme.second_sweep) return 1000;
    int rate = clk_theme.tick_rate;
    if (rate < 1) rate = 1;
    if (rate > 30) rate = 30;
    uint32_t p = 1000 / (uint32_t)rate;
    if (p < 33) p = 33;
    return p;
}

/* Port of orb_sched_bg_rate(): the fastest tick period that divides the background's
 * frame period evenly, holding `*every` ticks per frame. Never faster than
 * 33 ms (the panel ceiling) nor slower than the hand asked for. */
uint32_t orb_sched_bg_rate(uint32_t base, uint32_t *every)
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

void orb_sched_ink_px_add(uint32_t n)
{
#if DISPLAY_BENCH
    s_bench_px += n;
#else
    (void)n;
#endif
}

static void tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_running) return;      /* parked: clock not on the glass */
    s_fps_ticks++;
    const uint32_t t_start_ms = lv_tick_get();
    if (orb_bg_gif_mode()) {
        /* Pace the GIF by its OWN delays: the tick rides the baked GIF delay
         * (orb_bg_gif_tick_ms), so the background advances exactly one frame per tick
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
        orb_bg_show_plate_frame(orb_bg_frame_now());
    }
    if (s_policy.tick) s_policy.tick(timer);
#if CLK_BG_DIRECT_PUSH
    /* Own every pixel any layer can reach, not just the ones that moved:
     * apply_angles marked the swept union (which clears the vacated wedge), this
     * marks the full box of each layer at its present pose (which is what a
     * hand that stands still or is clipped by the GIF rect's x-extent needs to
     * be restored and redrawn every tick). Marks union per row, so both
     * contributions apply, and the commit consumes them. */
    if (orb_sched_l4_active() && s_policy.mark_all) s_policy.mark_all();
#if DISPLAY_BENCH
    if (orb_sched_l4_active() && s_policy.bench) s_policy.bench();   /* walks the blocks + marks their rows */
#endif
    request_commit();
#endif
    report_fps_if_due();

    /* The GIF delta bank always runs on its own fastest beat; a non-looping
     * plate background only needs the fast beat while a play is running (the
     * check in orb_bg_frame_now() may have just started one).
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
    if (orb_bg_gif_mode())                              period = orb_bg_gif_tick_ms();
    else if (orb_bg_bg_live() && (clk_theme.bg_loop || s_bg_play_start != 0))
                                                 period = orb_sched_bg_rate(orb_sched_tick_base_ms(), &s_bg_every);
    else                                         { s_bg_every = 1; period = orb_sched_tick_base_ms(); }
    if (orb_bg_gif_mode()) {
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

/* ---- the cadence's own lifecycle ----------------------------------------- */

void orb_sched_start(uint32_t period_ms, const orb_sched_policy_t *policy)
{
    if (policy) s_policy = *policy;
    s_tick_timer = lv_timer_create(tick_timer_cb, period_ms, NULL);
    lv_timer_pause(s_tick_timer);   /* parked until the clock is on the glass */
}

bool orb_sched_started(void) { return s_tick_timer != NULL; }
bool orb_sched_running(void) { return s_running; }
void orb_sched_set_running(bool on) { s_running = on; }

void orb_sched_resume(void)
{
    if (s_tick_timer) lv_timer_resume(s_tick_timer);
}

void orb_sched_pause(void)
{
    if (s_tick_timer) lv_timer_pause(s_tick_timer);
}

void orb_sched_reanchor_gif(void)
{
    /* The canvas still mirrors exactly what the glass held when we parked —
     * repaint it whole and keep cycling from the current frame; resetting the
     * counter would desync canvas vs deltas. */
    s_gif_due = lv_tick_get();
    s_tick_due = 0;                 /* re-anchor the beat ladder */
}

void orb_sched_reset_fps(void)
{
    s_fps_t0 = lv_tick_get();
    s_fps_ticks = 0;
    uint32_t bf; uint64_t bt; uint32_t bm;
    display_bench_snapshot(&bf, &bt, &bm);
    s_fps_bench_f0 = bf;
    s_fps_bench_t0 = bt;
}
