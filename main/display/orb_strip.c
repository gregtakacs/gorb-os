/* The carousel — contract in orb_strip.h, design in plan §54. */
#include "orb_strip.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "input_router.h"
#include "knob.h"
#include "orb_app.h"
#include "orb_sched.h"
#include "orb_jobs.h"
#include "orb_sd.h"
#include "orb_theme.h"

static const char *TAG = "strip";

#define W        SCREEN_WIDTH
#define H        SCREEN_HEIGHT
#define CX       (W / 2)
#define CY       (H / 2)
#define PV       232            /* a theme preview's side, and the centre slot's     */
#define PEEK     110.0f         /* a neighbour's diameter                            */
#define SPACING  172.0f         /* centre to a neighbour's centre                    */
#define NAME_W   300
#define NAME_H   40
#define NAME_Y   (CY + PV / 2 + 14)
#define MAX_ITEMS 12
#define STEP_MS   140           /* one place along the strip                         */
#define ZOOM_MS   320           /* the disc zoom in and out                          */
#define IDLE_MS   3000          /* §54: a pause this long opens the centre           */
#define FADE_MS   260           /* the preview fades into the live app               */
#define SETTLE_MS 900           /* the longest the fade waits for the app to draw    */

/* One place on the strip: an app, or the gear (app NULL). The picture is a square in
 * wire order: the theme's preview (PV), the glass as the carousel found it (W, the app
 * it opened on, until the strip moves off it), or the gear. */
typedef struct {
    const orb_app_t *app;
    const uint16_t  *px;
    int              side;
    uint16_t        *name;      /* NAME_W x NAME_H, wire order, black ground */
} item_t;

/* The preview cache: one plane per app slug, filled once by the worker. */
typedef struct {
    char      slug[24];
    uint16_t *px;               /* PV x PV wire order, corners black; NULL = none */
} pv_t;

/* SETTLE: the app is switched in under the held glass and is drawing into the mirror;
 * FADE: the preview fades into what it drew. */
typedef enum { ST_OFF, ST_IN, ST_IDLE, ST_TURN, ST_OUT, ST_SETTLE, ST_FADE } st_t;

static pv_t      s_pv[MAX_ITEMS];
static int       s_pv_n;
static volatile bool s_pv_ready;
static uint16_t *s_gear;        /* PV x PV wire order */
static uint16_t *s_snap;        /* W x H: the glass at open */
static uint16_t *s_names;       /* MAX_ITEMS name planes, back to back */

static item_t    s_it[MAX_ITEMS];
static int       s_n;
static int       s_from_i;      /* the item the carousel opened on */
static st_t      s_st;
static float     s_pos;         /* centre position, unwrapped */
static float     s_pos0;        /* a turn's start */
static int       s_target;      /* a turn's end, unwrapped */
static int64_t   s_t0;          /* the running motion's start */
static int64_t   s_last_us;     /* the last knob event, for the idle open */
static int       s_pending;     /* steps asked for after the IN zoom */
static int       s_open_i;      /* OUT: the item that opens */
static bool      s_dirty;       /* the settled frame was skipped and is still owed */
static uint32_t  s_frames0;     /* SETTLE: the held-frame count at the switch */
static int       s_fade_a;      /* FADE: 0..32 toward the live app */

/* Per frame. */
static float     s_zoom;        /* 0 = strip, 1 = the centre fills the glass */
static float     s_peek_k;      /* neighbours' spread: 1 in place, >1 pushed out */

