/* The theme carousel — contract in theme_strip.h, design in app-platform-plan §61. */
#include "theme_strip.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "knob.h"
#include "menu_view.h"
#include "input_router.h"   /* §61: a press on the worn theme goes back to the app */
#include "orb_app.h"
#include "orb_sd.h"
#include "orb_theme.h"
#include "themes_page.h"
#include "orb_ui_icons.h"   /* §61: the drawn Back card */
#include "orb_disc.h"       /* §61: a preview streams in as its disc */
#include "orb_inflate.h"

static const char *TAG = "theme_strip";

#define W        SCREEN_WIDTH
#define H        SCREEN_HEIGHT
#define CX       (W / 2)
#define CY       (H / 2)
#define D_C      300            /* the centre disc, and its sharp plane's side          */
#define D_N      150.0f         /* a neighbour's diameter                               */
#define SP       228.0f         /* centre to a neighbour's centre, down the glass       */
#define TH       150            /* a lazy thumbnail's side: a neighbour's, 1:1          */
#define NAME_W   300
#define NAME_H   36
#define NAME_Y   (CY + D_C / 2 - 70)   /* the name band, across the centre disc's foot */
#define MAX_ITEMS 65            /* Back and up to 64 themes (the page's row ceiling)    */
#define NAMES    8              /* name planes kept, most recently drawn               */
#define STEP_MS  130            /* one place                                           */
#define REST_MS  150            /* the strip rests this long before the card is read   */
#define KEEP     6              /* thumbnails kept this many places either side         */
#define TRIES    3              /* a read that failed for memory is tried this often    */

typedef enum { LD_NONE, LD_POSTED, LD_THUMB, LD_NONE_TO_SHOW } ld_t;

typedef struct {
    char      id[ORB_THEME_HANDLE_MAX + 1];
    char      name[48];
    bool      active, ok;
    uint16_t *thumb;            /* TH x TH wire order, corners black; NULL until read */
    ld_t      ld;
    uint8_t   tries;
} item_t;

/* What the loader is asked, and what it answers. One at a time. */
typedef struct {
    char     id[ORB_THEME_HANDLE_MAX + 1];
    int      item;
    bool     hi;                /* the centre: the sharp plane too                     */
    uint32_t moves, session;
} job_t;
typedef struct {
    int       item;
    uint32_t  session;
    uint16_t *thumb;
    bool      hi;               /* s_hi now holds the item's sharp plane              */
    bool      none;             /* no preview to show (read, and nothing there)        */
    bool      mem;              /* short of memory: a try spent                        */
    bool      hi_asked;
} ans_t;

static item_t   *s_it;          /* MAX_ITEMS, PSRAM, while open                         */
static const orb_app_t *s_app_of;   /* whose themes these are                           */
static int       s_n;
static bool      s_open;
static uint32_t  s_session;
static volatile uint32_t s_moves;   /* every move: a read in flight for another item stops */
static volatile bool     s_moving;

static float     s_pos, s_pos0;
static int       s_target;
static int64_t   s_t0, s_rest_from;
static bool      s_turning, s_dirty;

static uint16_t *s_hi;          /* D_C x D_C wire order, the sharp centre              */

/* §61: the centre's sharp plane, taken when the strip opens. A preview is never inflated
 * whole: a 434 KB square stopped being there once thumbnails had cut PSRAM up and the clock
 * held its art beside the menu (Gold, 2026-11-08: one theme in four came back grey, then the
 * Back card), so each one streams through a 32 KB window and is boxed down row by row into
 * its thumbnail and, for the centre, this plane. The loader writes it while a job runs; a
 * close that finds the loader busy leaves it to theme_strip_service. It is single: the strip
 * stops drawing it (s_hi_i = -1) before a job that rewrites it is posted, and the Back card
 * borrows it (s_hi_i = 0) while it is the centre. */
static bool      s_orphans;
static int       s_hi_i = -1;
static int       s_hi_failed = -1;  /* asked for and not made (memory): not asked again */
static uint16_t *s_back;        /* the Back card's picture, s_back_side square         */

static struct { int item; uint8_t *a; uint32_t used; } s_names[NAMES];  /* alpha 0..32 */
static uint32_t  s_name_clock;

