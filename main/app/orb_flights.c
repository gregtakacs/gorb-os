/* Flight feed service — contract in orb_flights.h. */
#include "orb_flights.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "orb_https.h"
#include "orb_jobs.h"
#include "orb_net.h"
#include "orb_sched.h"
#include "orb_sd.h"
#include "orb_tls_mem.h"
#include "esp_app_desc.h"

static const char *TAG = "orb_flights";

/* Below orb_net/orb_dns (4), the jobs worker (3) and lvgl (5): the poll only
 * ever runs on time nobody drawing wanted. */
#define FLIGHTS_PRIO        2
#define FLIGHTS_STACK       8192      /* mbedTLS handshake + cert verify; PSRAM */
#define FLIGHTS_TIMEOUT_MS  8000
/* A card session's tail: the display refunds its ring up to 1.5 s after the
 * card lock drops (display.c RING_NET_WINDOW_US), so stay clear a bit longer. */
#define FLIGHTS_CARD_QUIET_MS 2000
/* The internal block a poll needs to start: a few lwIP pbufs and the radio's
 * TX buffers for the request. Set from the bench (§6), not guessed. */
#define FLIGHTS_MIN_LARGEST 1536

/* A 4xx is the provider saying no: rest it, starting at a minute and doubling
 * to a quarter hour, and ask the other one meanwhile. orb-os's record is the
 * reason: retrying a rate limit at full rate is the one response guaranteed to
 * make it worse, and the owner has seen adsb.lol's site 403 a page left open
 * until it was reloaded (2026-11-01). Transport failures do not rest: those
 * are the network's, and the next poll is the retry. */
#define REST_MIN_MS   60000u
#define REST_MAX_MS   900000u
static int64_t  s_rest_until_us[2];
static uint32_t s_rest_ms[2];

static bool resting(int src) { return esp_timer_get_time() < s_rest_until_us[src]; }

static void rest(int src)
{
    s_rest_ms[src] = s_rest_ms[src] ? s_rest_ms[src] * 2 : REST_MIN_MS;
    if (s_rest_ms[src] > REST_MAX_MS) s_rest_ms[src] = REST_MAX_MS;
    s_rest_until_us[src] = esp_timer_get_time() + (int64_t)s_rest_ms[src] * 1000;
    ESP_LOGW(TAG, "FLT provider %d refused; resting it %u s", src, (unsigned)(s_rest_ms[src] / 1000));
}

static const char *const s_origin[] = {
    [ORB_FLIGHTS_ADSB_LOL] = "https://api.adsb.lol",
    [ORB_FLIGHTS_ADSB_FI]  = "https://opendata.adsb.fi",
};

static orb_flights_cfg_t   s_cfg = {
    .enabled = false, .lat = 33.45, .lon = -112.07, .radius_nm = 40,
    .period_ms = 15000, .bg_period_ms = 60000, .visible = true, .boost = true,
    .hide_ground = false, .source = ORB_FLIGHTS_ADSB_LOL, .keep_alive = true,
};
static orb_flights_stats_t s_st;
static SemaphoreHandle_t   s_mux;
static TaskHandle_t        s_task;
static orb_https_t        *s_https;
static uint8_t             s_https_src = 0xFF;
static bool                s_https_ka;

/* Both tables in PSRAM: the working set the sink fills, and the published one. */
static orb_flight_t *s_work;
static int           s_work_n;
static orb_flight_t *s_pub;
static int           s_pub_n;
static int64_t       s_pub_us;

/* ---- the sink: nearest-N as the body streams past ----------------------- */

typedef struct {
    orb_readsb_t p;
    double lat0, lon0, coslat0;
    bool   hide_ground;
} poll_ctx_t;