static inline int wrap(int i) { return ((i % s_n) + s_n) % s_n; }
static inline uint16_t wire(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ---- the gear ------------------------------------------------------------------ */

static void make_gear(void)
{
    s_gear = heap_caps_malloc((size_t)PV * PV * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_gear) return;
    const uint16_t ground = wire(0x2104);     /* #202020 */
    const uint16_t metal  = wire(0xC638);     /* #c0c4c0 */
    const float c = PV / 2.0f - 0.5f;
    for (int y = 0; y < PV; y++)
        for (int x = 0; x < PV; x++) {
            const float dx = x - c, dy = y - c, r = sqrtf(dx * dx + dy * dy);
            uint16_t v = 0;
            if (r <= PV / 2.0f) {
                v = ground;
                const float a = atan2f(dy, dx);
                /* eight teeth: the rim reaches out to 78 px across a tooth's 40 % */
                const float t = fmodf(a / (2.0f * (float)M_PI) * 8.0f + 8.0f, 1.0f);
                const float rim = (t < 0.4f) ? 78.0f : 62.0f;
                if (r <= rim && r >= 26.0f) v = metal;
            }
            s_gear[y * PV + x] = v;
        }
}

/* ---- the preview cache ----------------------------------------------------------- */

static uint16_t *pv_for(const orb_app_t *a)
{
    if (!a || !a->slug) return NULL;
    for (int i = 0; i < s_pv_n; i++)
        if (strcmp(s_pv[i].slug, a->slug) == 0) return s_pv[i].px;
    return NULL;
}

/* On the jobs worker, as the theme picker's preview reads are: the reads ride the card
 * mutex like any picker's, never the LVGL task's. */
static void pv_job(void *arg)
{
    (void)arg;
    if (!orb_sd_mounted()) {
        ESP_LOGW(TAG, "no card: the carousel waits for its previews, the text list stays");
        return;
    }
    const size_t cap = (size_t)PV * PV * 2;
    for (size_t i = 0; i < orb_app_count() && s_pv_n < MAX_ITEMS; i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind == ORB_APP_KIND_SHELL || !a->slug) continue;
        pv_t *p = &s_pv[s_pv_n];
        snprintf(p->slug, sizeof p->slug, "%s", a->slug);
        p->px = NULL;
        char stem[64];
        if (orb_theme_pool_stem(a->slug, stem, sizeof stem)) {
            uint16_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            uint16_t w = 0, h = 0;
            char why[96];
            const int64_t t0 = esp_timer_get_time();
            const orb_asset_ans_t ans = buf
                ? orb_theme_preview_read(stem, (uint8_t *)buf, cap, &w, &h, why, sizeof why)
                : ORB_ASSET_NO_SLOT;
            if (ans == ORB_ASSET_OK && w == PV && h == PV) {
                for (size_t k = 0; k < (size_t)PV * PV; k++) buf[k] = wire(buf[k]);
                p->px = buf;
                ESP_LOGI(TAG, "preview %s (%s): %d ms", a->slug, stem,
                         (int)((esp_timer_get_time() - t0) / 1000));
            } else {
                heap_caps_free(buf);
                ESP_LOGW(TAG, "no preview for %s (%s): answer %d %ux%u", a->slug, stem,
                         (int)ans, w, h);
            }
        }
        s_pv_n++;
    }
    s_pv_ready = true;
}

/* Let the boot settle first: the card mounts and the first dial binds before the cache
 * reads anything. */
static esp_timer_handle_t s_kick;

/* The worker takes one job at a time; one outstanding (a store, a card walk) is asked
 * again two seconds on. */
static void pv_kick(void *arg)
{
    (void)arg;
    if (orb_jobs_post(pv_job, NULL, 0) != ESP_OK)
        esp_timer_start_once(s_kick, 2000 * 1000);
}

void orb_strip_init(void)
{
    make_gear();
    const esp_timer_create_args_t ta = { .callback = pv_kick, .name = "strip_pv" };
    if (esp_timer_create(&ta, &s_kick) != ESP_OK || esp_timer_start_once(s_kick, 8000 * 1000) != ESP_OK)
        ESP_LOGE(TAG, "the preview timer did not start; the text list stays");
}

/* ---- names --------------------------------------------------------------------- */

/* LVGL draws each name once into a canvas that never reaches a screen; the frames copy
 * the plane. On the LVGL task, at open. */
