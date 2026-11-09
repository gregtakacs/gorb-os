/* The carousel — contract in orb_strip.h, design in plan §54. */
#include "orb_strip.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
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
#include "orb_ui_icons.h"   /* §61: the drawn Settings card */
#include "orb_app_wasm.h"   /* §61: an app's own icon */

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
#define FACE      300           /* theme-cache-plan ruling 2: the fill's face, a preview */
#define RING_GAP  6             /* the progress ring, this far outside the face...       */
#define RING_W    10            /* ...and this wide                                       */
#define RING_STEP 20            /* permille between two redraws: each costs the fill one */

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
    uint16_t *px;               /* side x side wire order, corners black; NULL = none */
    uint8_t  *z;                /* §60.2: the full preview as stored (deflated disc), kept so
                                 * the centre's full plane never needs the card again */
    uint32_t  zn;
    uint16_t  zside;
    int       side;             /* the theme's own preview size: 232 for every shipped
                                 * app's (Weather's was 128 before v1.4.1) — the disc
                                 * scales any square up to PV */
} pv_t;

/* SETTLE: the app is switched in under the held glass and is drawing into the mirror;
 * FADE: the preview fades into what it drew. FILL: theme-cache-plan §2.4, the app's theme
 * is coming back from the card while the app runs bare under the held glass: its preview at
 * FACE with the progress ring round it; GROW: the fill is over, the face zooms to the glass
 * and SETTLE and FADE follow as for any open. */
typedef enum { ST_OFF, ST_IN, ST_IDLE, ST_TURN, ST_OUT, ST_SETTLE, ST_FADE, ST_FILL, ST_GROW } st_t;

static pv_t      s_pv[MAX_ITEMS];
static int       s_pv_n;
static volatile bool s_pv_ready;
/* §60: one full-glass plane, the centre app's, inflated on the preview task once a turn
 * settles, so the zoom into the app is drawn 1:1 rather than from the 232 cache magnified.
 * §60.2: inflated from the app's kept bytes, never read off the card: a card read lends the
 * display's DMA ring for its whole length, and every frame of the carousel dropped while one
 * ran (Gold, 2026-11-07: 6,856 of 6,856 skips with the ring out, three 0.6 s reads a run). The
 * task clears ready before it writes, and the strip uses the plane only while ready and while
 * it names the app that opens. */
static uint16_t *s_hi;
static char      s_hi_slug[24];
static volatile bool s_hi_ready;
static volatile bool s_hi_drop;     /* the strip closed: the preview task frees the plane */
static char      s_hi_want[24];
static TaskHandle_t s_pv_th;
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
static float     s_out_z;       /* OUT: the zoom's end, 1 = the glass, less = the fill's face */
static float     s_in_z;        /* IN: the zoom's start, likewise (opened over the face) */
#define FACE_Z   ((float)(FACE - PV) / (float)(W - PV))   /* the face, as a zoom */
static const orb_app_t *s_face_app;   /* FILL, GROW: the app whose theme is filling */
static int       s_ring = -1;   /* the ring as drawn, permille; -1 = no ring */

/* Per frame. */
static float     s_zoom;        /* 0 = strip, 1 = the centre fills the glass */
static float     s_peek_k;      /* neighbours' spread: 1 in place, >1 pushed out */

