/* The OS's maps — contract in orb_map.h.
 *
 * Threads: the app's calls run on the LVGL task; renders run on the map task. A view's
 * raster is the map task's while its state reads RENDERING and the app's once it reads
 * READY; the app shows the other view meanwhile. */
#include "orb_map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "orb_app.h"          /* ORB_APP_CAP */
#include "orb_home.h"
#include "orb_map_raster.h"
#include "orb_places.h"
#include "orb_sd.h"
#include "display.h"   /* display_ring_urgent_for */
#include "orb_glass.h"   /* §6.21: rasters and scratch in the glass region */

static const char *TAG = "map";

#define WRITE_QUIET_MS 1000             /* stand aside while the card was written this recently */
#define SCRATCH_B     (256 * 1024)      /* one fill's edges: DFW's largest needs ~220 KB */
#define LINGER_S      (15 * 60)
#define TASK_STACK    6144
#define TASK_PRIO     2                 /* under the LVGL task: a render never delays a frame */

typedef struct {
    volatile int32_t state;             /* ORB_MAP_* */
    uint32_t seq;                       /* bumps on every open, so a stale render is dropped */
    int      place;
    orb_map_req_t req;
    int      z;
    float    scale, rot;
    int32_t  c_lat_e6, c_lon_e6;        /* the file's centre */
    uint8_t *raster;
    orb_map_markrec_t marks[ORB_MAP_MARKS_MAX];
    uint8_t  n_marks;
    int64_t  opened_us;
} view_t;

typedef struct {
    view_t   v[ORB_MAP_VIEWS];
    int64_t  off_since_us;
} app_maps_t;

EXT_RAM_BSS_ATTR static app_maps_t s_a[ORB_APP_CAP];
/* Held by the map task for a whole render. Freeing an app's rasters or files takes it
 * first, so a render never reads freed memory; the LVGL task only ever tries it. */
static SemaphoreHandle_t s_busy;
static StaticSemaphore_t s_busy_buf;
static QueueHandle_t s_q;
static volatile uint32_t s_gen = 1;
static uint32_t s_renders, s_last_ms;

typedef struct { uint8_t app, view; uint32_t seq; } job_t;


static app_maps_t *app_of(int s) { return s >= 0 && s < ORB_APP_CAP ? &s_a[s] : NULL; }

/* ---- names and places ------------------------------------------------------------- */

static int32_t e4(int32_t e6) { return (e6 >= 0 ? e6 + 50 : e6 - 50) / 100; }

void orb_map_file_name(int place, int32_t lat_e6, int32_t lon_e6, int z, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%d-%ld_%ld-z%d-f2.orbmap", place, (long)e4(lat_e6), (long)e4(lon_e6), z);
}

static bool place_at(int place, int32_t *lat, int32_t *lon)
{
    if (place == 0) {
        orb_home_t h;
        if (!orb_home_get(&h)) return false;
        *lat = h.lat_e6; *lon = h.lon_e6;
        return true;
    }
    orb_place_rec_t p;
    if (place < 1 || place > ORB_PLACES_N || !orb_places_get(place - 1, &p)) return false;
    *lat = p.lat_e6; *lon = p.lon_e6;
    return true;
}

/* The level whose own scale is nearest the view's: it then draws at 0.71x to 1.41x. */
static void pick_level(int32_t lat_e6, int32_t radius_m, int *z, float *scale)
{
    const double c = cos(lat_e6 / 1e6 * M_PI / 180.0);
    const double want = (double)radius_m / (ORB_MAP_W / 2);                 /* m per glass px */
    int zz = (int)lround(log2(156543.03392 * c / want));
    if (zz < 6) zz = 6;
    if (zz > 11) zz = 11;
    *z = zz;
    *scale = (float)(156543.03392 * c / ldexp(1.0, zz) / want);            /* glass px per file px */
}

/* ---- the card --------------------------------------------------------------------- */

/* A level file read in pieces from the card (§6.20), for orb_map_render_src: no PSRAM
 * block as big as the file. Each piece takes the card lock alone, as read_file's do. */