static void make_names(void)
{
    if (!s_names)
        s_names = heap_caps_malloc((size_t)MAX_ITEMS * NAME_W * NAME_H * 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_draw_buf_t *db = lv_draw_buf_create(NAME_W, NAME_H, LV_COLOR_FORMAT_RGB565, 0);
    lv_obj_t *cv = db ? lv_canvas_create(lv_layer_sys()) : NULL;
    for (int i = 0; i < s_n; i++) s_it[i].name = NULL;
    if (!s_names || !cv) {
        if (cv) lv_obj_delete(cv);
        if (db) lv_draw_buf_destroy(db);
        return;
    }
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, db);
    for (int i = 0; i < s_n; i++) {
        const orb_app_t *a = s_it[i].app;
        const char *txt = a ? (a->title ? a->title : a->slug) : "Settings";
        lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);
        lv_layer_t layer;
        lv_canvas_init_layer(cv, &layer);
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.color = lv_color_white();
        d.font  = &lv_font_montserrat_28;
        d.align = LV_TEXT_ALIGN_CENTER;
        d.text  = txt;
        lv_area_t ar = { 0, 4, NAME_W - 1, NAME_H - 1 };
        lv_draw_label(&layer, &d, &ar);
        lv_canvas_finish_layer(cv, &layer);
        uint16_t *dst = s_names + (size_t)i * NAME_W * NAME_H;
        for (int y = 0; y < NAME_H; y++) {
            const uint16_t *src = (const uint16_t *)(db->data + (size_t)y * db->header.stride);
            for (int x = 0; x < NAME_W; x++) dst[y * NAME_W + x] = wire(src[x]);
        }
        s_it[i].name = dst;
    }
    lv_obj_delete(cv);
    lv_draw_buf_destroy(db);
}

/* ---- composing ----------------------------------------------------------------- */

typedef struct {
    const uint16_t *px;
    int   side;
    float cx, cy, d;            /* centre and diameter on the glass */
} disc_t;

static disc_t s_draw[6];
static int    s_draw_n;
static const uint16_t *s_label;  /* the centre's name, or NULL mid-motion */

/* One disc's span of dest row y, nearest-sampled out of its square. */
static void disc_row(uint16_t *d, int y, const disc_t *c)
{
    const float r = c->d * 0.5f, dy = (y + 0.5f) - c->cy;
    if (dy <= -r || dy >= r) return;
    const float hw = sqrtf(r * r - dy * dy);
    int x0 = (int)(c->cx - hw + 0.5f), x1 = (int)(c->cx + hw - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x1 < x0) return;
    const float s = (float)c->side / c->d;              /* source px per dest px */
    int sy = (int)((dy + r) * s);
    if (sy < 0) sy = 0;
    if (sy >= c->side) sy = c->side - 1;
    const uint16_t *row = c->px + (size_t)sy * (size_t)c->side;
    const uint32_t step = (uint32_t)(s * 65536.0f);
    uint32_t fx = (uint32_t)(((x0 + 0.5f) - (c->cx - r)) * s * 65536.0f);
    for (int x = x0; x <= x1; x++, fx += step) {
        uint32_t sx = fx >> 16;
        if (sx >= (uint32_t)c->side) sx = (uint32_t)c->side - 1;
        d[x] = row[sx];
    }
}

static void compose(int y0, int rows, uint16_t *dst, void *arg)
{
    (void)arg;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *d = dst + (size_t)r * W;
        memset(d, 0, (size_t)W * 2);
        for (int i = 0; i < s_draw_n; i++) disc_row(d, y, &s_draw[i]);
        if (s_label && y >= NAME_Y && y < NAME_Y + NAME_H)
            memcpy(d + (CX - NAME_W / 2), s_label + (size_t)(y - NAME_Y) * NAME_W,
                   (size_t)NAME_W * 2);
    }
}

/* Wire-order RGB565 blend, a of 32 toward b (orb_fx.c's, one multiply pair a pixel). */
static inline uint16_t mix_wire(uint16_t aw, uint16_t bw, uint32_t a)
{
    const uint32_t x = __builtin_bswap16(aw), y = __builtin_bswap16(bw);
    const uint32_t A = (x | (x << 16)) & 0x07E0F81Fu;
    const uint32_t B = (y | (y << 16)) & 0x07E0F81Fu;
    const uint32_t R = ((A * (32u - a) + B * a) >> 5) & 0x07E0F81Fu;
    return __builtin_bswap16((uint16_t)(R | (R >> 16)));
}

/* The fade: the full-glass preview (the OUT zoom's last frame) blended toward the
 * mirror, which holds what the opened app has drawn. */