static void emit(const orb_ac_t *a, void *ctx)
{
    const poll_ctx_t *c = ctx;
    if (c->hide_ground && a->on_ground) return;
    /* Equirectangular is exact enough inside a few hundred km, and the dial
     * cannot draw the difference. */
    const double dy = (a->lat - c->lat0) * 111.195;
    const double dx = (a->lon - c->lon0) * 111.195 * c->coslat0;
    const float d = (float)sqrt(dx * dx + dy * dy);
    int slot = -1;
    if (s_work_n < ORB_FLIGHTS_MAX) {
        slot = s_work_n++;
    } else {
        int far = 0;
        for (int i = 1; i < s_work_n; i++)
            if (s_work[i].dist_km > s_work[far].dist_km) far = i;
        if (d >= s_work[far].dist_km) return;
        slot = far;
    }
    s_work[slot].ac = *a;
    s_work[slot].dist_km = d;
    float b = (float)(atan2(dx, dy) * 180.0 / M_PI);
    s_work[slot].bearing_deg = b < 0 ? b + 360.0f : b;
}

/* Worth a faster background poll (§5): close, low and closing, or squawking. */
static bool interesting(const orb_flight_t *t, int n)
{
    for (int i = 0; i < n; i++) {
        const orb_ac_t *a = &t[i].ac;
        if (a->squawk == 7500 || a->squawk == 7600 || a->squawk == 7700) return true;
        if (a->on_ground) continue;
        if (t[i].dist_km < 9.26f) return true;                       /* 5 nm */
        if (a->has_alt && a->alt_ft < 3000 && t[i].dist_km < 18.52f && !isnan(a->track)) {
            /* Closing: its track points within 60 deg of the bearing back home. */
            float home = t[i].bearing_deg + 180.0f;
            float d = fmodf(fabsf(a->track - home), 360.0f);
            if (d > 180.0f) d = 360.0f - d;
            if (d < 60.0f) return true;
        }
    }
    return false;
}

static bool sink(const char *buf, size_t len, void *ctx)
{
    poll_ctx_t *c = ctx;
    return orb_readsb_feed(&c->p, buf, len);
}

/* ---- one poll ------------------------------------------------------------ */

/* Bench: the internal pool at each stage of a poll, and what mbedTLS holds of it. */
static void stage(const char *what)
{
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    char pend[96] = "";
    size_t at = 0;
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        int n = 0;
        if (lwip_ioctl(fd, FIONREAD, &n) == 0 && n > 0 && at < sizeof pend - 16)
            at += (size_t)snprintf(pend + at, sizeof pend - at, " fd%d=%d", fd, n);
    }
    ESP_LOGI(TAG, "FLT heap %-8s int_free=%u largest=%u blocks=%u tls_int=%d pending:%s", what,
             (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
             (unsigned)hi.allocated_blocks, (int)orb_tls_mem_internal_live(), at ? pend : " none");
}

/* Bench: which internal blocks a fresh connection leaves live. Walk the pool
 * before and after, and print the new blocks by size: sizes name their owners. */
/* Off by default: the walk holds the heap lock while it runs. Found the 15.5 KB
 * of mbedTLS state the allocator was not seeing (orb_tls_mem.c); set to 1 to
 * re-price a new connection. */
#define FLIGHTS_BLOCK_DIFF 0
typedef struct { uintptr_t p; size_t n; } blk_t;
#define BLK_MAX 1024
static blk_t *s_blk0;
static int    s_blk0_n;
typedef struct { blk_t *b; int n; } walk_t;

static bool walk_cb(walker_heap_into_t h, walker_block_info_t b, void *u)
{
    (void)h;
    walk_t *w = u;
    if (b.used && w->n < BLK_MAX) w->b[w->n++] = (blk_t){ (uintptr_t)b.ptr, b.size };
    return true;
}

static int blk_walk(blk_t *into)
{
    walk_t w = { into, 0 };
    heap_caps_walk(MALLOC_CAP_INTERNAL, walk_cb, &w);
    return w.n;
}