/* The loader: its own task, one job at a time, its answer reaped on the LVGL task. */
static TaskHandle_t      s_ld;
static SemaphoreHandle_t s_ld_mx;
static volatile bool     s_ld_busy;
static job_t             s_job;
static ans_t             s_ans;
static volatile bool     s_ans_ready;

static inline uint16_t wire(uint16_t c) { return (uint16_t)((c << 8) | (c >> 8)); }

/* ---- pictures ---------------------------------------------------------------------- */

/* §61: the load in flight, as the waiting card's progress ring draws it. The loader writes;
 * the strip reads and redraws when the arc has grown enough to see. Reading the card is the
 * first 70%, streaming the picture down the rest. */
#define PROG_READ  700
#define PROG_STEP  25           /* permille between two redraws of the ring */
static volatile int      s_prog_item = -1;
static volatile uint16_t s_prog;            /* permille */
static volatile bool     s_prog_stream;     /* the loader is streaming: disc_sink reports */
static int               s_prog_drawn = -1;

static bool keep_reading(void *arg, uint32_t done, uint32_t total)
{
    const job_t *j = (const job_t *)arg;
    if (total) s_prog = (uint16_t)((uint64_t)done * PROG_READ / total);
    return s_open && !s_moving && j->moves == s_moves && j->session == s_session;
}

/* ---- streaming a disc down (§61) -------------------------------------------------- */

/* One box-filtered target: the rows of a `sw` square in, a `dw` square out in wire order,
 * by the spans orb_strip.c's box_down uses, so the two carousels shrink alike. An upscale
 * (an older 232 px preview into the 300 centre) repeats rows. */
typedef struct {
    int       sw, dw, dy;
    uint16_t *dst;
    uint32_t *acc;              /* dw x {r, g, b, n} */
} scaler_t;

static void sc_row(scaler_t *c, int y, const uint16_t *row)
{
    while (c->dy < c->dw) {
        const int y0 = c->dy * c->sw / c->dw;
        const int y1 = (c->dy + 1) * c->sw / c->dw > y0 + 1 ? (c->dy + 1) * c->sw / c->dw : y0 + 1;
        if (y < y0) return;
        for (int dx = 0; dx < c->dw; dx++) {
            const int x0 = dx * c->sw / c->dw;
            const int x1 = (dx + 1) * c->sw / c->dw > x0 + 1 ? (dx + 1) * c->sw / c->dw : x0 + 1;
            uint32_t *a = c->acc + dx * 4;
            for (int x = x0; x < x1 && x < c->sw; x++) {
                const uint16_t v = row[x];
                a[0] += v >> 11; a[1] += (v >> 5) & 63; a[2] += v & 31; a[3]++;
            }
        }
        if (y < y1 - 1) return;
        for (int dx = 0; dx < c->dw; dx++) {
            uint32_t *a = c->acc + dx * 4;
            const uint32_t n = a[3] ? a[3] : 1;
            c->dst[(size_t)c->dy * c->dw + dx] =
                wire((uint16_t)(((a[0] / n) << 11) | ((a[1] / n) << 5) | (a[2] / n)));
            a[0] = a[1] = a[2] = a[3] = 0;
        }
        c->dy++;
    }
}

typedef struct {
    int       side, y, x0, n, k;
    bool      half;
    uint8_t   lo;
    uint16_t *row;
    scaler_t *sc;
    int       nsc;
} disc_in_t;

static void disc_next_row(disc_in_t *d)
{
    uint32_t x0, x1;
    orb_disc_span_n((uint32_t)d->side, (uint32_t)d->y, &x0, &x1);
    d->x0 = (int)x0;
    d->n = (int)(x1 - x0 + 1);
    d->k = 0;
    memset(d->row, 0, (size_t)d->side * 2);
}

static void disc_sink(void *ctx, const uint8_t *p, size_t n)
{
    disc_in_t *d = ctx;
    for (size_t i = 0; i < n && d->y < d->side; i++) {
        if (!d->half) { d->lo = p[i]; d->half = true; continue; }
        d->half = false;
        d->row[d->x0 + d->k++] = (uint16_t)(d->lo | (p[i] << 8));
        if (d->k == d->n) {
            for (int s = 0; s < d->nsc; s++) sc_row(&d->sc[s], d->y, d->row);
            if (++d->y < d->side) disc_next_row(d);
            if (s_prog_stream)
                s_prog = (uint16_t)(PROG_READ + (1000 - PROG_READ) * d->y / d->side);
        }
    }
}