static inline int wrap(int i) { return ((i % s_n) + s_n) % s_n; }
static inline uint16_t wire(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ---- the gear ------------------------------------------------------------------ */

static void box_down(const uint16_t *src, int sw, uint16_t *dst, int dw);

/* §61: a deflated disc (a drawn card, or an app's icon) as a PV square in wire order, or
 * NULL. Inflated at its own side, then boxed down to the strip's. */
static uint16_t *card_plane(const uint8_t *z, uint32_t n, uint16_t side)
{
    if (!z || !n || !side || side > W) return NULL;
    uint16_t *full = (uint16_t *)orb_preview_inflate(z, n, side);
    if (!full) return NULL;
    uint16_t *out = full;
    if (side != PV) {
        out = heap_caps_malloc((size_t)PV * PV * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out) box_down(full, side, out, PV);
        heap_caps_free(full);
        if (!out) return NULL;
    }
    for (int k = 0; k < PV * PV; k++) out[k] = wire(out[k]);
    return out;
}

static void make_gear(void)
{
    /* §61: the drawn Settings card the firmware carries; the gear below if it will not
     * inflate. */
    s_gear = card_plane(orb_ui_icon_settings_z, orb_ui_icon_settings_n, orb_ui_icon_settings_side);
    if (s_gear) return;
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

/* §60: a plane of side sw (RGB565, native order) down to side dw by box averaging, the
 * strip's cache size from a full-glass preview. */
static void box_down(const uint16_t *src, int sw, uint16_t *dst, int dw)
{
    for (int y = 0; y < dw; y++) {
        const int y0 = y * sw / dw, y1 = ((y + 1) * sw / dw > y0 + 1) ? (y + 1) * sw / dw : y0 + 1;
        for (int x = 0; x < dw; x++) {
            const int x0 = x * sw / dw, x1 = ((x + 1) * sw / dw > x0 + 1) ? (x + 1) * sw / dw : x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < sw; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const uint16_t c = src[(size_t)yy * sw + xx];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++;
                }
            dst[(size_t)y * dw + x] = n ? (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n)) : 0;
        }
    }
}

/* One theme's preview, read at its own size into a fresh buffer (wire order is the
 * caller's to make). NULL when it has none or the read fails. */
static uint16_t *pv_read(const char *stem, uint16_t *w_out, const char *slug)
{
    uint16_t w = 0, h = 0;
    char why[96];
    if (orb_theme_preview_read(stem, NULL, 0, &w, &h, why, sizeof why) != ORB_ASSET_OK ||
        w != h || w == 0 || w > W) {
        ESP_LOGW(TAG, "no preview for %s (%s): %ux%u %s", slug, stem, w, h, why);
        return NULL;
    }
    const size_t cap = (size_t)w * h * 2;
    uint16_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return NULL;
    if (orb_theme_preview_read(stem, (uint8_t *)buf, cap, &w, &h, why, sizeof why) != ORB_ASSET_OK) {
        ESP_LOGW(TAG, "preview %s (%s) did not read: %s", slug, stem, why);
        heap_caps_free(buf);
        return NULL;
    }
    *w_out = w;
    return buf;
}

/* §61: an app with no theme to show wears the icon it handed over (orb.icon), drawn
 * once per boot on the first open that sees it. */
static struct { const orb_app_t *app; uint16_t *px; } s_icon[MAX_ITEMS];
static const uint16_t *icon_for(const orb_app_t *a)
{
    const uint8_t *z = NULL;
    uint32_t n = 0;
    uint16_t side = 0;
    int free_i = -1;
    for (int i = 0; i < MAX_ITEMS; i++) {
        if (s_icon[i].app == a && s_icon[i].px) return s_icon[i].px;
        if (!s_icon[i].px && free_i < 0) free_i = i;
    }
    if (free_i < 0 || !orb_wasm_icon(a, &z, &n, &side)) return NULL;
    s_icon[free_i].px = card_plane(z, n, side);
    s_icon[free_i].app = s_icon[free_i].px ? a : NULL;
    return s_icon[free_i].px;
}

static const pv_t *pv_for(const orb_app_t *a)
{
    if (!a || !a->slug) return NULL;
    for (int i = 0; i < s_pv_n; i++)
        if (strcmp(s_pv[i].slug, a->slug) == 0 && s_pv[i].px) return &s_pv[i];
    return NULL;
}