static void blk_diff(void)
{
    if (!s_blk0) return;
    blk_t *now = heap_caps_malloc(BLK_MAX * sizeof *now, MALLOC_CAP_SPIRAM);
    if (!now) return;
    const int n = blk_walk(now);
    struct { size_t sz; int cnt; } hist[48];
    int nh = 0, newn = 0;
    size_t newb = 0;
    for (int i = 0; i < n; i++) {
        bool seen = false;
        for (int j = 0; j < s_blk0_n && !seen; j++)
            seen = s_blk0[j].p == now[i].p && s_blk0[j].n == now[i].n;
        if (seen) continue;
        newn++; newb += now[i].n;
        int k = 0;
        while (k < nh && hist[k].sz != now[i].n) k++;
        if (k == nh && nh < 48) { hist[nh].sz = now[i].n; hist[nh].cnt = 0; nh++; }
        if (k < nh) hist[k].cnt++;
    }
    char line[400];
    size_t at = 0;
    for (int k = 0; k < nh && at < sizeof line - 16; k++)
        at += (size_t)snprintf(line + at, sizeof line - at, " %ux%d", (unsigned)hist[k].sz, hist[k].cnt);
    ESP_LOGI(TAG, "FLT newblocks %d (%u B):%s", newn, (unsigned)newb, line);
    heap_caps_free(now);
    heap_caps_free(s_blk0);
    s_blk0 = NULL;
}

static const char *gate(void)
{
    if (!orb_net_is_up()) { s_st.skip_net++; return "no network"; }
    if (orb_jobs_busy()) { s_st.skip_jobs++; return "a job is running"; }
    if (orb_sd_lock_idle_ms() < FLIGHTS_CARD_QUIET_MS) { s_st.skip_card++; return "the card is busy"; }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < FLIGHTS_MIN_LARGEST) {
        s_st.skip_heap++;
        return "internal heap is thin";
    }
    return NULL;
}