/* A deflated disc of `side` px into `thumb` (TH) and, when non-NULL, `hi` (D_C), both
 * optional; or a whole square `sq` (native order) when z is NULL. */
static bool stream_down(const uint8_t *z, uint32_t zn, const uint16_t *sq, int side,
                        uint16_t *thumb, uint16_t *hi)
{
    scaler_t sc[2];
    int nsc = 0;
    uint32_t *acc = heap_caps_calloc((size_t)(TH + D_C) * 4, sizeof *acc, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *row = heap_caps_malloc((size_t)side * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (acc && row) {
        if (thumb) sc[nsc++] = (scaler_t){ .sw = side, .dw = TH, .dst = thumb, .acc = acc };
        if (hi)    sc[nsc++] = (scaler_t){ .sw = side, .dw = D_C, .dst = hi, .acc = acc + TH * 4 };
        if (z) {
            disc_in_t d = { .side = side, .row = row, .sc = sc, .nsc = nsc };
            disc_next_row(&d);
            ok = orb_inflate_stream(z, zn, orb_disc_px((uint32_t)side) * 2u, disc_sink, &d) &&
                 d.y == side;
        } else {
            for (int y = 0; y < side; y++)
                for (int s = 0; s < nsc; s++) sc_row(&sc[s], y, sq + (size_t)y * side);
            ok = true;
        }
    }
    heap_caps_free(acc);
    heap_caps_free(row);
    return ok;
}

/* One theme's preview into its thumbnail (and the sharp plane for the centre): the stored
 * deflated bytes read in short holds and streamed down with the card free, or (a theme baked
 * before v1.2.20) the raw plane read whole, which is small. *gone when the read was
 * abandoned for a move, *mem when memory ran short. */
static bool read_preview(job_t *j, uint16_t *thumb, uint16_t *hi, bool *gone, bool *mem)
{
    *gone = false;
    *mem = false;
    uint32_t zn = 0;
    uint16_t zs = 0;
    char why[72] = "";
    uint8_t *z = orb_theme_preview_zcopy_ex(j->id, &zn, &zs, why, sizeof why, keep_reading, j);
    if (!z && !keep_reading(j, 0, 0)) { *gone = true; return false; }
    if (z) {
        const bool ok = zs <= W && stream_down(z, zn, NULL, zs, thumb, hi);
        heap_caps_free(z);
        if (!ok) { ESP_LOGW(TAG, "%s: %u B did not stream down", j->id, (unsigned)zn); *mem = true; }
        return ok;
    }
    if (!why[0]) { *mem = true; return false; }   /* no reason: the copy's own buffer */
    uint16_t w = 0, h = 0;
    if (orb_theme_preview_read(j->id, NULL, 0, &w, &h, why, sizeof why) != ORB_ASSET_OK ||
        !w || w != h || w > W)
        return false;
    uint16_t *sq = heap_caps_malloc((size_t)w * w * 2u, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sq) { *mem = true; return false; }
    bool ok = orb_theme_preview_read(j->id, (uint8_t *)sq, (size_t)w * w * 2u, &w, &h,
                                     why, sizeof why) == ORB_ASSET_OK &&
              stream_down(NULL, 0, sq, w, thumb, hi);
    heap_caps_free(sq);
    return ok;
}

static void ld_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_t j = s_job;
        ans_t a = { .item = j.item, .session = j.session, .hi_asked = j.hi };
        s_prog = 0;
        s_prog_item = j.item;
        s_prog_stream = true;
        bool gone = false, mem = false;
        uint16_t *thumb = heap_caps_malloc((size_t)TH * TH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *hi = j.hi ? s_hi : NULL;
        bool got = false;
        if (!thumb) mem = true;
        else if (orb_sd_mounted() && !orb_theme_upload_busy())
            got = read_preview(&j, thumb, hi, &gone, &mem);
        if (got) {
            a.thumb = thumb;
            a.hi = hi != NULL;
        } else {
            heap_caps_free(thumb);
            if (!gone && !mem) {
                a.none = true;
                ESP_LOGW(TAG, "%s: no picture to show", j.id);
            }
        }
        s_prog_stream = false;
        a.mem = mem && !gone && !got;
        if (gone || a.mem) a.item = -a.item - 1;   /* not done: read it again later */
        xSemaphoreTake(s_ld_mx, portMAX_DELAY);
        s_ans = a;
        s_ans_ready = true;
        xSemaphoreGive(s_ld_mx);
        s_ld_busy = false;
    }
}

static void hi_take(void)
{
    if (!s_hi) s_hi = heap_caps_malloc((size_t)D_C * D_C * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool ld_post(int item, bool hi)
{
    if (s_ld_busy || s_ans_ready || item <= 0 || item >= s_n) return false;
    hi_take();                                       /* without it the centre wears its thumb */
    if (hi && s_hi && s_hi_i == item) hi = false;
    if (hi && s_hi) s_hi_i = -1;                     /* not drawn while it is rewritten */
    if (!s_ld) {
        if (!s_ld_mx) s_ld_mx = xSemaphoreCreateMutex();
        if (!s_ld_mx ||
            xTaskCreatePinnedToCoreWithCaps(ld_main, "theme_ts", 8192, NULL, 2, &s_ld,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_ld = NULL;
            return false;
        }
    }
    memset(&s_job, 0, sizeof s_job);
    strlcpy(s_job.id, s_it[item].id, sizeof s_job.id);
    s_job.item = item;
    s_job.hi = hi;
    s_job.moves = s_moves;
    s_job.session = s_session;
    s_ld_busy = true;
    if (s_it[item].ld == LD_NONE) s_it[item].ld = LD_POSTED;
    xTaskNotifyGive(s_ld);
    return true;
}

/* Take a finished load: file its pictures, or free them when the strip it was for has
 * closed (a new open is a new session). */
static int dist_to_cursor(int i)
{
    const int c = (int)floorf(s_pos + 0.5f);
    return i > c ? i - c : c - i;
}

static void ld_reap(void)
{
    if (!s_ans_ready) return;
    xSemaphoreTake(s_ld_mx, portMAX_DELAY);
    ans_t a = s_ans;
    s_ans_ready = false;
    xSemaphoreGive(s_ld_mx);
    s_prog_item = -1;                                   /* the ring is done either way */
    s_prog_drawn = -1;
    s_dirty = true;
    const bool mine = s_open && a.session == s_session;
    if (!mine) { heap_caps_free(a.thumb); return; }
    if (a.item < 0) {                                   /* abandoned, or short of memory */
        const int i = -a.item - 1;
        if (i > 0 && i < s_n && !s_it[i].thumb) {
            s_it[i].ld = (a.mem && ++s_it[i].tries >= TRIES) ? LD_NONE_TO_SHOW : LD_NONE;
        }
        heap_caps_free(a.thumb);
        return;
    }
    if (a.item <= 0 || a.item >= s_n) { heap_caps_free(a.thumb); return; }
    item_t *it = &s_it[a.item];
    if (a.thumb) {
        heap_caps_free(it->thumb);
        it->thumb = a.thumb;
        it->ld = LD_THUMB;
    } else {
        it->ld = a.none ? LD_NONE_TO_SHOW : (it->thumb ? LD_THUMB : LD_NONE);
    }
    if (a.hi) s_hi_i = a.item; else if (a.hi_asked && !a.thumb) {
        s_hi_failed = a.item;
    }
    /* Thumbnails far from the cursor go, so a long list costs KEEP either side. */
    for (int i = 1; i < s_n; i++)
        if (s_it[i].thumb && dist_to_cursor(i) > KEEP) {
            heap_caps_free(s_it[i].thumb);
            s_it[i].thumb = NULL;
            s_it[i].ld = LD_NONE;
        }
    s_dirty = true;
}

/* ---- names --------------------------------------------------------------------------- */

/* The name plane of item i: drawn once by LVGL into a canvas that never reaches a screen,
 * kept as coverage (0..32), the most recent NAMES of them. */
static const uint8_t *name_of(int i)
{
    int slot = -1, old = 0;
    for (int k = 0; k < NAMES; k++) {
        if (s_names[k].a && s_names[k].item == i) { s_names[k].used = ++s_name_clock; return s_names[k].a; }
        if (!s_names[k].a) { if (slot < 0) slot = k; }
        else if (s_names[k].used < s_names[old].used) old = k;
    }
    if (slot < 0) slot = old;
    if (!s_names[slot].a)
        s_names[slot].a = heap_caps_malloc((size_t)NAME_W * NAME_H, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_names[slot].a) return NULL;
    lv_draw_buf_t *db = lv_draw_buf_create(NAME_W, NAME_H, LV_COLOR_FORMAT_RGB565, 0);
    lv_obj_t *cv = db ? lv_canvas_create(lv_layer_sys()) : NULL;
    if (!cv) {
        if (db) lv_draw_buf_destroy(db);
        return NULL;
    }
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(cv, db);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_white();
    d.font  = &lv_font_montserrat_28;
    d.align = LV_TEXT_ALIGN_CENTER;
    d.text  = s_it[i].name;
    lv_area_t ar = { 0, 3, NAME_W - 1, NAME_H - 1 };
    lv_draw_label(&layer, &d, &ar);
    lv_canvas_finish_layer(cv, &layer);
    uint8_t *dst = s_names[slot].a;
    for (int y = 0; y < NAME_H; y++) {
        const uint16_t *src = (const uint16_t *)(db->data + (size_t)y * db->header.stride);
        for (int x = 0; x < NAME_W; x++)
            dst[y * NAME_W + x] = (uint8_t)((((src[x] >> 5) & 63) * 32 + 31) / 63);
    }
    lv_obj_delete(cv);
    lv_draw_buf_destroy(db);
    s_names[slot].item = i;
    s_names[slot].used = ++s_name_clock;
    return dst;
}

/* The Back card: the drawn one the firmware carries (orb_ui_icons.c, §61), at its own 300 px
 * so the centre shows it 1:1; a plain disc with a chevron if it will not inflate. */
static int s_back_side = TH;
static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float t = (vx * wx + vy * wy) / (vx * vx + vy * vy);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float dx = wx - t * vx, dy = wy - t * vy;
    return sqrtf(dx * dx + dy * dy);
}

static void make_back(void)
{
    if (s_back) return;
    s_back = heap_caps_malloc((size_t)TH * TH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_back) return;
    s_back_side = TH;
    if (stream_down(orb_ui_icon_back_z, orb_ui_icon_back_n, NULL, orb_ui_icon_back_side,
                    s_back, NULL))
        return;
    ESP_LOGW(TAG, "the drawn Back card did not stream down: the plain one stands in");
    const float c = TH / 2.0f - 0.5f;
    const float tip_x = TH * 0.36f, arm_x = TH * 0.58f, cy = TH * 0.40f, arm = TH * 0.20f;
    for (int y = 0; y < TH; y++)
        for (int x = 0; x < TH; x++) {
            const float dx = x - c, dy = y - c;
            uint16_t v = 0;
            if (dx * dx + dy * dy <= c * c) {
                const float d = fminf(seg_dist(x, y, tip_x, cy, arm_x, cy - arm),
                                      seg_dist(x, y, tip_x, cy, arm_x, cy + arm));
                v = d <= 6.0f ? 0xFFFF : 0x2945;
            }
            s_back[y * TH + x] = wire(v);
        }
}

/* The Back card sharp while it is the centre: streamed into the centre's plane, which no
 * load is writing (the loader is idle). LVGL task. */
static void back_hi(void)
{
    hi_take();
    if (!s_hi || s_ld_busy || s_hi_i == 0) return;
    s_hi_i = -1;
    if (stream_down(orb_ui_icon_back_z, orb_ui_icon_back_n, NULL, orb_ui_icon_back_side,
                    NULL, s_hi)) {
        s_hi_i = 0;
        s_dirty = true;
    }
}

/* ---- composing ------------------------------------------------------------------------- */

typedef struct {
    const uint16_t *px;         /* NULL: a plain card                                   */
    int   side;
    float cy, d;
    int   prog;                 /* a plain card being read: its ring, permille; -1 none */
} disc_t;

static disc_t          s_draw[5];
static int             s_draw_n;
static const uint8_t  *s_label;
static uint16_t        s_label_col;      /* wire order */

static void disc_row(uint16_t *dst, int y, const disc_t *c)
{
    const float r = c->d * 0.5f, dy = (y + 0.5f) - c->cy;
    if (dy <= -r || dy >= r) return;
    const float hw = sqrtf(r * r - dy * dy);
    int x0 = (int)(CX - hw + 0.5f), x1 = (int)(CX + hw - 0.5f);
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x1 < x0) return;
    if (!c->px) {                                        /* not read yet: a card */
        const uint16_t g = wire(0x2945), rim = wire(0x52AA);
        if (c->prog < 0) {
            const bool edge = dy <= -r + 2 || dy >= r - 2;
            for (int x = x0; x <= x1; x++) dst[x] = (edge || x <= x0 + 1 || x >= x1 - 1) ? rim : g;
            return;
        }
        /* §61: the progress ring, clockwise from twelve: a bright arc over a dim track,
         * just inside the card's edge. */
        const uint16_t track = wire(0x3186), arc = wire(0x66DF);
        const float rw = c->d * 0.045f + 2.0f, inner = r - rw - 1.0f;
        const float lim = (float)c->prog * (2.0f * (float)M_PI) / 1000.0f;
        for (int x = x0; x <= x1; x++) {
            const float dx = (x + 0.5f) - CX;
            const float dist = sqrtf(dx * dx + dy * dy);
            if (dist < inner || dist > r - 1.0f) { dst[x] = g; continue; }
            float a = atan2f(dx, -dy);
            if (a < 0) a += 2.0f * (float)M_PI;
            dst[x] = a <= lim ? arc : track;
        }
        return;
    }
    const float s = (float)c->side / c->d;
    int sy = (int)((dy + r) * s);
    if (sy < 0) sy = 0;
    if (sy >= c->side) sy = c->side - 1;
    const uint16_t *row = c->px + (size_t)sy * (size_t)c->side;
    const uint32_t step = (uint32_t)(s * 65536.0f);
    uint32_t fx = (uint32_t)(((x0 + 0.5f) - (CX - r)) * s * 65536.0f);
    for (int x = x0; x <= x1; x++, fx += step) {
        uint32_t sx = fx >> 16;
        if (sx >= (uint32_t)c->side) sx = (uint32_t)c->side - 1;
        dst[x] = row[sx];
    }
}

static inline uint16_t mix_wire(uint16_t aw, uint16_t bw, uint32_t a)
{
    const uint32_t x = __builtin_bswap16(aw), y = __builtin_bswap16(bw);
    const uint32_t A = (x | (x << 16)) & 0x07E0F81Fu;
    const uint32_t B = (y | (y << 16)) & 0x07E0F81Fu;
    const uint32_t R = ((A * (32u - a) + B * a) >> 5) & 0x07E0F81Fu;
    return __builtin_bswap16((uint16_t)(R | (R >> 16)));
}

static void compose(int y0, int rows, uint16_t *dst, void *arg)
{
    (void)arg;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *d = dst + (size_t)r * W;
        memset(d, 0, (size_t)W * 2);
        for (int i = 0; i < s_draw_n; i++) disc_row(d, y, &s_draw[i]);
        if (s_label && y >= NAME_Y && y < NAME_Y + NAME_H) {
            /* The band darkens what is under it, and the name is laid over that. */
            const uint8_t *a = s_label + (size_t)(y - NAME_Y) * NAME_W;
            uint16_t *p = d + (CX - NAME_W / 2);
            for (int x = 0; x < NAME_W; x++) {
                const uint16_t under = p[x] ? mix_wire(p[x], 0, 18) : 0;
                p[x] = a[x] ? mix_wire(under, s_label_col, a[x]) : under;
            }
        }
    }
}

/* Lay the strip out at position pos: neighbours first, the centre last, on top. */
static void layout(float pos)
{
    s_draw_n = 0;
    const int c = (int)floorf(pos + 0.5f);
    static const int order[5] = { -2, 2, -1, 1, 0 };
    for (int oi = 0; oi < 5; oi++) {
        const int idx = c + order[oi];
        if (idx < 0 || idx >= s_n) continue;
        const float dd = (float)idx - pos, ad = fabsf(dd);
        if (ad > 1.7f) continue;
        disc_t *q = &s_draw[s_draw_n++];
        const float t = ad > 1.0f ? 1.0f : ad;
        q->d  = D_C + (D_N - D_C) * t;
        q->cy = CY + dd * SP;
        q->prog = -1;
        if (ad < 0.02f && s_hi && s_hi_i == idx) { q->px = s_hi; q->side = D_C; }
        else if (idx == 0) { q->px = s_back; q->side = s_back_side; }
        else {
            q->px = s_it[idx].thumb; q->side = TH;
            if (!q->px && idx == s_prog_item) q->prog = s_prog;
        }
    }
    /* The name of the theme nearest the centre, all the way through a spin. */
    s_label = name_of(c < 0 ? 0 : (c >= s_n ? s_n - 1 : c));
    const bool on = c > 0 && c < s_n && s_it[c].active;
    s_label_col = wire(on ? 0xFD20 : 0xFFFF);            /* the one on the dial: amber */
}

static bool frame(void)
{
    display_fx_stats_t st;
    display_fx_shadow_next(!s_turning);
    return display_fx_frame_try(compose, NULL, true, &st) == ESP_OK;
}

/* ---- open, run, close -------------------------------------------------------------------- */

static void free_all(void)
{
    for (int i = 0; s_it && i < s_n; i++) heap_caps_free(s_it[i].thumb);
    heap_caps_free(s_it);
    s_it = NULL;
    s_n = 0;
    /* The loader may still be inflating into these: then they wait for it. */
    if (s_ld_busy) {
        s_orphans = true;
    } else {
        heap_caps_free(s_hi);
        s_hi = NULL;
    }
    s_hi_i = -1;
    s_hi_failed = -1;
    heap_caps_free(s_back);
    s_back = NULL;
    for (int k = 0; k < NAMES; k++) { heap_caps_free(s_names[k].a); s_names[k].a = NULL; }
}

bool theme_strip_active(void) { return s_open; }

bool theme_strip_open(const orb_app_t *app)
{
    if (s_open || s_orphans || !app || !app->slug) return false;
    const int n = themes_page_count_for(app->slug);
    if (n <= 0) return false;
    hi_take();
    s_it = heap_caps_calloc(MAX_ITEMS, sizeof *s_it, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_it) { free_all(); return false; }
    s_n = 1;
    strlcpy(s_it[0].name, "Back", sizeof s_it[0].name);
    s_it[0].ok = true;
    int start = 1;
    for (int i = 0; i < n && s_n < MAX_ITEMS; i++) {
        const char *id = NULL, *name = NULL;
        bool active = false, ok = false;
        if (!themes_page_item(app->slug, i, &id, &name, &active, &ok)) break;
        item_t *it = &s_it[s_n];
        strlcpy(it->id, id, sizeof it->id);
        strlcpy(it->name, name, sizeof it->name);
        it->active = active;
        it->ok = ok;
        it->ld = ok ? LD_NONE : LD_NONE_TO_SHOW;
        if (active) start = s_n;
        s_n++;
    }
    make_back();
    s_app_of = app;
    s_session++;
    s_moves++;
    s_open = true;
    s_moving = false;
    s_turning = false;
    s_pos = (float)start;
    s_target = start;
    s_rest_from = esp_timer_get_time();
    display_ring_urgent(true);
    display_glass_hold(true);
    layout(s_pos);
    s_dirty = !frame();
    ESP_LOGI(TAG, "open: %s, %d themes, on %d", app->slug, s_n - 1, start);
    return true;
}

static void close_strip(const char *status)
{
    if (!s_open) return;
    int pics = 0;
    for (int i = 1; i < s_n; i++) {
        if (s_it[i].thumb) pics++;
        else ESP_LOGI(TAG, "no picture yet: %s (%d)", s_it[i].name, (int)s_it[i].ld);
    }
    ESP_LOGI(TAG, "%d of %d pictures read", pics, s_n - 1);
    s_open = false;
    s_moves++;                                   /* a read in flight gives up */
    free_all();
    display_glass_hold(false);
    display_ring_urgent(false);
    if (status) menu_view_status(status);
    ESP_LOGI(TAG, "closed%s%s", status ? ": " : "", status ? status : "");
}

void theme_strip_abort(void) { close_strip(NULL); }

void theme_strip_service(void)
{
    if (s_ld_mx) ld_reap();
    if (s_orphans && !s_ld_busy && !s_open) {
        heap_caps_free(s_hi);
        s_hi = NULL;
        s_orphans = false;
    }
}

static void start_turn(int steps, int64_t now)
{
    int t = (s_turning ? s_target : (int)floorf(s_pos + 0.5f)) + steps;
    if (t < 0) t = 0;
    if (t > s_n - 1) t = s_n - 1;
    if (t == s_target && s_turning) return;
    if (t == (int)floorf(s_pos + 0.5f) && !s_turning) return;
    s_pos0 = s_pos;
    s_target = t;
    s_t0 = now;
    s_turning = true;
    s_moving = true;
    s_moves++;
}

/* While resting: read what the glass wants next. The centre first (its sharp plane, and
 * its thumbnail if it has none), then the nearest theme still a plain card, ahead of the
 * way the owner last turned first. */
static int s_dir = 1;
static void feed_loader(int64_t now)
{
    if (s_turning || s_ld_busy || s_ans_ready || now - s_rest_from < (int64_t)REST_MS * 1000) return;
    const int c = (int)floorf(s_pos + 0.5f);
    if (c == 0) { back_hi(); return; }
    if (c > 0 && s_it[c].ok && s_it[c].ld != LD_NONE_TO_SHOW && s_hi_i != c && s_hi_failed != c) {
        ld_post(c, true);
        return;
    }
    for (int d = 1; d <= KEEP && d < s_n; d++) {
        const int pick[2] = { c + d * s_dir, c - d * s_dir };
        for (int k = 0; k < 2; k++) {
            const int i = pick[k];
            if (i <= 0 || i >= s_n) continue;
            if (s_it[i].ld == LD_NONE && s_it[i].ok) { ld_post(i, false); return; }
        }
    }
}

static void step_once(int32_t delta, bool pressed, bool lpress, int64_t now)
{
    if (!s_open) return;
    if (lpress) { close_strip(NULL); return; }
    if (pressed) {
        const int c = s_turning ? s_target : (int)floorf(s_pos + 0.5f);
        if (c <= 0) { close_strip(NULL); return; }
        if (s_it[c].active) {                           /* §61: already worn: back to the app */
            const orb_app_t *a = s_app_of;
            close_strip(NULL);
            if (a) input_router_show(a);
            return;
        }
        if (!s_it[c].ok) return;                         /* it will not install: stay */
        char id[sizeof s_it[c].id];
        strlcpy(id, s_it[c].id, sizeof id);
        close_strip(themes_page_pick_id(id));
        return;
    }
    if (delta) {
        const int steps = knob_row_step(delta) * (delta < 0 ? -delta : delta);
        if (steps) s_dir = steps > 0 ? 1 : -1;
        start_turn(steps, now);
    }
    if (s_turning) {
        const int n = s_target - (int)floorf(s_pos0 + 0.5f);
        const float span = (float)(n < 0 ? -n : n);
        const float t = (float)(now - s_t0) / (STEP_MS * 1000.0f * (span > 1 ? 0.6f + 0.4f * span : 1.0f));
        const float p = t >= 1.0f ? 1.0f : 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        s_pos = s_pos0 + ((float)s_target - s_pos0) * p;
        if (t >= 1.0f) {
            s_pos = (float)s_target;
            s_turning = false;
            s_moving = false;
            s_rest_from = now;
        }
        layout(s_pos);
        const bool drawn = frame();
        s_dirty = !drawn && !s_turning;                  /* a skipped last frame is owed */
        return;
    }
    /* §61: the ring of a waiting card on the glass, redrawn as it grows. */
    const int pi = s_prog_item, pc = (int)floorf(s_pos + 0.5f);
    if (pi > 0 && pi < s_n && !s_it[pi].thumb && pi >= pc - 2 && pi <= pc + 2 &&
        (s_prog_drawn < 0 || (int)s_prog - s_prog_drawn >= PROG_STEP)) {
        s_prog_drawn = s_prog;
        s_dirty = true;
    }
    if (s_dirty) { layout(s_pos); s_dirty = !frame(); }
    feed_loader(now);
}

/* While it moves, frames go back to back for up to 200 ms of this poll, the knob read
 * between them so a turn mid-motion retargets at once (orb_strip_poll's shape). */
void theme_strip_poll(int32_t delta, bool pressed, bool lpress, int64_t now)
{
    if (!s_open) return;
    ld_reap();
    step_once(delta, pressed, lpress, now);
    const int64_t until = now + 200000;
    while (s_open && s_turning && esp_timer_get_time() < until) {
        knob_poll();
        const int32_t d = knob_take_delta() + knob_take_push_delta();
        const bool    p = knob_take_press();
        const bool    l = knob_take_long_press();
        step_once(d, p, l, esp_timer_get_time());
    }
}