static void compose_fade(int y0, int rows, uint16_t *dst, void *arg)
{
    (void)arg;
    const uint16_t *m = display_fb_mirror();
    const uint32_t a = (uint32_t)s_fade_a;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *d = dst + (size_t)r * W;
        memset(d, 0, (size_t)W * 2);
        for (int i = 0; i < s_draw_n; i++) disc_row(d, y, &s_draw[i]);
        const uint16_t *mr = m + (size_t)y * W;
        for (int x = 0; x < W; x++) d[x] = mix_wire(d[x], mr[x], a);
    }
}

/* Lay the strip out at position pos, zoom z (the centre's growth toward the glass) and
 * neighbour spread k, farthest drawn first so the centre lands on top. */
static void layout(float pos, float z, float k)
{
    s_draw_n = 0;
    const int c = (int)floorf(pos + 0.5f);
    int order[5] = { -2, 2, -1, 1, 0 };
    for (int oi = 0; oi < 5; oi++) {
        const int idx = c + order[oi];
        const float dd = (float)idx - pos;              /* places from the centre */
        const float ad = fabsf(dd);
        if (ad > 1.6f) continue;
        if (s_n == 1 && idx != c) continue;
        const item_t *it = &s_it[wrap(idx)];
        if (!it->px) continue;
        disc_t *q = &s_draw[s_draw_n++];
        const float t = ad > 1.0f ? 1.0f : ad;
        q->d  = PV + (PEEK - PV) * t;
        q->cx = CX + dd * SPACING * k;
        q->cy = CY;
        if (ad < 0.5f && z > 0.0f) q->d = PV + (float)(W - PV) * z;   /* z 1: the glass, 1:1 */
        q->px = it->px;
        q->side = it->side;
    }
    const float frac = fabsf(pos - (float)c);
    s_label = (frac < 0.02f && z <= 0.0f) ? s_it[wrap(c)].name : NULL;
}

/* One open's figures, logged at its close: where a slow turn went. */
static struct {
    int      frames, skipped;
    uint64_t frame_us, compose_us;
    uint32_t frame_max_us, gap_max_us, events;
    int64_t  last_poll_us;
} s_m;

static bool frame_kept(bool keep);
static bool frame(void) { return frame_kept(false); }

static bool frame_kept(bool keep)
{
    display_fx_stats_t st;
    display_fx_shadow_next(keep);
    const esp_err_t e = display_fx_frame_try(compose, NULL, true, &st);
    if (e != ESP_OK) { s_m.skipped++; return false; }
    const uint32_t us = st.frame_us + st.te_us;
    s_m.frames++;
    s_m.frame_us += us;
    s_m.compose_us += st.compose_us;
    if (us > s_m.frame_max_us) s_m.frame_max_us = us;
    return true;
}

static void log_figures(const char *how)
{
    ESP_LOGI(TAG, "%s: %d frames (avg %lu us, compose %lu us, max %lu us), %d skipped (card had the ring), "
             "%lu knob events, longest poll gap %lu ms", how, s_m.frames,
             (unsigned long)(s_m.frames ? s_m.frame_us / (uint64_t)s_m.frames : 0),
             (unsigned long)(s_m.frames ? s_m.compose_us / (uint64_t)s_m.frames : 0),
             (unsigned long)s_m.frame_max_us, s_m.skipped, (unsigned long)s_m.events,
             (unsigned long)(s_m.gap_max_us / 1000));
}

static float ease(float t)
{
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* ---- open, run, close ------------------------------------------------------------ */

bool orb_strip_active(void) { return s_st != ST_OFF; }

bool orb_strip_open(const orb_app_t *from, int step)
{
    if (s_st != ST_OFF || !s_pv_ready || !s_gear) return false;
    const uint16_t *mirror = display_fb_mirror();
    if (!mirror) return false;
    if (!s_snap) s_snap = heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_snap) return false;

    s_n = 0;
    s_from_i = 0;
    for (size_t i = 0; i < orb_app_count() && s_n < MAX_ITEMS - 1; i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind == ORB_APP_KIND_SHELL) continue;
        item_t *it = &s_it[s_n];
        it->app = a;
        it->px = pv_for(a);
        it->side = PV;
        if (!it->px) { it->px = s_gear; }      /* an app with no preview: a plain disc */
        if (a == from) s_from_i = s_n;
        s_n++;
    }
    s_it[s_n] = (item_t){ .app = NULL, .px = s_gear, .side = PV };
    if (from == NULL || from->kind == ORB_APP_KIND_SHELL) s_from_i = s_n;
    s_n++;

    /* The app the carousel opens on wears the glass itself until the strip moves off
     * it, so the zoom down starts from exactly what was showing. */
    memcpy(s_snap, mirror, (size_t)W * H * 2);
    s_it[s_from_i].px = s_snap;
    s_it[s_from_i].side = W;
    make_names();

    display_glass_hold(true);
    orb_sched_set_running(false);
    s_pos = (float)s_from_i;
    s_pending = step;
    memset(&s_m, 0, sizeof s_m);
    s_st = ST_IN;
    s_t0 = esp_timer_get_time();
    s_last_us = s_t0;
    ESP_LOGI(TAG, "open on %s, %d places", from && from->slug ? from->slug : "?", s_n);
    return true;
}