/* The reads ride the card mutex like any picker's, never the LVGL task's. On a task of
 * their own, not the jobs worker: the worker takes one job at a time, and a theme store
 * posted while this read held it was refused with ESP_ERR_INVALID_STATE — the hub's
 * Weather install stores Scope right after the orb's reboot, exactly when this ran
 * (owner, 2026-11-07). Each read also waits out a store or an upload in flight. */
static void pv_load(void)
{
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
        p->px = NULL; p->z = NULL; p->zn = 0; p->zside = 0;
        char stem[64];
        while (orb_jobs_busy() || orb_theme_upload_busy()) vTaskDelay(pdMS_TO_TICKS(500));
        if (orb_theme_pool_stem(a->slug, stem, sizeof stem)) {
            const int64_t t0 = esp_timer_get_time();
            uint16_t w = 0;
            uint16_t *buf = NULL;
            char why[96];
            /* §60.2: the stored bytes first, kept; the cache plane is derived from them */
            p->z = orb_theme_preview_zcopy(stem, &p->zn, &p->zside, why, sizeof why);
            if (p->z) {
                buf = (uint16_t *)orb_preview_inflate(p->z, p->zn, p->zside);
                w = p->zside;
            } else {
                buf = pv_read(stem, &w, a->slug);         /* a raw preview, an older bake */
            }
            if (buf && w > PV) {                 /* §60: a full-glass preview, boxed down */
                uint16_t *sm = heap_caps_malloc((size_t)PV * PV * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (sm) box_down(buf, w, sm, PV);
                heap_caps_free(buf);
                buf = sm;
                w = PV;
            }
            if (buf) {
                for (size_t k = 0; k < (size_t)w * w; k++) buf[k] = wire(buf[k]);
                p->px = buf;
                p->side = w;
                ESP_LOGI(TAG, "preview %s (%s): %d ms, %lu B kept", a->slug, stem,
                         (int)((esp_timer_get_time() - t0) / 1000), (unsigned long)p->zn);
            }
        }
        s_pv_n++;
    }
    s_pv_ready = true;
}

/* Let the boot settle first: the card mounts and the first dial binds before the cache
 * reads anything. */
/* Let the boot settle first: the card mounts and the first dial binds before the cache
 * reads anything. The task parks once the cache is filled (8 KB of PSRAM stack; it
 * touches only the card and the heap, never flash). */
static void pv_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    pv_load();
    /* §60: then the centre app's full-glass plane, each time the strip asks */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_hi_drop) {
            s_hi_drop = false;
            uint16_t *old = s_hi;
            s_hi = NULL;
            s_hi_slug[0] = '\0';
            heap_caps_free(old);
        }
        char want[sizeof s_hi_want];
        memcpy(want, s_hi_want, sizeof want);
        want[sizeof want - 1] = '\0';
        if (!want[0] || (s_hi_ready && strcmp(want, s_hi_slug) == 0)) continue;
        const pv_t *src = NULL;
        for (int i = 0; i < s_pv_n; i++) if (!strcmp(s_pv[i].slug, want)) src = &s_pv[i];
        if (!src || !src->z || src->zside != W) continue;  /* no kept bytes: the cache is it */
        s_hi_ready = false;
        const int64_t t0 = esp_timer_get_time();
        uint16_t *buf = (uint16_t *)orb_preview_inflate(src->z, src->zn, src->zside);
        if (!buf) continue;
        for (size_t k = 0; k < (size_t)W * W; k++) buf[k] = wire(buf[k]);
        uint16_t *old = s_hi;
        s_hi = buf;
        memcpy(s_hi_slug, want, sizeof s_hi_slug);
        s_hi_ready = true;
        if (old) heap_caps_free(old);
        ESP_LOGI(TAG, "full preview %s: %d ms (inflated, no card)", want,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
}

/* Ask for the centre app's full-glass plane (LVGL task). */
static void hi_want(const orb_app_t *a)
{
    if (!a || !a->slug || !s_pv_th) return;
    if (s_hi_ready && strcmp(a->slug, s_hi_slug) == 0) return;
    snprintf(s_hi_want, sizeof s_hi_want, "%s", a->slug);
    xTaskNotifyGive(s_pv_th);
}