static void poll_once(const orb_flights_cfg_t *cfg)
{
    s_st.polls++;
    const char *why = gate();
    if (why) {
        ESP_LOGI(TAG, "FLT skip: %s", why);
        return;
    }
    int src = cfg->source;
    if (resting(src)) {
        if (resting(!src)) {
            s_st.skip_rest++;
            ESP_LOGI(TAG, "FLT skip: both providers resting");
            return;
        }
        src = !src;
        s_st.failovers++;
    }
    if (!s_https || s_https_src != src || s_https_ka != cfg->keep_alive) {
        stage("pre-free");
        orb_https_free(s_https);
        stage("pre-new");
        char ua[64];
        snprintf(ua, sizeof ua, "gorb-os/%s (flight tracker)", esp_app_get_description()->version);
        s_https = orb_https_new(s_origin[src], ua,
                                cfg->keep_alive, FLIGHTS_TIMEOUT_MS);
        s_https_src = (uint8_t)src;
        s_https_ka = cfg->keep_alive;
        if (!s_https) { s_st.fail++; ESP_LOGW(TAG, "FLT client alloc failed"); return; }
        stage("post-new");
#if FLIGHTS_BLOCK_DIFF
        s_blk0 = heap_caps_malloc(BLK_MAX * sizeof *s_blk0, MALLOC_CAP_SPIRAM);
        if (s_blk0) s_blk0_n = blk_walk(s_blk0);
#endif
    }
    char path[96];
    if (src == ORB_FLIGHTS_ADSB_FI)
        snprintf(path, sizeof path, "/api/v2/lat/%.4f/lon/%.4f/dist/%u", cfg->lat, cfg->lon,
                 (unsigned)cfg->radius_nm);
    else
        snprintf(path, sizeof path, "/v2/point/%.4f/%.4f/%u", cfg->lat, cfg->lon,
                 (unsigned)cfg->radius_nm);

    poll_ctx_t *c = heap_caps_calloc(1, sizeof *c, MALLOC_CAP_SPIRAM);
    if (!c) { s_st.fail++; return; }
    c->lat0 = cfg->lat;
    c->lon0 = cfg->lon;
    c->coslat0 = cos(cfg->lat * M_PI / 180.0);
    c->hide_ground = cfg->hide_ground;
    orb_readsb_init(&c->p, emit, c);
    s_work_n = 0;

    orb_sched_jank_t j0, j1;
    orb_sched_jank_snapshot(&j0);
    orb_https_result_t r;
    const esp_err_t err = orb_https_get(s_https, path, sink, c, &r);
    orb_sched_jank_snapshot(&j1);
    stage("post-get");
    blk_diff();
    const bool good = err == ESP_OK && r.status == 200 && orb_readsb_done(&c->p);

    s_st.opens = orb_https_opens(s_https);
    s_st.last_status = r.status;
    s_st.last_err = err;
    s_st.last_open_ms = r.open_ms;
    s_st.last_total_ms = r.total_ms;
    if (r.total_ms > s_st.max_total_ms) s_st.max_total_ms = r.total_ms;
    s_st.last_bytes = (uint32_t)r.bytes;
    s_st.last_objects = c->p.objects;
    if (r.int_free_min < s_st.poll_int_free_min || !s_st.poll_int_free_min)
        s_st.poll_int_free_min = r.int_free_min;
    if (r.int_largest_min < s_st.poll_int_largest_min || !s_st.poll_int_largest_min)
        s_st.poll_int_largest_min = r.int_largest_min;

    s_st.last_src = (uint8_t)src;
    if (r.status >= 400 && r.status < 500) {
        s_st.refused++;
        rest(src);
    }
    if (good) {
        s_rest_ms[src] = 0;                  /* forgiven: the next refusal starts at a minute */
        s_st.ok++;
        s_st.last_kept = (uint32_t)s_work_n;
        s_st.last_ok_us = esp_timer_get_time();
        xSemaphoreTake(s_mux, portMAX_DELAY);
        orb_flight_t *t = s_pub; s_pub = s_work; s_work = t;   /* swap, no copy */
        s_pub_n = s_work_n;
        s_pub_us = s_st.last_ok_us;
        s_st.seq++;
        s_st.interesting = interesting(s_pub, s_pub_n);
        xSemaphoreGive(s_mux);
    } else {
        s_st.fail++;
    }
    /* One line per poll: the feed's cost and the dial's beat during it. */
    ESP_LOGI(TAG, "FLT %s %d %s open=%u ttfb=%u total=%u B=%u ac=%u kept=%u "
                  "int_min=%u lg_min=%u | dial ticks=%u >1.5x=%u >2x=%u >3x=%u max=%u rung=%u",
             good ? "ok" : "FAIL", r.status, r.reused ? "reused" : "NEW",
             (unsigned)r.open_ms, (unsigned)r.ttfb_ms, (unsigned)r.total_ms, (unsigned)r.bytes,
             (unsigned)c->p.objects, (unsigned)s_work_n,
             (unsigned)r.int_free_min, (unsigned)r.int_largest_min,
             (unsigned)(j1.ticks - j0.ticks),
             (unsigned)((j1.le200 - j0.le200) + (j1.le300 - j0.le300) + (j1.gt300 - j0.gt300)),
             (unsigned)((j1.le300 - j0.le300) + (j1.gt300 - j0.gt300)),
             (unsigned)(j1.gt300 - j0.gt300), (unsigned)j1.max_gap_ms, (unsigned)j1.rung_fps);
    if (!good && err != ESP_OK)
        ESP_LOGW(TAG, "FLT error %s, parser done=%d error=%d", esp_err_to_name(err),
                 orb_readsb_done(&c->p), c->p.error);
    heap_caps_free(c);
}

/* The wait before the next poll: the visible rate, the background rate, or the
 * boost while the last picture held something worth watching. 0 = none. */
static uint32_t next_wait_ms(const orb_flights_cfg_t *cfg)
{
    if (cfg->visible) return cfg->period_ms;
    if (!cfg->bg_period_ms) return 0;
    if (cfg->boost && s_st.interesting && ORB_FLIGHTS_BOOST_MS < cfg->bg_period_ms)
        return ORB_FLIGHTS_BOOST_MS;
    return cfg->bg_period_ms;
}

