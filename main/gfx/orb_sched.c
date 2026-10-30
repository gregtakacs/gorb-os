/* main/gfx/orb_sched.c — the OS-owned frame scheduler and delta bank.
 *
 * Extracted from main/clock/clock_view.c in P1 with the code unchanged; see
 * orb_sched.h for what moved and where the seam is. gfx_parity_test.py lifts
 * every routine below out of both the pinned pre-extraction clock_view.c and
 * this file and compares token streams, so "unchanged" is checked, not claimed.
 *
 * The one thing this TU used to be able to say about itself — that it keeps no
 * art-bank dependency of its own — is no longer true as of Migration 2's commit
 * hook. orb_sched_commit_layers() reads art_bank_ptr() and the broker's table
 * getter, because a tier-1 layer row's offset is an offset into the live
 * payload, and only the OS knows that payload. The rule that survives is the
 * narrower one the hook exists to serve: the DELTA BANK still never maps the
 * partition itself — orb_bg_init() takes the app's own blob() base, and every
 * GIF and plate read below goes through s_art. What the hook adds is one read
 * of the mapped payload, nothing else. As of Commit 4 this TU also OWNS the
 * hook: sched_flush_commit() is what display's flush calls, and the dial's
 * private commit hook is gone — the app layer table is the only L4 job source
 * on the glass.
 */

#include "orb_sched.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "display.h"
#include "orb_gfx.h"          /* CLK_SCR_W/H, the row marks, the commit band's counters */
#include "orb_layers.h"       /* orb_layer_tab_t, ORB_LAYER_JOB_MAX, orb_layers_jobs  */
#include "orb_svc.h"          /* orb_svc_layer_table: the broker's read door          */
#include "orb_app.h"          /* orb_app_current: whose table the commit pass composites */
#include "art_bank.h"         /* art_bank_ptr: the payload a tier-1 row's off indexes  */

/* After orb_gfx.h, which brings in clock_assets.h: orb_theme_geo.h #undefs the
 * generated header's names to redefine them as runtime reads, and must not be
 * expanded inside them. */
#include "orb_theme_geo.h"

/* Kept deliberately: this TU's logs must land under the tag the flash-gate
 * protocol and the boot-log checks already match on, so the extraction moves no
 * observable output along with the code. */
static const char *TAG = "clock";

/* bgAnim plate frames: the bound is clk_types.h's CLK_BG_MAX (§31), the one
 * number the projection refuses a plate bank against and this array's size.
 * It arrives through orb_gfx.h above. */

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
 * so the GIF contributes ~1 tile/frame and the plateau was the blanket
 * static-layer re-mark's hand-box floor, not the GIF — a discipline since
 * deleted (Migration 2 Commit 4): rows mark only what moved.
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

/* The plain hand beat: 1000/tickRate (>= 33 ms) whenever the theme NAMES a beat
 * faster than one a second, and 1 s for the classic deadbeat. In GIF delta mode
 * the whole dial runs on the fastest shared beat.
 *
 * The beat used to be read only for a sweep, and a TICKING theme fell to 1000 ms
 * however fast it named. clock_view's hand_angles quantises the wall fraction
 * into tick_rate steps, so a 4/s dial woken once a second showed one of those
 * four steps a second — the Motion dropdown's tick rungs were inert on a dial
 * with no bank to drive the beat, which is every still dial (the Station report
 * of Oct 30, the other half of it). Honour the named beat for both looks: a
 * theme that names nothing keeps 1 Hz exactly as FORMAT 2 classic art always ran,
 * and a still dial is no longer the one art class whose cadence the OS ignored. */