void orb_strip_init(void)
{
    make_gear();
    if (xTaskCreatePinnedToCoreWithCaps(pv_task, "strip_pv", 8192, NULL, 2, &s_pv_th,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS)
        ESP_LOGE(TAG, "the preview task did not start; the text list stays");
}

/* ---- names --------------------------------------------------------------------- */

/* LVGL draws each name once into a canvas that never reaches a screen; the frames copy
 * the plane. On the LVGL task, at open. */
static void make_names(void)
{
    heap_caps_free(s_names);            /* this open's places only, freed at close too */
    s_names = heap_caps_malloc((size_t)s_n * NAME_W * NAME_H * 2,
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

/* theme-cache-plan ruling 2: the fill's progress ring, theme_strip's (§61) — a bright arc
 * over a dim track, clockwise from twelve — just outside the face. */
static void ring_row(uint16_t *d, int y)
{
    const float r0 = FACE * 0.5f + RING_GAP, r1 = r0 + RING_W, dy = (y + 0.5f) - CY;
    if (dy <= -r1 || dy >= r1) return;
    const uint16_t track = wire(0x3186), arc = wire(0x66DF);
    const float lim = (float)s_ring * (2.0f * (float)M_PI) / 1000.0f;
    /* the band alone: from the outer edge in to the inner one, either side (one span
     * through the middle rows that miss the inner circle) */
    const float ho = sqrtf(r1 * r1 - dy * dy);
    const float hi = fabsf(dy) < r0 ? sqrtf(r0 * r0 - dy * dy) : 0.0f;
    const int spans[2][2] = { { (int)(CX - ho), (int)(CX - hi) }, { (int)(CX + hi), (int)(CX + ho) } };
    for (int k = 0; k < 2; k++) {
        int x0 = spans[k][0], x1 = spans[k][1];
        if (k == 1 && hi == 0.0f) break;                 /* one span covered the row */
        if (k == 0 && hi == 0.0f) x1 = spans[1][1];
        if (x0 < 0) x0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        for (int x = x0; x <= x1; x++) {
            const float dx = (x + 0.5f) - CX, dist2 = dx * dx + dy * dy;
            if (dist2 < r0 * r0 || dist2 > r1 * r1) continue;
            float a = atan2f(dx, -dy);
            if (a < 0) a += 2.0f * (float)M_PI;
            d[x] = a <= lim ? arc : track;
        }
    }
}

/* The face's disc, d across: the app's full plane when it is in, else its cached preview,
 * else a plain card (a fill at boot, before the cache has read anything). */
static void face_disc(float d)
{
    const orb_app_t *a = s_face_app;
    disc_t *q = &s_draw[0];
    s_draw_n = 1;
    *q = (disc_t){ .px = NULL, .side = PV, .cx = CX, .cy = CY, .d = d };
    if (a && a->slug && s_hi_ready && s_hi && strcmp(a->slug, s_hi_slug) == 0) {
        q->px = s_hi; q->side = W;
    } else {
        const pv_t *p = pv_for(a);
        if (p) { q->px = p->px; q->side = p->side; }
    }
    if (!q->px) s_draw_n = 0;                    /* the ring alone, over black */
    s_label = NULL;
}

/* theme-cache-plan §2.4: a fill holds the card, and the card lock carries the display's DMA
 * ring, so while the strip has the glass it tells the fill so — the fill lets the card go after
 * each chunk and waits while a frame is owed — and takes the ring back at every return rather
 * than after the network's window (Bronze 2026-11-08: 12 face frames in a 9 s fill without;
 * the carousel opened over a running fill drew nothing). Every poll, from orb_strip_poll. */
static void glass_tell(void)
{
    const bool up = s_st != ST_OFF;
    const bool owed = up && (s_dirty || s_st == ST_IN || s_st == ST_TURN ||
                             s_st == ST_OUT || s_st == ST_GROW || s_st == ST_FADE);
    display_ring_urgent_for(DISPLAY_URGENT_FILL, up);
    orb_theme_fill_glass(up, owed);
}

static void compose(int y0, int rows, uint16_t *dst, void *arg)
{
    (void)arg;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *d = dst + (size_t)r * W;
        memset(d, 0, (size_t)W * 2);
        for (int i = 0; i < s_draw_n; i++) disc_row(d, y, &s_draw[i]);
        if (s_ring >= 0) ring_row(d, y);
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
        /* §60: the centre growing toward the glass draws from its full plane once it is in */
        if (ad < 0.5f && z > 0.0f && it->px != s_snap && it->app && s_hi_ready && s_hi &&
            strcmp(it->app->slug, s_hi_slug) == 0) {
            q->px = s_hi;
            q->side = W;
        }
    }
    const float frac = fabsf(pos - (float)c);
    s_label = (frac < 0.02f && z <= 0.0f) ? s_it[wrap(c)].name : NULL;
}

/* One open's figures, logged at its close: where a slow turn went. */
static struct {
    int      frames, skipped, card_skipped;
    uint64_t frame_us, compose_us;
    uint32_t frame_max_us, gap_max_us, events;
    int64_t  last_poll_us;
} s_m;

/* The glass as the carousel found it is spent once it opens an app: freed, not kept for the
 * next open (one malloc then). Kept, its 434 KB of PSRAM starved the opened app — a fill
 * landing under the face found no room for the app's layer table (Bronze 2026-11-08: 15 of
 * 15 rows refused). */
static void snap_free(void)
{
    if (!s_snap) return;
    for (int i = 0; i < s_n; i++)
        if (s_it[i].px == s_snap) { s_it[i].px = NULL; }
    heap_caps_free(s_snap);
    s_snap = NULL;
}

/* At close the carousel gives back what only it draws: its name planes (made anew at each
 * open) and the centre app's full plane (the preview task inflates it again, ~110 ms, on the
 * next settle). Kept, they held ~720 KB of PSRAM under whatever app was open. The plane is
 * the preview task's to free (it may be writing it): asked here, freed there. */
static void strip_give_back(void)
{
    snap_free();
    for (int i = 0; i < MAX_ITEMS; i++) s_it[i].name = NULL;
    heap_caps_free(s_names);
    s_names = NULL;
    s_hi_ready = false;
    s_hi_want[0] = '\0';                /* or the wake would inflate the last one again */
    s_hi_drop = true;
    if (s_pv_th) xTaskNotifyGive(s_pv_th);
}

static bool frame_kept(bool keep);
static bool frame(void) { return frame_kept(false); }

static bool frame_kept(bool keep)
{
    display_fx_stats_t st;
    display_fx_shadow_next(keep);
    const esp_err_t e = display_fx_frame_try(compose, NULL, true, &st);
    if (e != ESP_OK) { s_m.skipped++; if (display_dma_parked()) s_m.card_skipped++; return false; }
    const uint32_t us = st.frame_us + st.te_us;
    s_m.frames++;
    s_m.frame_us += us;
    s_m.compose_us += st.compose_us;
    if (us > s_m.frame_max_us) s_m.frame_max_us = us;
    return true;
}

static void log_figures(const char *how)
{
    ESP_LOGI(TAG, "%s: %d frames (avg %lu us, compose %lu us, max %lu us), %d skipped (%d with the ring "
             "out to the card), %lu knob events, longest poll gap %lu ms", how, s_m.frames,
             (unsigned long)(s_m.frames ? s_m.frame_us / (uint64_t)s_m.frames : 0),
             (unsigned long)(s_m.frames ? s_m.compose_us / (uint64_t)s_m.frames : 0),
             (unsigned long)s_m.frame_max_us, s_m.skipped, s_m.card_skipped, (unsigned long)s_m.events,
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
    uint32_t fd, ft;
    const bool from_fill = from && from->slug && orb_theme_fill_progress(from->slug, &fd, &ft);
    const uint16_t *mirror = display_fb_mirror();
    if (!mirror) return false;
    /* The glass as found, for the zoom down from it. Optional: with radar on the glass PSRAM
     * held 295 KB, the snapshot's 434 KB would not come, and the carousel fell back to the
     * text list (owner's bench, 2026-11-09); without it the zoom starts from the preview. */
    if (!s_snap) s_snap = heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    s_n = 0;
    s_from_i = 0;
    for (size_t i = 0; i < orb_app_count() && s_n < MAX_ITEMS - 1; i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind == ORB_APP_KIND_SHELL) continue;
        item_t *it = &s_it[s_n];
        it->app = a;
        const pv_t *pv = pv_for(a);
        const uint16_t *ic = pv ? NULL : icon_for(a);   /* §61: no theme: its own icon */
        it->px = pv ? pv->px : (ic ? ic : s_gear);
        it->side = pv ? pv->side : PV;
        if (a == from) s_from_i = s_n;
        s_n++;
    }
    s_it[s_n] = (item_t){ .app = NULL, .px = s_gear, .side = PV };
    if (from == NULL || from->kind == ORB_APP_KIND_SHELL) s_from_i = s_n;
    s_n++;

    /* The app the carousel opens on wears the glass itself until the strip moves off
     * it, so the zoom down starts from exactly what was showing. */
    if (s_snap) memcpy(s_snap, mirror, (size_t)W * H * 2);
    /* §61: Settings wears its own card, never a picture of the menu it was opened over: a
     * snapshot only comes off an app's card when the strip moves (start_turn), so the
     * Settings card kept the black MENU landing for the whole visit (owner, 2026-11-08). */
    if (s_it[s_from_i].app && !from_fill && s_snap) {
        s_it[s_from_i].px = s_snap;
        s_it[s_from_i].side = W;
    }
    /* theme-cache-plan §2.4: opened over the fill's face, the zoom down starts from the face
     * (its preview, not the bare app under it) */
    s_in_z = from_fill ? FACE_Z : 1.0f;
    s_out_z = 1.0f;
    s_face_app = NULL;
    s_ring = -1;
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
    s_t0 = now;
    s_st = ST_SETTLE;
    if (s_out_z < 1.0f) {
        /* theme-cache-plan §2.4: the zoom stopped at the face; the open posted the fill (the
         * face waits on it), or did not after all (it grows into the app at once) */
        uint32_t d, t;
        s_dirty = true;
        s_st = orb_theme_fill_progress(it->app->slug, &d, &t) ? ST_FILL : ST_GROW;
        snap_free();                           /* the face draws from the previews alone */
    }
}

void orb_strip_fill_face(const orb_app_t *app)
{
    uint32_t d, t;
    if (s_st != ST_OFF || !app || !app->slug || !orb_theme_fill_progress(app->slug, &d, &t)) return;
    s_face_app = app;
    s_n = 0;
    s_ring = -1;
    s_dirty = true;
    hi_want(app);
    memset(&s_m, 0, sizeof s_m);
    display_glass_hold(true);
    s_st = ST_FILL;
    s_t0 = esp_timer_get_time();
    ESP_LOGI(TAG, "fill face for %s", app->slug);
}

/* Hand the glass back: the panel already shows the mirror (the fade's last frame was
 * all app), so the release's full re-send changes nothing anyone can see. */
static void release(void)
{
    strip_give_back();
    s_st = ST_OFF;
    s_face_app = NULL;
    s_ring = -1;
    display_glass_hold(false);
    log_figures("closed");
}

void orb_strip_abort(void)
{
    if (s_st == ST_OFF) return;
    strip_give_back();
    s_st = ST_OFF;
    s_face_app = NULL;
    s_ring = -1;
    glass_tell();
    orb_sched_set_running(true);
    display_glass_hold(false);
}

static void start_out(int64_t now)
{
    s_open_i = (int)floorf(s_pos + 0.5f);
    s_pos = (float)s_open_i;
    /* theme-cache-plan ruling 2: an app whose theme is on the card only opens onto its fill's
     * face, so the zoom stops there */
    const orb_app_t *a = s_it[wrap(s_open_i)].app;
    uint32_t fd, ft;
    s_face_app = a && a->slug && (orb_theme_fill_wanted(a->slug) ||
                                  orb_theme_fill_progress(a->slug, &fd, &ft)) ? a : NULL;
    s_out_z = s_face_app ? FACE_Z : 1.0f;
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
        const pv_t *p = pv_for(f->app);
        const uint16_t *ic = p ? NULL : icon_for(f->app);
        f->px = p ? p->px : (ic ? ic : s_gear);
        f->side = p ? p->side : PV;
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
        layout(s_pos, s_in_z * (1.0f - p), 1.0f + (1.0f - p) * 1.4f);
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
            if (s_it[s_from_i].app && s_snap) {
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
                hi_want(s_it[wrap(s_target)].app);   /* §60: fetch its full plane for the zoom */
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
        layout(s_pos, p * s_out_z, 1.0f + p * 1.4f);
        const bool frame_drawn_last = frame();
        if (t >= 1.0f && (frame_drawn_last || now - s_t0 > 2000000)) finish(now);
        return;
    }

    if (s_st == ST_FILL) {
        /* the app is not ready for the knob: a hold leaves (the carousel, over the face);
         * a turn or a press is spent */
        if (lpress) {
            const orb_app_t *a = s_face_app;
            s_st = ST_OFF;
            s_ring = -1;
                    if (!orb_strip_open(a, 0)) { display_glass_hold(false); input_router_inject(0, false, true); }
            return;
        }
        uint32_t d = 0, t = 0;
        if (!orb_theme_fill_progress(s_face_app->slug, &d, &t)) {
            ESP_LOGI(TAG, "fill face for %s: %u ms", s_face_app->slug, (unsigned)((now - s_t0) / 1000));
            s_st = ST_GROW;                     /* landed, or failed: the app either way */
                    s_t0 = now;
            return;
        }
        const int pm = t ? (int)((uint64_t)d * 1000u / t) : 0;
        if (s_dirty || s_ring < 0 || pm - s_ring >= RING_STEP) {
            const int was = s_ring;
            s_ring = pm;
            face_disc(FACE);
            s_dirty = !frame_kept(true);         /* still between redraws: a bench grab sees it */
            if (s_dirty) s_ring = was < 0 ? 0 : was;
        }
        return;
    }
    if (s_st == ST_GROW) {
        const float t = (float)(now - s_t0) / (ZOOM_MS * 1000.0f);
        s_ring = -1;
        face_disc(FACE + (float)(W - FACE) * ease(t));
        const bool drawn = frame();
        if (t >= 1.0f && (drawn || now - s_t0 > 2000000)) {
            s_frames0 = display_held_frames();
            s_st = ST_SETTLE;
            s_t0 = now;
        }
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
    while ((s_st == ST_IN || s_st == ST_TURN || s_st == ST_OUT || s_st == ST_GROW) &&
           esp_timer_get_time() < until) {
        /* SETTLE needs LVGL to run and draw the app, so the loop stops at it; the FADE
         * runs one frame a poll, so the app keeps drawing under it. */
        knob_poll();
        const int32_t d = knob_take_delta();
        const bool    p = knob_take_press();
        const bool    l = knob_take_long_press();
        (void)knob_take_push_delta();            /* the carousel has no press-and-turn */
        step_once(d, p, l, esp_timer_get_time());
    }
    if (s_st != ST_OFF) s_m.last_poll_us = esp_timer_get_time();
    glass_tell();
}
