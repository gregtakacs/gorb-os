/**
 * orb_fx.c — the shell-transition bench (see header).
 *
 * Every effect is a slab painter: given rows [y, y+rows) it writes full-width
 * wire-order pixels and returns. Two sources only. FROM is the panel mirror,
 * already in wire order at full resolution — the glass the user is looking at.
 * TO is the theme's preview plane, byte-swapped to wire order once at hand-over
 * and sampled nearest-neighbour through two maps built once per run, so the
 * full-screen preview costs a table lookup per pixel and no division.
 *
 * Per frame, the painter's parameters (an offset, a radius, an alpha, a scale)
 * are computed once in fx_prepare() so the per-row work is spans and copies.
 * That is the cost model the bench exists to price: row copies, span fills, a
 * per-pixel blend (fade), and a per-pixel scaled gather (disc).
 */

#include "orb_fx.h"

#if CONFIG_ORB_DEBUG_HTTP

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_config.h"
#include "display.h"

static const char *TAG = "fx";

#define W  SCREEN_WIDTH
#define H  SCREEN_HEIGHT
#define CX (W / 2)
#define CY (H / 2)

static const char *const s_names[ORB_FX_COUNT] = {
    [ORB_FX_SLIDE] = "slide",
    [ORB_FX_IRIS]  = "iris",
    [ORB_FX_FADE]  = "fade",
    [ORB_FX_DISC]  = "disc",
    [ORB_FX_DRUM]  = "drum",
};

const char *orb_fx_name(orb_fx_kind_t k)
{
    return (unsigned)k < ORB_FX_COUNT ? s_names[k] : NULL;
}

int orb_fx_find(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < ORB_FX_COUNT; i++)
        if (strcmp(name, s_names[i]) == 0) return i;
    return -1;
}

/* ---- the run's state ------------------------------------------------------ */

typedef struct {
    orb_fx_req_t    req;
    uint16_t       *plane;          /* wire order after hand-over */
    uint16_t        pw, ph;
    orb_fx_report_t rep;
    bool            abandoned;      /* the door stopped waiting; service frees */
} fx_job_t;

typedef struct {
    const uint16_t *from;           /* panel mirror, wire order, stride W */
    const uint16_t *plane;
    int             pw, ph;
    uint16_t        xmap[W];        /* dest column -> plane column, full screen */
    uint16_t        ymap[H];        /* dest row    -> plane row,    full screen */
    orb_fx_kind_t   kind;
    int             dir;
    /* per frame, set by fx_prepare() */
    float           p;              /* eased progress 0..1 */
    int             off;            /* slide: px travelled */
    int             r2;             /* iris: radius squared, -1 = all preview */
    int             a5;             /* fade: 0..32 weight of the preview */
    int             bnd;            /* drum: first preview row */
    int             phase;          /* disc: 0 shrink, 1 slide, 2 grow */
    float           s_from, s_to;   /* disc: dest px per source px */
    int             cx_from, cx_to; /* disc: centres */
} fx_ctx_t;

static volatile fx_job_t *s_job;    /* posted by the door, run by the poll */
static SemaphoreHandle_t  s_done;
static SemaphoreHandle_t  s_lock;   /* the abandoned/give handshake */
static fx_ctx_t          *s_ctx;    /* PSRAM, built once, ~2 KB */

/* ---- sources -------------------------------------------------------------- */

static inline void from_span(const fx_ctx_t *c, int y, int x0, int x1,
                             int sx0, uint16_t *d)
{
    if (x1 < x0) return;
    memcpy(d + x0, c->from + (size_t)y * W + sx0, (size_t)(x1 - x0 + 1) * 2);
}

/* The preview, full screen, at source column sx..: d[x0..x1] = to(y, sx0 + i). */
static inline void to_span(const fx_ctx_t *c, int y, int x0, int x1, int sx0,
                           uint16_t *d)
{
    const uint16_t *row = c->plane + (size_t)c->ymap[y] * (size_t)c->pw;
    const uint16_t *xm  = c->xmap + sx0;
    for (int x = x0, i = 0; x <= x1; x++, i++) d[x] = row[xm[i]];
}

/* One source (stride `stride`, sw x sh) scaled by `s` dest px per source px,
 * centred on (cx, cy), stamped into dest row y. Pixels outside the box are not
 * touched. Q16 stepping: one add per pixel, no division in the loop. */