typedef struct { char full[112]; } cardsrc_t;
static int card_read(void *ctx, uint32_t off, uint8_t *dst, uint32_t n)
{
    const cardsrc_t *c = ctx;
    while (orb_sd_write_idle_ms() < WRITE_QUIET_MS) vTaskDelay(pdMS_TO_TICKS(50));
    orb_sd_lock();
    FILE *f = fopen(c->full, "rb");
    const bool got = f && fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
    if (f) fclose(f);
    orb_sd_unlock();
    return got ? (int)n : -1;
}
#define STREAM_WIN (16 * 1024)

/* ---- the render ------------------------------------------------------------------- */

typedef struct { view_t *v; } markctx_t;

static void on_mark(void *ctx, uint8_t kind, uint8_t rank, float x, float y, const char *name, uint8_t n)
{
    view_t *v = ((markctx_t *)ctx)->v;
    if (v->n_marks >= ORB_MAP_MARKS_MAX) return;
    if (x < -64 || x > ORB_MAP_W + 64 || y < -64 || y > ORB_MAP_W + 64) return;
    orb_map_markrec_t *m = &v->marks[v->n_marks++];
    m->x = (int16_t)lroundf(x);
    m->y = (int16_t)lroundf(y);
    m->kind = kind;
    m->rank = rank;
    const uint8_t k = n < sizeof m->name ? n : sizeof m->name - 1;
    memcpy(m->name, name, k);
    m->name[k] = 0;
}

static void render(int app_slot, int vi, uint32_t seq, void *scratch)
{
    app_maps_t *a = app_of(app_slot);
    view_t *v = &a->v[vi];
    int32_t lat, lon;
    if (!place_at(v->place, &lat, &lon)) { v->state = ORB_MAP_ABSENT; return; }
    pick_level(lat, v->req.radius_m, &v->z, &v->scale);
    char name[64];
    orb_map_file_name(v->place, lat, lon, v->z, name, sizeof name);
    const int64_t t0 = esp_timer_get_time();
    /* Every level file is read from the card as it is drawn, never held (§6.21: a 36-320 KB
     * file cached per zoom and place was the big block that came and went; Gold, 2026-11-08,
     * refused a 312 KB one with 919 KB free). */
    if (!orb_sd_mounted() || orb_sd_usb_owned()) {
        if (v->seq == seq) v->state = ORB_MAP_NOCARD;
        return;
    }
    if (!v->raster) v->raster = orb_glass_alloc(ORB_MAP_RASTER_B);   /* §6.21 */
    if (!v->raster) { v->state = ORB_MAP_NOMEM; return; }
    cardsrc_t cs;
    snprintf(cs.full, sizeof cs.full, ORB_SD_BASE ORB_MAP_DIR "/%s", name);
    struct stat stt;
    orb_sd_lock();
    const bool there = stat(cs.full, &stt) == 0;
    orb_sd_unlock();
    uint8_t *win = there ? heap_caps_malloc(STREAM_WIN, MALLOC_CAP_SPIRAM) : NULL;
    if (!win) {
        if (v->seq == seq) v->state = there ? ORB_MAP_NOMEM : ORB_MAP_ABSENT;
        ESP_LOGW(TAG, "slot %d view %d: %s %s", app_slot, vi + 1, name, there ? "(no window)" : "is not on the card");
        return;
    }
    const orb_map_src_t src = { card_read, &cs, (uint32_t)stt.st_size };
    uint8_t buf[36];
    if (card_read(&cs, 0, buf, sizeof buf) != (int)sizeof buf) {
        heap_caps_free(win);
        if (v->seq == seq) v->state = ORB_MAP_ABSENT;
        return;
    }
    v->c_lat_e6 = (int32_t)(buf[8] | buf[9] << 8 | buf[10] << 16 | (uint32_t)buf[11] << 24);
    v->c_lon_e6 = (int32_t)(buf[12] | buf[13] << 8 | buf[14] << 16 | (uint32_t)buf[15] << 24);
    v->rot = v->req.rot_cdeg / 100.0f * (float)M_PI / 180.0f;
    v->n_marks = 0;
    markctx_t mc = { v };
    orb_map_style_t st = { .scale = v->scale, .rot_rad = v->rot, .role_on = v->req.role_on,
                           .mark_on = v->req.mark_on, .mark = on_mark, .mark_ctx = &mc };
    memcpy(st.width, v->req.width, 16);
    memcpy(st.dash, v->req.dash, 16);
    orb_map_stats_t stats;
    const int rr = orb_map_render_src(&src, buf, win, STREAM_WIN, &st, v->raster, scratch, SCRATCH_B, &stats);
    heap_caps_free(win);
    s_last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s_renders++;
    if (v->seq != seq) return;                          /* re-opened meanwhile: that render wins */
    v->state = rr == ORB_MAP_OK ? ORB_MAP_READY : ORB_MAP_BAD;
    ESP_LOGI(TAG, "slot %d view %d: %s at %.2fx, %lu ms, %lu marks%s%s", app_slot, vi + 1, name, v->scale,
             (unsigned long)s_last_ms, (unsigned long)v->n_marks, stats.skipped ? ", a fill skipped" : "",
             rr == ORB_MAP_OK ? "" : rr == ORB_MAP_E_CRC ? ", its CRC failed" : ", the file is damaged");
}