static void flights_task(void *arg)
{
    (void)arg;
    orb_tls_mem_psram_for_me();          /* this task's TLS sessions live in PSRAM */
    for (;;) {
        orb_flights_cfg_t cfg;
        orb_flights_get_cfg(&cfg);
        const bool polling = cfg.enabled && (cfg.visible || cfg.bg_period_ms);
        if (!polling) {
            if (s_https) {
                stage("pre-free");
                orb_https_free(s_https); s_https = NULL; s_https_src = 0xFF;
                stage("off");
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        poll_once(&cfg);
        orb_flights_get_cfg(&cfg);       /* the wait follows the cfg as it is NOW */
        const uint32_t w = next_wait_ms(&cfg);
        ulTaskNotifyTake(pdTRUE, w ? pdMS_TO_TICKS(w) : portMAX_DELAY);
    }
}

/* ---- API ------------------------------------------------------------------ */

/* Nothing at boot: the task, its PSRAM stack and its tables are made on the
 * first enable. The boot ladder bottoms out the internal pool at wifi_start
 * (orb_net's [heap] lines), and a feed nobody has turned on must not be in
 * that queue — measured, a mutex there was the difference for the serial door. */
esp_err_t orb_flights_init(void)
{
    return ESP_OK;
}

static esp_err_t start(void)
{
    if (s_task) return ESP_OK;
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (!s_work) s_work = heap_caps_calloc(ORB_FLIGHTS_MAX, sizeof *s_work, MALLOC_CAP_SPIRAM);
    if (!s_pub) s_pub = heap_caps_calloc(ORB_FLIGHTS_MAX, sizeof *s_pub, MALLOC_CAP_SPIRAM);
    if (!s_mux || !s_work || !s_pub) return ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(flights_task, "orb_flights", FLIGHTS_STACK, NULL, FLIGHTS_PRIO,
                            &s_task, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void orb_flights_get_cfg(orb_flights_cfg_t *out)
{
    if (!s_mux) { *out = s_cfg; return; }        /* not started: no writer yet */
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mux);
}

esp_err_t orb_flights_set_cfg(const orb_flights_cfg_t *cfg)
{
    if (cfg->radius_nm < 1 || cfg->radius_nm > 250 || cfg->period_ms < 2000 ||
        (cfg->bg_period_ms && cfg->bg_period_ms < 2000) ||
        cfg->source > ORB_FLIGHTS_ADSB_FI || fabs(cfg->lat) > 90 || fabs(cfg->lon) > 180)
        return ESP_ERR_INVALID_ARG;
    if (!s_task && !cfg->enabled) { s_cfg = *cfg; return ESP_OK; }
    const esp_err_t err = start();
    if (err != ESP_OK) return err;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg = *cfg;
    xSemaphoreGive(s_mux);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void orb_flights_stats(orb_flights_stats_t *out) { *out = s_st; }

int orb_flights_snapshot(orb_flight_t *out, int max, uint32_t *age_ms, uint32_t *seq)
{
    if (seq) *seq = s_st.seq;
    if (!s_mux) { if (age_ms) *age_ms = UINT32_MAX; return 0; }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    const int n = s_pub_n < max ? s_pub_n : max;
    if (out && n > 0) memcpy(out, s_pub, (size_t)n * sizeof *out);
    if (age_ms) *age_ms = s_pub_us ? (uint32_t)((esp_timer_get_time() - s_pub_us) / 1000) : UINT32_MAX;
    xSemaphoreGive(s_mux);
    return n;
}

int orb_flights_json(char *buf, size_t cap)
{
    orb_flights_cfg_t cfg;
    orb_flights_get_cfg(&cfg);
    const orb_flights_stats_t st = s_st;
    orb_sched_jank_t j;
    orb_sched_jank_snapshot(&j);
    int n = snprintf(buf, cap,
        "{\"cfg\":{\"on\":%d,\"lat\":%.4f,\"lon\":%.4f,\"nm\":%u,\"period_ms\":%u,\"bg_ms\":%u,\"vis\":%d,\"boost\":%d,\"hg\":%d,\"src\":%u,\"ka\":%d},"
        "\"stats\":{\"polls\":%u,\"ok\":%u,\"fail\":%u,\"skip\":{\"net\":%u,\"jobs\":%u,\"card\":%u,\"heap\":%u,\"rest\":%u},"
        "\"refused\":%u,\"failovers\":%u,\"src\":%u,\"seq\":%u,\"interesting\":%d,"
        "\"opens\":%u,\"status\":%d,\"err\":%d,\"open_ms\":%u,\"total_ms\":%u,\"max_total_ms\":%u,"
        "\"bytes\":%u,\"objects\":%u,\"kept\":%u,\"int_free_min\":%u,\"int_largest_min\":%u},"
        "\"heap\":{\"tls_int\":%d,\"int_free\":%u,\"int_largest\":%u,\"int_lowwater\":%u,\"psram_free\":%u},"
        "\"jank\":{\"ticks\":%u,\"le125\":%u,\"le150\":%u,\"le200\":%u,\"le300\":%u,\"gt300\":%u,"
        "\"max_gap_ms\":%u,\"period_ms\":%u,\"rung\":%u,\"rung_moves\":%u,\"burst_ignored\":%u},\"ac\":[",
        cfg.enabled, cfg.lat, cfg.lon, (unsigned)cfg.radius_nm, (unsigned)cfg.period_ms,
        (unsigned)cfg.bg_period_ms, cfg.visible, cfg.boost, cfg.hide_ground,
        (unsigned)cfg.source, cfg.keep_alive,
        (unsigned)st.polls, (unsigned)st.ok, (unsigned)st.fail, (unsigned)st.skip_net,
        (unsigned)st.skip_jobs, (unsigned)st.skip_card, (unsigned)st.skip_heap,
        (unsigned)st.skip_rest, (unsigned)st.refused, (unsigned)st.failovers, (unsigned)st.last_src,
        (unsigned)st.seq, st.interesting,
        (unsigned)st.opens, st.last_status, st.last_err, (unsigned)st.last_open_ms,
        (unsigned)st.last_total_ms, (unsigned)st.max_total_ms, (unsigned)st.last_bytes,
        (unsigned)st.last_objects, (unsigned)st.last_kept, (unsigned)st.poll_int_free_min,
        (unsigned)st.poll_int_largest_min,
        (int)orb_tls_mem_internal_live(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)j.ticks, (unsigned)j.le125, (unsigned)j.le150, (unsigned)j.le200,
        (unsigned)j.le300, (unsigned)j.gt300, (unsigned)j.max_gap_ms, (unsigned)j.period_ms,
        (unsigned)j.rung_fps, (unsigned)j.rung_moves, (unsigned)orb_sched_bg_bursts_ignored());
    if (n < 0 || (size_t)n >= cap) return -1;
    if (!s_mux) return n + snprintf(buf + n, cap - (size_t)n, "]}");
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int i = 0; i < s_pub_n; i++) {
        const orb_flight_t *f = &s_pub[i];
        const int m = snprintf(buf + n, cap - (size_t)n,
            "%s{\"hex\":\"%s\",\"cs\":\"%s\",\"t\":\"%s\",\"km\":%.1f,\"brg\":%.0f,\"alt\":%d,"
            "\"gnd\":%d,\"trk\":%.0f,\"gs\":%.0f}",
            i ? "," : "", f->ac.hex, f->ac.flight, f->ac.type, f->dist_km, f->bearing_deg,
            (int)f->ac.alt_ft, f->ac.on_ground, isnan(f->ac.track) ? -1.0f : f->ac.track,
            isnan(f->ac.gs) ? -1.0f : f->ac.gs);
        if (m < 0 || (size_t)(n + m) >= cap - 4) break;
        n += m;
    }
    xSemaphoreGive(s_mux);
    n += snprintf(buf + n, cap - (size_t)n, "]}");
    return n;
}