static void scaled_row(uint16_t *d, int y, const uint16_t *src, int sw, int sh,
                       int stride, int cx, int cy, float s)
{
    if (s <= 0.0f) return;
    const float hw = sw * s * 0.5f, hh = sh * s * 0.5f;
    const float top = cy - hh;
    if (y < (int)top || y >= (int)(cy + hh)) return;
    int sy = (int)((y - top) / s);
    if (sy < 0 || sy >= sh) return;
    int x0 = (int)(cx - hw), x1 = (int)(cx + hw) - 1;
    if (x0 < 0) x0 = 0;
    if (x1 > W - 1) x1 = W - 1;
    if (x1 < x0) return;
    const uint32_t step = (uint32_t)(65536.0f / s);
    uint32_t fx = (uint32_t)((x0 - (cx - hw)) / s * 65536.0f);
    const uint16_t *row = src + (size_t)sy * (size_t)stride;
    for (int x = x0; x <= x1; x++, fx += step) {
        uint32_t sx = fx >> 16;
        if (sx >= (uint32_t)sw) sx = (uint32_t)sw - 1;
        d[x] = row[sx];
    }
}

/* Wire-order RGB565 blend, a5 of 32 toward b. The 0x07E0F81F spread keeps each
 * channel's product inside its own bits (max 31*32 / 63*32), so one multiply
 * pair blends all three. */
static inline uint16_t mix_wire(uint16_t aw, uint16_t bw, uint32_t a5)
{
    const uint32_t a = __builtin_bswap16(aw), b = __builtin_bswap16(bw);
    const uint32_t A = (a | (a << 16)) & 0x07E0F81Fu;
    const uint32_t B = (b | (b << 16)) & 0x07E0F81Fu;
    const uint32_t R = ((A * (32u - a5) + B * a5) >> 5) & 0x07E0F81Fu;
    return __builtin_bswap16((uint16_t)(R | (R >> 16)));
}

/* ---- the painters --------------------------------------------------------- */

static void fx_compose(int y0, int rows, uint16_t *dst, void *arg)
{
    const fx_ctx_t *c = (const fx_ctx_t *)arg;
    for (int r = 0; r < rows; r++) {
        const int y = y0 + r;
        uint16_t *d = dst + (size_t)r * W;
        switch (c->kind) {
        case ORB_FX_SLIDE: {
            const int off = c->off;
            if (c->dir >= 0) {          /* glass leaves left, preview enters right */
                from_span(c, y, 0, W - 1 - off, off, d);
                to_span(c, y, W - off, W - 1, 0, d);
            } else {                    /* glass leaves right, preview enters left */
                to_span(c, y, 0, off - 1, W - off, d);
                from_span(c, y, off, W - 1, 0, d);
            }
            break;
        }
        case ORB_FX_IRIS: {
            if (c->r2 < 0) { to_span(c, y, 0, W - 1, 0, d); break; }
            const int dy = 2 * (y - CY) + 1;          /* doubled: half-pixel centre */
            const int rem = c->r2 - dy * dy;
            if (rem <= 0) { from_span(c, y, 0, W - 1, 0, d); break; }
            const int half = (int)(sqrtf((float)rem) * 0.5f);
            int x0 = CX - half, x1 = CX + half - 1;
            if (x0 < 0) x0 = 0;
            if (x1 > W - 1) x1 = W - 1;
            from_span(c, y, 0, x0 - 1, 0, d);
            to_span(c, y, x0, x1, x0, d);
            from_span(c, y, x1 + 1, W - 1, x1 + 1, d);
            break;
        }
        case ORB_FX_FADE: {
            if (c->a5 <= 0)  { from_span(c, y, 0, W - 1, 0, d); break; }
            if (c->a5 >= 32) { to_span(c, y, 0, W - 1, 0, d); break; }
            const uint16_t *f   = c->from + (size_t)y * W;
            const uint16_t *row = c->plane + (size_t)c->ymap[y] * (size_t)c->pw;
            const uint32_t a5 = (uint32_t)c->a5;
            for (int x = 0; x < W; x++) d[x] = mix_wire(f[x], row[c->xmap[x]], a5);
            break;
        }
        case ORB_FX_DISC: {
            memset(d, 0, (size_t)W * 2);
            if (c->phase == 0) {
                scaled_row(d, y, c->from, W, H, W, CX, CY, c->s_from);
            } else if (c->phase == 1) {
                scaled_row(d, y, c->from, W, H, W, c->cx_from, CY, c->s_from);
                scaled_row(d, y, c->plane, c->pw, c->ph, c->pw, c->cx_to, CY, c->s_to);
            } else {
                scaled_row(d, y, c->plane, c->pw, c->ph, c->pw, CX, CY, c->s_to);
            }
            break;
        }
        case ORB_FX_DRUM: {
            const int b = c->bnd;           /* rows [0,b) squashed glass, [b,H) preview */
            if (y < b) {
                const int sy = (int)((int64_t)y * H / b);
                memcpy(d, c->from + (size_t)sy * W, (size_t)W * 2);
            } else {
                const int ty = (int)((int64_t)(y - b) * H / (H - b));
                to_span(c, ty, 0, W - 1, 0, d);
            }
            break;
        }
        default:
            memset(d, 0, (size_t)W * 2);
        }
    }
}