uint32_t orb_sched_tick_base_ms(void)
{
    if (orb_bg_gif_mode()) return orb_bg_gif_tick_ms();
    const int rate_in = clk_theme.tick_rate;
    if (!clk_theme.second_sweep && rate_in < 2) return 1000;
    int rate = rate_in;
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

/* P8.2's mode-aware rung pick. Pure — see the header for the no-alias rule, the
 * two rate modes, and the working cap. Ladder rungs a DISTINCT ask may sit on
 * and the tick may run on (§40), descending so the first hit is the highest.
 * A 0 ask imposes nothing in any mode; a FREE ask rides the working cap. */
static uint32_t rung_gcd(uint32_t a, uint32_t b)
{
    while (b) { uint32_t t = a % b; a = b; b = t; }
    return a;
}

static const uint8_t RUNG_LADDER[] = { 12, 8, 4, 2, 1 };
#define RUNG_LADDER_N (sizeof RUNG_LADDER / sizeof RUNG_LADDER[0])

/* Clamp an animated rate into the owner-signed [4..15] band: below the panel
 * floor the pick cannot be honoured anyway; above the ceiling nothing beat. */
static uint32_t rung_clamp(uint32_t fps)
{
    if (fps < ORB_SCHED_PANEL_FLOOR) return ORB_SCHED_PANEL_FLOOR;
    if (fps > ORB_SCHED_FPS_CEILING) return ORB_SCHED_FPS_CEILING;
    return fps;
}

uint32_t orb_sched_pick_rung(const orb_sched_ask_t *asks, unsigned n, uint32_t floor_fps)
{
    /* The working cap starts at the MEASURED floor (never an ask) and is bound
     * tighter by every fluid cap; it lives inside [4..15] by definition, so the
     * animated band needs no later clamp on the free/fluid return. */
    uint32_t cap = rung_clamp(floor_fps ? floor_fps : 1);

    uint32_t lcm = 1;        /* the divisor every distinct ask must divide into */
    int      over = 0;       /* an LCM past the panel range: no even rung exists */
    int      any_distinct = 0;
    uint16_t slowest = 0;    /* the set's floor: the slowest distinct ask */

    for (unsigned i = 0; i < n; i++) {
        const orb_sched_ask_t *a = &asks[i];
        if (a->fps == 0) continue;                       /* a 0 ask binds nothing */
        if (a->mode == ORB_SCHED_FREE) continue;         /* rides the cap */
        if (a->mode == ORB_SCHED_FLUID) {
            uint32_t c = rung_clamp(a->fps);             /* fluid: a cap, not a divisor */
            if (c < cap) cap = c;
            continue;
        }
        /* DISTINCT: a divisor, and a ladder rung by install-time construction. */
        any_distinct = 1;
        if (slowest == 0 || a->fps < slowest) slowest = a->fps;
        if (!over) {
            uint32_t g = rung_gcd(lcm, a->fps);
            uint32_t l = lcm / g * a->fps;
            if (l == 0 || l > 1000) over = 1;            /* overflow guard, not policy */
            else lcm = l;
        }
    }

    if (!any_distinct) return cap;     /* fluid/free only: run at the working cap */

    /* Highest ladder rung at or under the cap that every distinct ask divides
     * into, so no constrained element aliases. The ladder is descending: the
     * first rung that fits is the answer. */
    if (!over) {
        for (unsigned r = 0; r < RUNG_LADDER_N; r++) {
            uint32_t rung = RUNG_LADDER[r];
            if (rung > cap) continue;
            if (rung % lcm != 0) continue;
            return rung;
        }
    }
    /* No clean rung under the cap: the honest answer is the set's own floor,
     * P8.1's fallback kept, clamped into the animated band. */
    return rung_clamp(slowest);
}

/* ---- the tick's live cadence (P8.2: the pick, not the bake, owns the beat) --
 *
 * The dial's asks come from the live theme; floor_fps comes from the running
 * measurement window, never an ask (the header demands it). The GIF's authored
 * tempo is a distinct divisor, so the pick cannot alias one-frame-per-tick; the
 * plate's every-K hold is an OUTPUT of the pick now (K = frame period / tick
 * period), replacing orb_sched_bg_rate's separate tick-side decision while the
 * decimation itself stays. */
#define SCHED_DIAL_ASKS 4
static orb_sched_ask_t s_dial_asks[SCHED_DIAL_ASKS];
static unsigned        s_dial_n;
static uint32_t        s_floor_fps    = ORB_SCHED_FPS_CEILING; /* hysteresis cap  */
static uint32_t        s_rung_fps     = ORB_SCHED_FPS_CEILING; /* current pick    */
static uint32_t        s_rung_period  = 66;                     /* 1000/rung, ms    */
static uint32_t        s_demote_run, s_promote_run;
static uint32_t        s_last_demote_ms;
static bool            s_floor_seeded;

static void sched_fill_asks(void);
static void sched_seed_floor(void);

/* The dial's per-element asks, read from the live bake (§40). GIF mode names the
 * bank's baked tempo as a distinct divisor; a plate bank names its bg_fps; the
 * carriage names its tourbillon tempo; the sweep is a fluid cap bounded by the
 * declared tick_rate; a still dial is one free ask that rides the floor. */
static void sched_fill_asks(void)
{
    unsigned n = 0;
    if (orb_bg_gif_mode()) {
        const uint32_t gt = orb_bg_gif_tick_ms() ? orb_bg_gif_tick_ms() : 1;
        s_dial_asks[n].fps  = (uint16_t)((1000 + gt / 2) / gt);   /* round to fps */
        s_dial_asks[n].mode = ORB_SCHED_DISTINCT;
        n++;
    } else if (orb_bg_bg_live()) {
        s_dial_asks[n].fps  = (uint16_t)(clk_theme.bg_fps < 1 ? 1 : clk_theme.bg_fps);
        s_dial_asks[n].mode = ORB_SCHED_DISTINCT;
        n++;
    }
    if (clk_theme.tourbillon_n) {
        s_dial_asks[n].fps  = (uint16_t)(clk_theme.tourbillon_fps < 1 ? 1 : clk_theme.tourbillon_fps);
        s_dial_asks[n].mode = ORB_SCHED_DISTINCT;
        n++;
    }
    if (clk_theme.second_sweep) {
        s_dial_asks[n].fps  = (uint16_t)(clk_theme.tick_rate ? clk_theme.tick_rate
                                                             : ORB_SCHED_FPS_CEILING);
        s_dial_asks[n].mode = ORB_SCHED_FLUID;
        n++;
    }
    if (n == 0) {                       /* static content: free, rides the floor */
        s_dial_asks[n].fps  = 0;
        s_dial_asks[n].mode = ORB_SCHED_FREE;
        n++;
    }
    s_dial_n = n;
}

/* The tick's period from the unified ladder: an animated dial — a GIF, a
 * looping/playing plate, a carriage, or a sweep — rides the picked rung; a
 * static one rides the hand beat (orb_sched_tick_base_ms), which the pick does
 * not schedule (§40). The plate's play-running test is live, so a one-shot play
 * that just started speeds the beat this tick, as the retired per-tick gate did. */
static uint32_t sched_tick_period_ms(void)
{
    const bool anim = orb_bg_gif_mode()
                   || (orb_bg_bg_live() && (clk_theme.bg_loop || s_bg_play_start != 0))
                   || clk_theme.tourbillon_n
                   || clk_theme.second_sweep;
    if (!anim) return orb_sched_tick_base_ms();
    if (!s_floor_seeded) { sched_fill_asks(); sched_seed_floor(); }  /* first animated beat */
    return s_rung_period ? s_rung_period : 1000;
}

/* Apply the picked rung: its period, and the plate's every-K hold as an OUTPUT
 * of the pick (K = frame period / tick period). GIF and static carry K=1. */
static void sched_apply_rung(void)
{
    s_rung_period = 1000u / (s_rung_fps ? s_rung_fps : 1);
    s_bg_every    = 1;
    if (!orb_bg_gif_mode() && orb_bg_bg_live() && clk_theme.bg_fps >= 1) {
        const uint32_t frame_p = 1000u / (uint32_t)clk_theme.bg_fps;
        uint32_t k = (frame_p + s_rung_period - 1) / (s_rung_period ? s_rung_period : 1);
        s_bg_every = k < 1 ? 1 : k;                /* the plate advances every K ticks */
    }
}

/* Seed the first-second floor from the §33 delivered table for the bank class
 * now on the glass, so the pick starts at the honest figure and the running
 * window only ever moves it from there. */
static void sched_seed_floor(void)
{
    if (orb_bg_gif_mode())     s_floor_fps = 12;   /* thin GIF bank: §33 delivers 12 */
    else if (orb_bg_bg_live()) s_floor_fps = ORB_SCHED_PANEL_FLOOR;  /* full-panel plate */
    else                       s_floor_fps = ORB_SCHED_FPS_CEILING;  /* still/sweep */
    s_rung_fps    = orb_sched_pick_rung(s_dial_asks, s_dial_n, s_floor_fps);
    sched_apply_rung();
    s_floor_seeded = true;
}

/* One rung move, logged once, quoting the measured figure it answers. */
static void sched_rung_move(uint32_t from_fps)
{
    s_rung_fps = orb_sched_pick_rung(s_dial_asks, s_dial_n, s_floor_fps);
    if (s_rung_fps == from_fps) return;          /* floor moved; the rung did not */
    sched_apply_rung();
    ESP_LOGI(TAG, "RUNG %u->%u fps (measured floor %u)",
             (unsigned)from_fps, (unsigned)s_rung_fps, (unsigned)s_floor_fps);
}

/* The report-second hysteresis (§40, signed as drafted): demote after
 * ORB_SCHED_HYST_DEMOTE_SEC report-seconds delivering under the pick, rate-
 * limited to one move per ORB_SCHED_HYST_DEMOTE_MS; promote after
 * ORB_SCHED_HYST_PROMOTE_SEC stable seconds carrying ORB_SCHED_HYST_SLACK_PCT%
 * headroom. A distinct GIF/carriage pin moves the floor but not the rung, so no
 * line prints — a rung that cannot fire is not claimed to have fired. */
static void sched_hysteresis(uint32_t measured)
{
    sched_fill_asks();                       /* the asks follow the live bake  */
    if (!s_floor_seeded) sched_seed_floor(); /* first second after a flip/theme  */
    const uint32_t now = lv_tick_get();
    if (measured < s_rung_fps) {
        s_promote_run = 0;
        if (++s_demote_run >= ORB_SCHED_HYST_DEMOTE_SEC &&
            (uint32_t)(now - s_last_demote_ms) >= ORB_SCHED_HYST_DEMOTE_MS) {
            const uint32_t from = s_rung_fps;
            s_floor_fps = measured ? measured : ORB_SCHED_PANEL_FLOOR;
            if (s_floor_fps < ORB_SCHED_PANEL_FLOOR) s_floor_fps = ORB_SCHED_PANEL_FLOOR;
            s_demote_run = 0;
            s_last_demote_ms = now;
            sched_rung_move(from);
        }
        return;
    }
    s_demote_run = 0;
    if (measured >= (uint32_t)((s_rung_fps * (100 + ORB_SCHED_HYST_SLACK_PCT)) / 100)) {
        if (++s_promote_run >= ORB_SCHED_HYST_PROMOTE_SEC &&
            s_floor_fps < ORB_SCHED_FPS_CEILING) {
            const uint32_t from = s_rung_fps;
            s_floor_fps = ORB_SCHED_FPS_CEILING;   /* the pick re-clamps it down */
            s_promote_run = 0;
            sched_rung_move(from);
        }
    } else {
        s_promote_run = 0;                       /* steady, but no promote slack */
    }
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
    /* The report-second boundary is where the measured figure meets the hysteresis
     * state machine (P8.2): the composited ticks in this window are what the glass
     * actually bore, so feed them (scaled to a clean per-second rate) to the
     * demote/promote gate before the counters reset. */
    const uint32_t elapsed = now - s_fps_t0;
    const uint32_t measured = elapsed
        ? (uint32_t)(((uint64_t)s_fps_ticks * 1000u + elapsed / 2u) / elapsed) : 0;
    sched_hysteresis(measured);
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

/* The commit hook's job array. Static rather than per-call stack: with the
 * table's ceiling at ORB_LAYER_MAX (16) it is ~1 KB of the LVGL task's stack
 * per flush, and that stack is not ours to spend — the same reasoning
 * clock_view.c's retired hook gave its own list. One caller (the flush hook
 * below), on one task (the LVGL one), exactly as that retired hook ran. */
static gfx_paint_ctx_t s_lay_jc[ORB_LAYER_JOB_MAX];

/* ---- the layer-table commit pass (Migration 2, P8 Commits 2-4) -----------
 *
 * The OS's own commit path for anything that draws through the app layer
 * table: read the entry on the glass's rows, build the job list in table order
 * (paint order IS table order — §1.2a), and hand it to the same gfx_commit()
 * the dial's private hook used to drive. The app never names a paint context,
 * a sprite or a row mark, which is the whole point of the table.
 *
 * `theme_base` is art_bank_ptr(), the mapped live payload — NULL before
 * art_bank_init() and for a board with no theme, and then every base-less row
 * draws nothing (orb_layers.c:320-323), which is what a dial with no art is
 * supposed to do. A tier-1 row's `art.off` indexes THAT, not the clock's blob:
 * the fold happens at the resolve step, into orb_asset_t.off — see the shipped
 * clock's build pass (clock_view.c's clock_layers_build) and the base
 * convention vectors in tools/host_orblayers_test.py section 14.
 *
 * The mark-set ownership that made Commits 2 and 3 land this pass in the tick
 * as an idle guard is closed: the legacy renderer that marked its rows in the
 * tick and committed them at the flush is deleted, and this pass now owns the
 * one weld point — sched_flush_commit(), registered with display's flush
 * below and in orb_sched_start(). Every mark site (the dial's wedges, the GIF
 * pushes, the tourbillon diff, the table's own wake touches) leaves its damage
 * in the row bank; the flush welds it all in one pass, after the chunks
 * mirror and before the bands DMA. Committing earlier — in the tick — would
 * leave any LVGL-initiated repaint (a bounce, a menu return) welded by nobody
 * and is gone for that reason.
 */
void orb_sched_commit_layers(const orb_app_t *active)
{
    const orb_layer_tab_t *t = orb_svc_layer_table(active);
    if (!t || orb_layers_count(t) == 0) return;
    if (!s_canvas) return;            /* no canvas, nothing to restore a marked
                                       * row FROM: the classic LVGL path owns
                                       * those pixels, as it owns them for a
                                       * non-GIF dial today */
    int nj = orb_layers_jobs(t, art_bank_ptr(), s_lay_jc, ORB_LAYER_JOB_MAX);
    if (nj) gfx_commit(nj, s_lay_jc, s_canvas);
}

#if CLK_BG_DIRECT_PUSH
/* The weld point (Migration 2, Commit 4): what display's flush calls once per
 * commit, after every dirty chunk has mirrored into the panel buffer and
 * before the bands DMA it out — the same slot clock_layer_commit occupied
 * until this commit deleted it.
 *
 * The glass gate is load-bearing and inherited whole from that hook: the tick
 * timer is paused off-glass but the flush is not, and since D35 home is the
 * menu shell, so the first commit after boot belongs to the MENU's frame. A
 * writer into the mirror with no view on the glass writes nothing; the marks
 * ride to the next commit, where either they flush as that screen's pixels or
 * the first activation's bounce repaints them whole. The l4 test is the
 * retired hook's first line kept: without it a board falling back to the
 * all-through-LVGL path (CLK_BG_DIRECT_PUSH=0 builds aside, a GIF bank that
 * outgrew the canvas) would weld an app table over LVGL's own render. */
static void sched_flush_commit(void)
{
    if (!orb_sched_l4_active()) return;
    if (!orb_sched_running()) return;
    orb_sched_commit_layers(orb_app_current());
}
#endif

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
#if DISPLAY_BENCH
    if (orb_sched_l4_active() && s_policy.bench) s_policy.bench();   /* walks the blocks + marks their rows */
#endif
    /* The marks this tick made — the dial's wedges and frame diffs, the GIF's
     * pushed rows, the table pass's wake touches — weld at the flush, where
     * sched_flush_commit() composites the app's rows over them before the DMA
     * reads a line. (Migration 2, Commit 4: the blanket static re-mark and the
     * interim tick-side commit both retired; the panel never sees a weld half
     * done, because the weld happens where the pixels leave.) */
    request_commit();
#endif
    report_fps_if_due();

    /* P8.2: the OS's pick owns the beat, not the bake. The period is the picked
     * rung for an animated dial, the hand beat for a static one
     * (sched_tick_period_ms). The GIF rides the rung too, and because the bank's
     * tempo is a DISTINCT divisor the pick cannot alias it — one frame advances
     * per tick, exactly as its own s_gif_due deadline above still drives.
     * Deadline-anchored for every mode now, not just the GIF: lv_timer decides
     * lateness against a ladder we advance by the beat each tick, so this
     * callback's own work (and the LVGL refresh it queues) is schedule to
     * absorb, not drift to accumulate. Ask a hair under what is left: the timer
     * only fires on a handler wake and lvgl_task's sleeps quantise to ~5 ms, so
     * an exact ask reliably lands one wake late. Measured reality for the thin
     * bank: the flush path books ~54 ms of TE-wait + band submits per commit, so
     * a rung asking 15 still arrives ~67 ms apart (~12 fps delivered) — the
     * ladder keeps the SCHEDULE honest; true 16 needs the flush path trimmed. */
    const uint32_t period = sched_tick_period_ms();
    if (!s_tick_due) s_tick_due = t_start_ms + period;
    else             s_tick_due += period;
    if ((int32_t)(t_start_ms - s_tick_due) >= (int32_t)period)
        s_tick_due = t_start_ms + period;            /* fell a beat behind */
    const int32_t left = (int32_t)(s_tick_due - t_start_ms) - 8;
    lv_timer_set_period(timer, (left < 2) ? 2 : (uint32_t)left);
#if DISPLAY_CADENCE_PROBE
    ESP_LOGI(TAG, "CAD tk now=%u due=%u gif_due=%u shown=%d",
             (unsigned)t_start_ms, (unsigned)s_tick_due,
             (unsigned)s_gif_due, s_gif_shown);
#endif
}

/* ---- the cadence's own lifecycle ----------------------------------------- */

void orb_sched_start(uint32_t period_ms, const orb_sched_policy_t *policy)
{
    if (policy) s_policy = *policy;
#if CLK_BG_DIRECT_PUSH
    /* Migration 2, Commit 4: the app layer table's weld point. Display calls
     * the hook at every commit, and the gate inside it — not a registration
     * traded per screen — is what keeps a parked dial out of another screen's
     * frame, exactly the discipline the clock's own registration used. */
    display_set_commit_hook(sched_flush_commit);
#endif
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
    s_floor_seeded = false;         /* the theme's bank class may have changed: re-seed */
    s_demote_run = 0;
    s_promote_run = 0;
    uint32_t bf; uint64_t bt; uint32_t bm;
    display_bench_snapshot(&bf, &bt, &bm);
    s_fps_bench_f0 = bf;
    s_fps_bench_t0 = bt;
}