/* The zoom out has landed: switch the app in under the still-held glass, and wait for
 * it to draw before fading to it (step_once's SETTLE and FADE). */
static void finish(int64_t now)
{
    const item_t *it = &s_it[wrap(s_open_i)];
    const bool same = wrap(s_open_i) == s_from_i;
    s_frames0 = display_held_frames();
    if (same && it->app) {
        orb_sched_set_running(true);           /* the app never left the screen */
    } else if (it->app) {
        input_router_show(it->app);
    } else {
        input_router_open_settings();
    }
    ESP_LOGI(TAG, "opened %s", it->app && it->app->slug ? it->app->slug : "settings");
    s_st = ST_SETTLE;
    s_t0 = now;
}

/* Hand the glass back: the panel already shows the mirror (the fade's last frame was
 * all app), so the release's full re-send changes nothing anyone can see. */
static void release(void)
{
    s_st = ST_OFF;
    display_glass_hold(false);
    log_figures("closed");
}

void orb_strip_abort(void)
{
    if (s_st == ST_OFF) return;
    s_st = ST_OFF;
    orb_sched_set_running(true);
    display_glass_hold(false);
}

static void start_out(int64_t now)
{
    s_open_i = (int)floorf(s_pos + 0.5f);
    s_pos = (float)s_open_i;
    s_st = ST_OUT;
    s_t0 = now;
}

static void start_turn(int steps, int64_t now)
{
    if (!steps) return;
    if (s_st == ST_TURN) {
        s_pos0 = s_pos;                         /* keep moving: retarget from here */
        s_target += steps;
    } else {
        s_pos0 = s_pos;
        s_target = (int)floorf(s_pos + 0.5f) + steps;
    }
    /* Once the strip leaves the app it opened on, that app wears its preview again. */
    item_t *f = &s_it[s_from_i];
    if (f->px == s_snap && f->app) {
        const uint16_t *p = pv_for(f->app);
        f->px = p ? p : s_gear;
        f->side = PV;
    }
    s_st = ST_TURN;
    s_t0 = now;
}