static void map_task(void *arg)
{
    (void)arg;
    void *scratch = NULL;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, scratch ? pdMS_TO_TICKS(30000) : portMAX_DELAY) != pdTRUE) {
            orb_glass_free(scratch);                    /* idle 30 s: give the scratch back */
            scratch = NULL;
            continue;
        }
        app_maps_t *a = app_of(j.app);
        if (!a || a->v[j.view].seq != j.seq) continue;  /* closed or re-opened since */
        if (!scratch) scratch = orb_glass_alloc(SCRATCH_B);   /* §6.21: the glass region */
        if (!scratch) { a->v[j.view].state = ORB_MAP_NOMEM; continue; }
        xSemaphoreTake(s_busy, portMAX_DELAY);
        render(j.app, j.view, j.seq, scratch);
        xSemaphoreGive(s_busy);
        if (!uxQueueMessagesWaiting(s_q)) { orb_glass_free(scratch); scratch = NULL; }   /* no render waits */
    }
}

static bool ensure_started(void)
{
    if (s_q) return true;
    s_busy = xSemaphoreCreateMutexStatic(&s_busy_buf);
    s_q = xQueueCreate(8, sizeof(job_t));
    if (!s_q) return false;
    /* The stack in PSRAM, as appnet's: the task reads the card and renders, and never
     * touches flash. In internal RAM its 6 KB took the block the Wi-Fi driver's
     * transmit buffers come from (Bronze under the clock, 2026-11-07: 23 KB free, the
     * largest block 7.4 KB), and once it stood, any reply longer than a segment
     * stopped after its first send. */
    StackType_t *stk = heap_caps_malloc(TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = heap_caps_calloc(1, sizeof *tcb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stk || !tcb ||
        !xTaskCreateStaticPinnedToCore(map_task, "map", TASK_STACK, NULL, TASK_PRIO, stk, tcb, tskNO_AFFINITY)) {
        heap_caps_free(stk);
        heap_caps_free(tcb);
        vQueueDelete(s_q);
        s_q = NULL;
        return false;
    }
    return true;
}

/* ---- the app's side (the LVGL task) ------------------------------------------------- */

int32_t orb_map_open(int app_slot, int place, const orb_map_req_t *req)
{
    app_maps_t *a = app_of(app_slot);
    if (!a || !req || place < 0 || place > ORB_PLACES_N || req->radius_m < 1000 || req->radius_m > 2000000)
        return ORB_MAP_ABSENT;
    if (!ensure_started()) return ORB_MAP_NOMEM;
    int vi = -1;
    for (int i = 0; i < ORB_MAP_VIEWS && vi < 0; i++) if (!a->v[i].opened_us) vi = i;
    if (vi < 0) vi = a->v[0].opened_us <= a->v[1].opened_us ? 0 : 1;   /* the oldest */
    view_t *v = &a->v[vi];
    v->seq++;
    v->place = place;
    v->req = *req;
    v->state = ORB_MAP_RENDERING;
    v->opened_us = esp_timer_get_time();
    const job_t j = { (uint8_t)app_slot, (uint8_t)vi, v->seq };
    if (xQueueSend(s_q, &j, 0) != pdTRUE) { v->state = ORB_MAP_NOMEM; return ORB_MAP_NOMEM; }
    return vi + 1;
}

static view_t *view_of(int app_slot, int32_t h)
{
    app_maps_t *a = app_of(app_slot);
    return a && h >= 1 && h <= ORB_MAP_VIEWS && a->v[h - 1].opened_us ? &a->v[h - 1] : NULL;
}

int32_t orb_map_ready(int app_slot, int32_t h)
{
    view_t *v = view_of(app_slot, h);
    return v ? v->state : ORB_MAP_ABSENT;
}

const uint8_t *orb_map_raster_of(int app_slot, int32_t h)
{
    view_t *v = view_of(app_slot, h);
    return v && v->state == ORB_MAP_READY ? v->raster : NULL;
}

int32_t orb_map_mark(int app_slot, int32_t h, int i, orb_map_markrec_t *out)
{
    view_t *v = view_of(app_slot, h);
    if (!v || v->state != ORB_MAP_READY || i < 0 || i >= v->n_marks) return ORB_MAP_ABSENT;
    *out = v->marks[i];
    return ORB_MAP_READY;
}

int32_t orb_map_project(int app_slot, int32_t h, int32_t lat_e6, int32_t lon_e6, int32_t *x, int32_t *y)
{
    view_t *v = view_of(app_slot, h);
    if (!v || v->state != ORB_MAP_READY) return ORB_MAP_ABSENT;
    /* Web Mercator pixels at the file's zoom, relative to its centre, then the view's
     * scale and rotation: the renderer's transform, so a point lands on its map pixel. */
    const double n = 256.0 * ldexp(1.0, v->z);
    const double s1 = sin(lat_e6 / 1e6 * M_PI / 180.0), s0 = sin(v->c_lat_e6 / 1e6 * M_PI / 180.0);
    const double dx = (lon_e6 - v->c_lon_e6) / 1e6 / 360.0 * n;
    const double dy = (log((1 + s0) / (1 - s0)) - log((1 + s1) / (1 - s1))) / (4 * M_PI) * n;
    const double cs = cos(v->rot), sn = sin(v->rot);
    *x = (int32_t)lround(ORB_MAP_W / 2.0 + v->scale * (cs * dx - sn * dy));
    *y = (int32_t)lround(ORB_MAP_W / 2.0 + v->scale * (sn * dx + cs * dy));
    return ORB_MAP_READY;
}

void orb_map_close(int app_slot, int32_t h)
{
    view_t *v = view_of(app_slot, h);
    if (!v) return;
    v->seq++;                                           /* a render in flight is dropped */
    v->opened_us = 0;
    v->state = ORB_MAP_ABSENT;
    /* The raster stays allocated: the map task may be writing it; it is reused, and freed
     * with the app's other maps. */
}

static void free_app(app_maps_t *a)
{
    for (int i = 0; i < ORB_MAP_VIEWS; i++) {
        a->v[i].seq++;
        a->v[i].opened_us = 0;
        a->v[i].state = ORB_MAP_ABSENT;
    }
    for (int i = 0; i < ORB_MAP_VIEWS; i++) { orb_glass_free(a->v[i].raster); a->v[i].raster = NULL; }
    a->off_since_us = 0;
}

void orb_map_release_app(int app_slot)
{
    app_maps_t *a = app_of(app_slot);
    if (!a) return;
    /* Rare (uninstall, a release): wait out a render that may be using this app's memory. */
    if (s_busy) xSemaphoreTake(s_busy, portMAX_DELAY);
    free_app(a);
    if (s_busy) xSemaphoreGive(s_busy);
}

void orb_map_drop(int app_slot)
{
    app_maps_t *a = app_of(app_slot);
    if (!a) return;
    if (s_busy && xSemaphoreTake(s_busy, 0) != pdTRUE) return;   /* a render is running: next pass */
    free_app(a);
    a->off_since_us = 0;
    if (s_busy) xSemaphoreGive(s_busy);
}

void orb_map_note_glass(int app_slot, bool on_glass, int64_t now_us)
{
    app_maps_t *a = app_of(app_slot);
    if (!a) return;
    /* A map the glass waits for reads the card in 32 KB pieces, and each read lends the
     * display's ring (orb_sd_lock): the glass then dropped every frame until 1.5 s after
     * the read, so a press's new place showed nothing for seconds (owner, 2026-11-08). The
     * ring comes back as each piece ends while the app on the glass waits for a render. */
    static int s_urgent_slot = -1;
    if (on_glass) {
        bool waiting = false;
        for (int i = 0; i < ORB_MAP_VIEWS; i++) waiting |= a->v[i].opened_us && a->v[i].state == ORB_MAP_RENDERING;
        display_ring_urgent_for(DISPLAY_URGENT_MAP, waiting);
        s_urgent_slot = waiting ? app_slot : -1;
    } else if (s_urgent_slot == app_slot) {
        display_ring_urgent_for(DISPLAY_URGENT_MAP, false);
        s_urgent_slot = -1;
    }
    /* Off the glass an app keeps no display buffers (owner, 2026-11-07): its views and their
     * rasters go at once, and it opens new ones when it is back. The level files are data,
     * not pixels: they stay LINGER_S, so a return does not read the card again. */
    if (!on_glass && (a->v[0].raster || a->v[1].raster) &&
        (!s_busy || xSemaphoreTake(s_busy, 0) == pdTRUE)) {       /* a render runs: next pass */
        for (int i = 0; i < ORB_MAP_VIEWS; i++) {
            a->v[i].seq++;
            a->v[i].opened_us = 0;
            a->v[i].state = ORB_MAP_ABSENT;
            orb_glass_free(a->v[i].raster);
            a->v[i].raster = NULL;
        }
        if (s_busy) xSemaphoreGive(s_busy);
    }
    bool any = false;
    for (int i = 0; i < ORB_MAP_VIEWS; i++) any |= a->v[i].raster != NULL;
    if (!any || on_glass) { a->off_since_us = 0; return; }
    if (!a->off_since_us) { a->off_since_us = now_us; return; }
    if (now_us - a->off_since_us >= (int64_t)LINGER_S * 1000000) {
        if (s_busy && xSemaphoreTake(s_busy, 0) != pdTRUE) return;   /* a render is running: next pass */
        ESP_LOGI(TAG, "slot %d: maps freed after %d min off the glass", app_slot, LINGER_S / 60);
        free_app(a);
        if (s_busy) xSemaphoreGive(s_busy);
    }
}

uint32_t orb_maps_gen(void) { return s_gen; }

void orb_maps_bump(void)
{
    /* The files changed: every app re-opens its views on the gen, and reads them afresh
     * (nothing is cached: §6.21). A render in flight finishes with what it read. */
    s_gen++;
}

uint32_t orb_map_app_bytes(int app_slot)
{
    const app_maps_t *a = app_of(app_slot);
    if (!a) return 0;
    uint32_t b = 0;
    for (int i = 0; i < ORB_MAP_VIEWS; i++) if (a->v[i].raster) b += ORB_MAP_RASTER_B;
    return b;
}

void orb_map_stats(uint32_t *renders, uint32_t *last_ms, uint32_t *files, uint32_t *file_bytes)
{
    *renders = s_renders; *last_ms = s_last_ms; *files = 0; *file_bytes = 0;   /* none held (§6.21) */
}