/* The restore: the mirror, verbatim. Also the copy baseline. */
static void fx_copy(int y0, int rows, uint16_t *dst, void *arg)
{
    const fx_ctx_t *c = (const fx_ctx_t *)arg;
    memcpy(dst, c->from + (size_t)y0 * W, (size_t)rows * W * 2);
}

static float ease_out(float t)
{
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

static void fx_prepare(fx_ctx_t *c, float t)
{
    const float p = ease_out(t < 0 ? 0 : (t > 1 ? 1 : t));
    c->p = p;
    switch (c->kind) {
    case ORB_FX_SLIDE:
        c->off = (int)(p * W + 0.5f) & ~1;     /* even: no odd-column seam */
        if (c->off > W) c->off = W;
        break;
    case ORB_FX_IRIS: {
        /* The glass is a disc of radius CX; at p == 1 everything is preview. */
        if (p >= 1.0f) { c->r2 = -1; break; }
        const int rr = (int)(p * 2 * (CX + 2));    /* doubled radius */
        c->r2 = rr * rr;
        break;
    }
    case ORB_FX_FADE:
        c->a5 = (int)(p * 32 + 0.5f);
        break;
    case ORB_FX_DISC: {
        /* 30 % shrink to a half-size disc, 40 % travel, 30 % grow to full. The
         * travel is on linear t inside its phase so the discs keep moving. */
        const float full = (float)W / (float)c->pw;
        const int   gap  = 300 * (c->dir >= 0 ? 1 : -1);
        if (p < 0.3f) {
            const float q = p / 0.3f;
            c->phase = 0;
            c->s_from = 1.0f - 0.5f * q;
        } else if (p < 0.7f) {
            const float q = (p - 0.3f) / 0.4f;
            c->phase = 1;
            c->s_from  = 0.5f;
            c->s_to    = 0.5f * full;
            c->cx_from = CX - (int)(gap * q);
            c->cx_to   = CX + (int)(gap * (1.0f - q));
        } else {
            const float q = (p - 0.7f) / 0.3f;
            c->phase = 2;
            c->s_to  = 0.5f * full + 0.5f * full * q;
        }
        break;
    }
    case ORB_FX_DRUM:
        c->bnd = (int)((1.0f - p) * H + 0.5f);
        if (c->bnd < 0) c->bnd = 0;
        if (c->bnd > H) c->bnd = H;
        break;
    default:
        break;
    }
}

/* ---- the run, on the LVGL task -------------------------------------------- */

static void fx_execute(fx_job_t *job)
{
    orb_fx_report_t *rp = &job->rep;
    memset(rp, 0, sizeof *rp);
    fx_ctx_t *c = s_ctx;
    c->from = display_fb_mirror();
    if (!c->from) { rp->err = ESP_ERR_NOT_SUPPORTED; return; }
    c->plane = job->plane;
    c->pw    = job->pw;
    c->ph    = job->ph;
    c->kind  = job->req.kind;
    c->dir   = job->req.dir >= 0 ? 1 : -1;
    for (int x = 0; x < W; x++) c->xmap[x] = (uint16_t)((uint32_t)x * c->pw / W);
    for (int y = 0; y < H; y++) c->ymap[y] = (uint16_t)((uint32_t)y * c->ph / H);

    const int64_t ms = job->req.ms ? job->req.ms : 1;
    uint64_t sum_frame = 0, sum_comp = 0, sum_te = 0;
    /* One untimed frame of the glass as it stands: it waits out the ring the
     * preview's card read borrowed, so the clock below times frames, not the loan. */
    {
        display_fx_stats_t w;
        if (display_fx_frame(fx_copy, c, false, &w) != ESP_OK) { rp->err = ESP_ERR_INVALID_STATE; return; }
    }
    const int64_t t0 = esp_timer_get_time();
    for (;;) {
        const float t = (float)(esp_timer_get_time() - t0) / (float)(ms * 1000);
        fx_prepare(c, t);
        display_fx_stats_t st;
        const esp_err_t e = display_fx_frame(fx_compose, c, job->req.te, &st);
        if (e != ESP_OK) { rp->err = e; break; }
        rp->frames++;
        rp->tiles = st.tiles;
        sum_frame += st.frame_us + st.te_us;
        sum_comp  += st.compose_us;
        sum_te    += st.te_us;
        if (st.frame_us + st.te_us > rp->frame_max_us) rp->frame_max_us = st.frame_us + st.te_us;
        if (st.compose_us > rp->compose_max_us) rp->compose_max_us = st.compose_us;
        if (t >= 1.0f) break;
    }
    rp->anim_us = (uint32_t)(esp_timer_get_time() - t0);
    if (rp->frames) {
        rp->frame_avg_us   = (uint32_t)(sum_frame / (uint64_t)rp->frames);
        rp->compose_avg_us = (uint32_t)(sum_comp  / (uint64_t)rp->frames);
        rp->te_avg_us      = (uint32_t)(sum_te    / (uint64_t)rp->frames);
    }
    if (job->req.hold_ms) vTaskDelay(pdMS_TO_TICKS(job->req.hold_ms));

    /* Always put the glass back, even after a failed frame: the mirror is still
     * the truth of what the view drew. */
    display_fx_stats_t st;
    if (display_fx_frame(fx_copy, c, job->req.te, &st) == ESP_OK) {
        rp->restore_us         = st.frame_us;
        rp->restore_compose_us = st.compose_us;
    } else if (rp->err == ESP_OK) {
        rp->err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s %u ms: %d frames in %u us, frame avg %u max %u, compose avg %u, "
                  "te avg %u, restore %u (compose %u), ring %d",
             orb_fx_name(c->kind), (unsigned)ms, rp->frames, (unsigned)rp->anim_us,
             (unsigned)rp->frame_avg_us, (unsigned)rp->frame_max_us,
             (unsigned)rp->compose_avg_us, (unsigned)rp->te_avg_us,
             (unsigned)rp->restore_us, (unsigned)rp->restore_compose_us, rp->tiles);
}

static void job_free(fx_job_t *job)
{
    heap_caps_free(job->plane);
    heap_caps_free(job);
}

void orb_fx_service(void)
{
    fx_job_t *job = (fx_job_t *)s_job;
    if (!job) return;
    fx_execute(job);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job = NULL;
    if (job->abandoned) job_free(job);
    else xSemaphoreGive(s_done);
    xSemaphoreGive(s_lock);
}

esp_err_t orb_fx_run(const orb_fx_req_t *req, uint16_t *plane, uint16_t w,
                     uint16_t h, uint32_t wait_ms, orb_fx_report_t *out)
{
    if (!req || !plane || !w || !h || !out || (unsigned)req->kind >= ORB_FX_COUNT) {
        heap_caps_free(plane);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_done = xSemaphoreCreateBinary();
        s_ctx  = heap_caps_calloc(1, sizeof *s_ctx, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_lock || !s_done || !s_ctx) { heap_caps_free(plane); return ESP_ERR_NO_MEM; }
    }
    if (s_job) { heap_caps_free(plane); return ESP_ERR_INVALID_STATE; }
    fx_job_t *job = heap_caps_calloc(1, sizeof *job, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!job) { heap_caps_free(plane); return ESP_ERR_NO_MEM; }
    /* The mirror is wire order; the plane arrives little-endian. Swap once here,
     * off the LVGL task, so no painter ever swaps. */
    for (size_t i = 0, n = (size_t)w * h; i < n; i++) plane[i] = __builtin_bswap16(plane[i]);
    job->req   = *req;
    job->plane = plane;
    job->pw    = w;
    job->ph    = h;
    xSemaphoreTake(s_done, 0);                  /* no stale give answers us */
    s_job = job;

    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (xSemaphoreTake(s_done, 0) != pdTRUE) {
            job->abandoned = true;              /* the service frees it */
            xSemaphoreGive(s_lock);
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s_lock);
    }
    *out = job->rep;
    job_free(job);
    return ESP_OK;
}

#endif /* CONFIG_ORB_DEBUG_HTTP */