static void step_once(int32_t delta, bool pressed, bool lpress, int64_t now)
{
    if (s_st == ST_OFF) return;
    if (delta || pressed || lpress) { s_last_us = now; s_m.events++; }

    switch (s_st) {
    case ST_IN: {
        s_pending += knob_col_step(delta) * (delta < 0 ? -delta : delta);
        const float t = (float)(now - s_t0) / (ZOOM_MS * 1000.0f);
        const float p = ease(t);
        /* the glass shrinks from full to the centre slot; the neighbours slide in */
        layout(s_pos, 1.0f - p, 1.0f + (1.0f - p) * 1.4f);
        frame();
        if (t >= 1.0f) {
            layout(s_pos, 0.0f, 1.0f);
            s_dirty = !frame_kept(true);
            s_st = ST_IDLE;
            s_last_us = now;
            if (s_pending) { start_turn(s_pending, now); s_pending = 0; }
        }
        return;
    }
    case ST_IDLE:
    case ST_TURN:
        if (lpress) {                           /* a hold leaves where it came from */
            s_pos = (float)s_from_i;
            if (s_it[s_from_i].app) {
                s_it[s_from_i].px = s_snap; s_it[s_from_i].side = W;
            }
            start_out(now);
            break;
        }
        if (pressed) { start_out(now); break; }
        if (delta) start_turn(knob_col_step(delta) * (delta < 0 ? -delta : delta), now);
        if (s_st == ST_TURN) {
            const int n = s_target - (int)floorf(s_pos0 + 0.5f);
            const float span = (float)(n < 0 ? -n : n);
            const float t = (float)(now - s_t0) / (STEP_MS * 1000.0f * (span > 1 ? 0.6f + 0.4f * span : 1.0f));
            const float p = ease(t);
            s_pos = s_pos0 + ((float)s_target - s_pos0) * p;
            if (t >= 1.0f) {
                s_pos = (float)s_target;
                s_st = ST_IDLE;
            }
            layout(s_pos, 0.0f, 1.0f);
            s_dirty = !frame_kept(s_st == ST_IDLE) && s_st == ST_IDLE;   /* a skipped last frame is owed */
            return;
        }
        if (s_dirty) { layout(s_pos, 0.0f, 1.0f); s_dirty = !frame_kept(true); }
        if (now - s_last_us >= (int64_t)IDLE_MS * 1000) start_out(now);
        return;
    default:
        break;
    }

    if (s_st == ST_OUT) {
        const float t = (float)(now - s_t0) / (ZOOM_MS * 1000.0f);
        const float p = ease(t);
        layout(s_pos, p, 1.0f + p * 1.4f);
        const bool frame_drawn_last = frame();
        if (t >= 1.0f && (frame_drawn_last || now - s_t0 > 2000000)) finish(now);
        return;
    }

    /* A touch while the app is fading in is the app's: hand the glass back now and pass
     * the event on through the router, rather than spend it on a fade. */
    if (s_st == ST_SETTLE || s_st == ST_FADE) {
        if (delta || pressed || lpress) {
            release();
            input_router_inject(delta, pressed, lpress);
            return;
        }
    }
    if (s_st == ST_SETTLE) {
        /* Two frames drawn is a moving dial settled; one frame and 150 ms is a still
         * page (the settings menu draws once) — waiting it out cost 0.9 s. */
        const uint32_t drew = display_held_frames() - s_frames0;
        if ((drew >= 2 && now - s_t0 >= 120000) || (drew >= 1 && now - s_t0 >= 150000) ||
            now - s_t0 >= (int64_t)SETTLE_MS * 1000) {
            s_st = ST_FADE;
            s_t0 = now;
        }
        return;
    }
    if (s_st == ST_FADE) {
        const float t = (float)(now - s_t0) / (FADE_MS * 1000.0f);
        s_fade_a = t >= 1.0f ? 32 : (int)(t * 32.0f + 0.5f);
        display_fx_stats_t st;
        display_fx_shadow_next(t >= 1.0f);
        const bool drawn = display_fx_frame_try(compose_fade, NULL, true, &st) == ESP_OK;
        if (drawn) { s_m.frames++; s_m.frame_us += st.frame_us + st.te_us; s_m.compose_us += st.compose_us; }
        else s_m.skipped++;
        if (t >= 1.0f && (drawn || now - s_t0 > 1000000)) release();
    }
}

/* While something moves, frames go back to back for up to 200 ms of this poll, and the
 * knob is read between them so a turn mid-motion retargets at once. One frame a poll
 * paid a refresh of slack per frame (28 ms against the bench's 17, Bronze 2026-11-07). */
void orb_strip_poll(int32_t delta, bool pressed, bool lpress, int64_t now)
{
    if (s_st == ST_OFF) return;
    if (s_m.last_poll_us && now - s_m.last_poll_us > (int64_t)s_m.gap_max_us)
        s_m.gap_max_us = (uint32_t)(now - s_m.last_poll_us);
    s_m.last_poll_us = now;
    step_once(delta, pressed, lpress, now);
    const int64_t until = now + 200000;
    while ((s_st == ST_IN || s_st == ST_TURN || s_st == ST_OUT) && esp_timer_get_time() < until) {
        /* SETTLE needs LVGL to run and draw the app, so the loop stops at it; the FADE
         * runs one frame a poll, so the app keeps drawing under it. */
        knob_poll();
        const int32_t d = knob_take_delta();
        const bool    p = knob_take_press();
        const bool    l = knob_take_long_press();
        step_once(d, p, l, esp_timer_get_time());
    }
    if (s_st != ST_OFF) s_m.last_poll_us = esp_timer_get_time();
}
