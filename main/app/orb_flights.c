/* Flight feed service — contract in orb_flights.h. */
#include "orb_flights.h"
#include "flights_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

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
 * are the network's, and the next poll is the retry.
 *
 * A 403 is not "busy", it is "banned" (api.adsb.lol answered exactly that,
 * 2026-11-01, after a day of bench probing on top of 15-20 s polls). Knocking
 * every quarter hour on a ban is the pattern that makes a temporary block
 * permanent, so a 403 rests the provider an hour, doubling to twelve. Any
 * good answer forgives either ladder. */
#define REST_MIN_MS   60000u
#define REST_MAX_MS   900000u
#define BAN_MIN_MS    3600000u
#define BAN_MAX_MS    43200000u

/* The next rest after a refusal with this status, from the last one. */
static uint32_t next_rest_ms(uint32_t last, int status)
{
    const bool ban = status == 403;
    const uint32_t lo = ban ? BAN_MIN_MS : REST_MIN_MS, hi = ban ? BAN_MAX_MS : REST_MAX_MS;
    uint32_t r = last ? last * 2 : lo;
    if (r < lo) r = lo;
    return r > hi ? hi : r;
}
static int64_t  s_rest_until_us[2];
static uint32_t s_rest_ms[2];

/* adsb.lol's budget (the owner's free key, 2026-11-01): 150 requests an hour,
 * 500 a day, 5,000 a month, and "repeated rate-limit hits can revoke a free
 * key". One request per 10 minutes stays under every window even if adsb.fi
 * were down all month (144 a day, ~4,300 a month), so adsb.lol is never
 * asked sooner, keyed or not, whatever the Source setting says: in between,
 * the other provider answers. */
#define LOL_GAP_MS    600000u
static int64_t  s_lol_next_us;

static bool resting(int src)
{
    const int64_t now = esp_timer_get_time();
    return now < s_rest_until_us[src] || (src == ORB_FLIGHTS_ADSB_LOL && now < s_lol_next_us);
}

/* Seconds until src may be asked again, 0 = now. */
static uint32_t rest_left_s(int src)
{
    int64_t until = s_rest_until_us[src];
    if (src == ORB_FLIGHTS_ADSB_LOL && s_lol_next_us > until) until = s_lol_next_us;
    const int64_t left = until - esp_timer_get_time();
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}

/* A 429's Retry-After is a floor: the rest is never shorter than asked. */
static void rest(int src, int status, uint32_t retry_after_s)
{
    s_rest_ms[src] = next_rest_ms(s_rest_ms[src], status);
    if (retry_after_s && (uint64_t)retry_after_s * 1000 > s_rest_ms[src])
        s_rest_ms[src] = retry_after_s > 86400u ? 86400000u : retry_after_s * 1000u;
    s_rest_until_us[src] = esp_timer_get_time() + (int64_t)s_rest_ms[src] * 1000;
    ESP_LOGW(TAG, "FLT provider %d refused (%d); resting it %u s", src, status,
             (unsigned)(s_rest_ms[src] / 1000));
}

static const char *const s_origin_real[] = {
    [ORB_FLIGHTS_ADSB_LOL] = "https://api.adsb.lol",
    [ORB_FLIGHTS_ADSB_FI]  = "https://opendata.adsb.fi",
};
static const char *s_origin[] = { "https://api.adsb.lol", "https://opendata.adsb.fi" };

/* Routes (§6.6): adsbdb.com, one GET per callsign, answers ~700 B. */
#define ROUTE_ORIGIN      "https://api.adsbdb.com"
#define ROUTE_TIMEOUT_MS  10000
#define ROUTE_BODY_MAX    2048
#define ROUTE_TRIES       2           /* transport failures before giving up on one */


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
static uint32_t            s_https_key_gen;   /* the key generation s_https was built with */

/* The adsb.lol key (orb_flights.h). PSRAM like the rest of this service's
 * state, read from NVS on first use rather than at boot (the boot ladder's
 * internal pool is the note above orb_flights_init), and guarded by a
 * spinlock: copies of at most 161 bytes, no allocation, nothing at boot. */
#define KEY_PART "storage"
#define KEY_NS   "orb.flights"
EXT_RAM_BSS_ATTR static char s_key[ORB_FLIGHTS_KEY_MAX + 1];
EXT_RAM_BSS_ATTR static char s_key_hdr[ORB_FLIGHTS_HDR_MAX + 1];
static uint32_t     s_key_gen;
static bool         s_key_loaded;
static portMUX_TYPE s_key_mux = portMUX_INITIALIZER_UNLOCKED;
static void key_load(void);
/* A wake from set_cfg means "poll now"; a wake from a route ask must not, or
 * stepping through cards would multiply the feed's request rate. */
static volatile bool       s_poll_kick;

/* The route cache: a ring of the last callsigns asked about. Written by the
 * task (answers) and the asker (new entries), both under s_mux. */
typedef struct {
    char              cs[9];
    uint8_t           state;      /* orb_route_state_t */
    uint8_t           tries;
    fm_route_t        r;
} route_slot_t;
static route_slot_t *s_routes;    /* PSRAM, ORB_FLIGHTS_ROUTES */
static int           s_route_next;
static orb_https_t  *s_route_https;
static int64_t       s_route_rest_until_us;
static uint32_t      s_route_rest_ms;
static struct { uint32_t asked, found, none, fail, refused; int last_status; uint32_t last_ms; } s_rst;

/* Bench: route provider 0 to an origin that refuses (airplanes.live answers 403
 * on adsb.lol's exact path shape), so the rest/failover ladder runs against a
 * real server. Reached only from the debug door; off restores the real one. */
void orb_flights_bench_refuse0(bool on)
{
    s_origin[0] = on ? "https://api.airplanes.live" : s_origin_real[0];
    s_https_src = 0xFF;                          /* drop the kept connection */
    s_rest_ms[0] = 0;
    s_rest_until_us[0] = 0;
}

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

/* Worth a faster background poll (§5): flights_model.c decides per contact. */
static bool interesting(const orb_flight_t *t, int n)
{
    for (int i = 0; i < n; i++)
        if (fm_interesting(&t[i].ac, t[i].dist_km, t[i].bearing_deg)) return true;
    return false;
}

static bool sink(const char *buf, size_t len, void *ctx)
{
    poll_ctx_t *c = ctx;
    return orb_readsb_feed(&c->p, buf, len);
}

/* ---- one poll ------------------------------------------------------------ */

/* count: a poll's skips are the feed's stats; a route lookup's are not. */
static const char *gate(bool count)
{
    if (!orb_net_is_up()) { if (count) s_st.skip_net++; return "no network"; }
    if (orb_jobs_busy()) { if (count) s_st.skip_jobs++; return "a job is running"; }
    if (orb_sd_lock_idle_ms() < FLIGHTS_CARD_QUIET_MS) { if (count) s_st.skip_card++; return "the card is busy"; }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < FLIGHTS_MIN_LARGEST) {
        if (count) s_st.skip_heap++;
        return "internal heap is thin";
    }
    return NULL;
}

static void poll_once(const orb_flights_cfg_t *cfg)
{
    s_st.polls++;
    const char *why = gate(true);
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
    if (!s_https || s_https_src != src || s_https_ka != cfg->keep_alive ||
        (src == ORB_FLIGHTS_ADSB_LOL && s_https_key_gen != s_key_gen)) {
        orb_https_free(s_https);
        char ua[64];
        snprintf(ua, sizeof ua, "gorb-os/%s (flight tracker)", esp_app_get_description()->version);
        s_https = orb_https_new(s_origin[src], ua,
                                cfg->keep_alive, FLIGHTS_TIMEOUT_MS);
        s_https_src = (uint8_t)src;
        s_https_ka = cfg->keep_alive;
        if (!s_https) { s_st.fail++; ESP_LOGW(TAG, "FLT client alloc failed"); return; }
        /* The key goes to adsb.lol and to nobody else. */
        if (src == ORB_FLIGHTS_ADSB_LOL) {
            key_load();
            EXT_RAM_BSS_ATTR static char k[ORB_FLIGHTS_KEY_MAX + 1], hd[ORB_FLIGHTS_HDR_MAX + 1];
            taskENTER_CRITICAL(&s_key_mux);
            memcpy(k, s_key, sizeof k);
            memcpy(hd, s_key_hdr, sizeof hd);
            s_https_key_gen = s_key_gen;
            taskEXIT_CRITICAL(&s_key_mux);
            if (k[0]) orb_https_set_header(s_https, hd, k);
        }
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
    /* Spent before the answer, so a request that hangs still counts. */
    if (src == ORB_FLIGHTS_ADSB_LOL) s_lol_next_us = esp_timer_get_time() + (int64_t)LOL_GAP_MS * 1000;
    const esp_err_t err = orb_https_get(s_https, path, sink, c, &r);
    orb_sched_jank_snapshot(&j1);
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
        rest(src, r.status, r.retry_after_s);
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

/* ---- one route lookup ------------------------------------------------------ */

typedef struct { char *buf; size_t n; bool over; } route_body_t;

static bool route_sink(const char *buf, size_t len, void *ctx)
{
    route_body_t *b = ctx;
    if (b->n + len >= ROUTE_BODY_MAX) { b->over = true; return false; }
    memcpy(b->buf + b->n, buf, len);
    b->n += len;
    return true;
}

/* The oldest pending callsign, copied out, or false. */
static bool route_pending(char cs[9])
{
    bool any = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int k = 0; k < ORB_FLIGHTS_ROUTES && !any; k++) {
        const route_slot_t *e = &s_routes[(s_route_next + k) % ORB_FLIGHTS_ROUTES];
        if (e->cs[0] && e->state == ORB_ROUTE_PENDING) { memcpy(cs, e->cs, 9); any = true; }
    }
    xSemaphoreGive(s_mux);
    return any;
}

static void route_answer(const char *cs, orb_route_state_t st, const fm_route_t *r, bool failed)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int k = 0; k < ORB_FLIGHTS_ROUTES; k++) {
        route_slot_t *e = &s_routes[k];
        if (strcmp(e->cs, cs)) continue;
        if (failed && ++e->tries < ROUTE_TRIES) break;          /* stays pending: try again */
        e->state = (uint8_t)st;
        if (r) e->r = *r;
        break;
    }
    xSemaphoreGive(s_mux);
}

/* One lookup, when one is waiting and the gate is open. A 404 is adsbdb not
 * knowing the callsign; another 4xx rests route lookups the way a refusal
 * rests a feed provider, and the callsign waits. */
static void route_once(void)
{
    char cs[9];
    if (!s_routes || !route_pending(cs)) return;
    if (esp_timer_get_time() < s_route_rest_until_us) return;
    const char *why = gate(false);
    if (why) { ESP_LOGD(TAG, "RTE skip: %s", why); return; }      /* retried every 200 ms */
    if (!s_route_https) {
        char ua[64];
        snprintf(ua, sizeof ua, "gorb-os/%s (flight tracker)", esp_app_get_description()->version);
        s_route_https = orb_https_new(ROUTE_ORIGIN, ua, true, ROUTE_TIMEOUT_MS);
        if (!s_route_https) { s_rst.fail++; route_answer(cs, ORB_ROUTE_NONE, NULL, true); return; }
    }
    route_body_t b = { .buf = heap_caps_malloc(ROUTE_BODY_MAX, MALLOC_CAP_SPIRAM) };
    if (!b.buf) { s_rst.fail++; return; }
    char path[32];
    snprintf(path, sizeof path, "/v0/callsign/%s", cs);
    s_rst.asked++;
    orb_https_result_t r;
    const esp_err_t err = orb_https_get(s_route_https, path, route_sink, &b, &r);
    b.buf[b.n] = 0;
    s_rst.last_status = r.status;
    s_rst.last_ms = r.total_ms;
    fm_route_t rt;
    if (err == ESP_OK && r.status == 200 && !b.over && fm_route_parse(b.buf, &rt)) {
        s_rst.found++;
        s_route_rest_ms = 0;
        route_answer(cs, ORB_ROUTE_FOUND, &rt, false);
        ESP_LOGI(TAG, "RTE %s %s-%s %s total=%u", cs, rt.from, rt.to, r.reused ? "reused" : "NEW",
                 (unsigned)r.total_ms);
    } else if (err == ESP_OK && (r.status == 200 || r.status == 404)) {
        s_rst.none++;
        route_answer(cs, ORB_ROUTE_NONE, NULL, false);
        ESP_LOGI(TAG, "RTE %s none (%d) total=%u", cs, r.status, (unsigned)r.total_ms);
    } else if (r.status >= 400 && r.status < 500) {
        s_rst.refused++;
        s_route_rest_ms = next_rest_ms(s_route_rest_ms, r.status);
        if (r.retry_after_s && (uint64_t)r.retry_after_s * 1000 > s_route_rest_ms)
            s_route_rest_ms = r.retry_after_s > 86400u ? 86400000u : r.retry_after_s * 1000u;
        s_route_rest_until_us = esp_timer_get_time() + (int64_t)s_route_rest_ms * 1000;
        ESP_LOGW(TAG, "RTE refused (%d); resting %u s", r.status, (unsigned)(s_route_rest_ms / 1000));
    } else {
        s_rst.fail++;
        route_answer(cs, ORB_ROUTE_NONE, NULL, true);
        ESP_LOGW(TAG, "RTE %s failed: %s status=%d", cs, esp_err_to_name(err), r.status);
    }
    heap_caps_free(b.buf);
}

static void flights_task(void *arg)
{
    (void)arg;
    orb_tls_mem_psram_for_me();          /* this task's TLS sessions live in PSRAM */
    int64_t next_poll_us = 0;
    for (;;) {
        orb_flights_cfg_t cfg;
        orb_flights_get_cfg(&cfg);
        const bool polling = cfg.enabled && (cfg.visible || cfg.bg_period_ms);
        if (!polling) {
            if (s_https) { orb_https_free(s_https); s_https = NULL; s_https_src = 0xFF; }
            if (s_route_https) { orb_https_free(s_route_https); s_route_https = NULL; }
            next_poll_us = 0;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (s_poll_kick || esp_timer_get_time() >= next_poll_us) {
            s_poll_kick = false;
            poll_once(&cfg);
            orb_flights_get_cfg(&cfg);   /* the wait follows the cfg as it is NOW */
            const uint32_t w = next_wait_ms(&cfg);
            next_poll_us = w ? esp_timer_get_time() + (int64_t)w * 1000 : INT64_MAX;
        }
        /* Routes ride the time between polls, one lookup per wake: a card
         * stepped through quickly queues several, and each gets its turn.
         * Only the tracker on the glass asks, so its connection goes when the
         * tracker does, and background polling keeps one socket. */
        if (cfg.visible) route_once();
        else if (s_route_https) { orb_https_free(s_route_https); s_route_https = NULL; }
        char cs[9];
        if (cfg.visible && s_routes && route_pending(cs) && esp_timer_get_time() >= s_route_rest_until_us) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));    /* the next one, soon */
            continue;
        }
        const int64_t left = next_poll_us - esp_timer_get_time();
        ulTaskNotifyTake(pdTRUE, next_poll_us == INT64_MAX ? portMAX_DELAY
                                 : left > 0 ? pdMS_TO_TICKS(left / 1000 + 1) : 0);
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

/* The stored key, once: from the feed task's first adsb.lol client or the key
 * door, whichever comes first. Two first callers both read the same NVS and
 * write the same bytes. */
static void key_load(void)
{
    if (s_key_loaded) return;
    EXT_RAM_BSS_ATTR static char k[ORB_FLIGHTS_KEY_MAX + 1], hd[ORB_FLIGHTS_HDR_MAX + 1];
    k[0] = hd[0] = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition(KEY_PART, KEY_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof k, m = sizeof hd;
        if (nvs_get_str(h, "lol_key", k, &n) != ESP_OK) k[0] = 0;
        if (nvs_get_str(h, "lol_hdr", hd, &m) != ESP_OK) hd[0] = 0;
        nvs_close(h);
    }
    if (!hd[0]) snprintf(hd, sizeof hd, "%s", ORB_FLIGHTS_KEY_HDR);
    taskENTER_CRITICAL(&s_key_mux);
    if (!s_key_loaded) {
        memcpy(s_key, k, sizeof s_key);
        memcpy(s_key_hdr, hd, sizeof s_key_hdr);
        s_key_loaded = true;
    }
    taskEXIT_CRITICAL(&s_key_mux);
}

/* Printable, no space, nothing that ends a JSON string or a header line. */
static bool key_chars_ok(const char *k)
{
    for (; *k; k++)
        if (*k <= 0x20 || *k > 0x7e || *k == '"' || *k == '\\') return false;
    return true;
}

static bool hdr_chars_ok(const char *h)
{
    for (; *h; h++)
        if (!((*h >= 'A' && *h <= 'Z') || (*h >= 'a' && *h <= 'z') || (*h >= '0' && *h <= '9') || *h == '-'))
            return false;
    return true;
}

const char *orb_flights_key_set(const char *key, const char *header)
{
    const bool clear = !key || !key[0];
    if (!header || !header[0]) header = ORB_FLIGHTS_KEY_HDR;
    if (!clear) {
        const size_t n = strlen(key);
        if (n < 8 || n > ORB_FLIGHTS_KEY_MAX) return "key must be 8 to 128 characters";
        if (!key_chars_ok(key)) return "key has a space, quote, backslash or non-ASCII character";
        if (strlen(header) > ORB_FLIGHTS_HDR_MAX || !hdr_chars_ok(header))
            return "header name must be letters, digits and dashes, at most 32";
    }
    nvs_handle_t h;
    if (nvs_open_from_partition(KEY_PART, KEY_NS, NVS_READWRITE, &h) != ESP_OK) return "key not stored (nvs open)";
    esp_err_t e;
    if (clear) {
        e = nvs_erase_key(h, "lol_key");
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) (void)nvs_erase_key(h, "lol_hdr");
    } else {
        e = nvs_set_str(h, "lol_key", key);
        if (e == ESP_OK) e = nvs_set_str(h, "lol_hdr", header);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return "key not stored (nvs)";

    EXT_RAM_BSS_ATTR static char k[ORB_FLIGHTS_KEY_MAX + 1], hd[ORB_FLIGHTS_HDR_MAX + 1];
    snprintf(k, sizeof k, "%s", clear ? "" : key);
    snprintf(hd, sizeof hd, "%s", clear ? ORB_FLIGHTS_KEY_HDR : header);
    taskENTER_CRITICAL(&s_key_mux);
    memcpy(s_key, k, sizeof s_key);
    memcpy(s_key_hdr, hd, sizeof s_key_hdr);
    s_key_gen++;
    s_key_loaded = true;
    taskEXIT_CRITICAL(&s_key_mux);
    /* A new key earns adsb.lol one fresh try: whatever rested it was the
     * request without this key. */
    s_rest_ms[ORB_FLIGHTS_ADSB_LOL] = 0;
    s_rest_until_us[ORB_FLIGHTS_ADSB_LOL] = 0;
    ESP_LOGI(TAG, "adsb.lol key %s (header %s)", clear ? "cleared" : "set", s_key_hdr);
    return NULL;
}

bool orb_flights_key_info(char *header, size_t hcap, char tail[5])
{
    key_load();
    char hd[ORB_FLIGHTS_HDR_MAX + 1], t[5] = "";
    taskENTER_CRITICAL(&s_key_mux);
    const bool set = s_key[0] != 0;
    memcpy(hd, s_key_hdr, sizeof hd);
    if (set) {
        const size_t n = strlen(s_key);
        memcpy(t, s_key + (n > 4 ? n - 4 : 0), n > 4 ? 4 : n);
    }
    taskEXIT_CRITICAL(&s_key_mux);
    if (header) snprintf(header, hcap, "%s", hd);
    if (tail) memcpy(tail, t, 5);
    return set;
}

static esp_err_t start(void)
{
    if (s_task) return ESP_OK;
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (!s_work) s_work = heap_caps_calloc(ORB_FLIGHTS_MAX, sizeof *s_work, MALLOC_CAP_SPIRAM);
    if (!s_pub) s_pub = heap_caps_calloc(ORB_FLIGHTS_MAX, sizeof *s_pub, MALLOC_CAP_SPIRAM);
    if (!s_routes) s_routes = heap_caps_calloc(ORB_FLIGHTS_ROUTES, sizeof *s_routes, MALLOC_CAP_SPIRAM);
    if (!s_mux || !s_work || !s_pub || !s_routes) return ESP_ERR_NO_MEM;
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
    s_poll_kick = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

orb_route_state_t orb_flights_route(const char *callsign, fm_route_t *out)
{
    if (!callsign || !fm_route_callsign_ok(callsign) || !s_mux || !s_routes || !s_task)
        return ORB_ROUTE_NONE;
    orb_route_state_t st = ORB_ROUTE_PENDING;
    bool queued = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int k = 0;
    for (; k < ORB_FLIGHTS_ROUTES; k++)
        if (!strcmp(s_routes[k].cs, callsign)) break;
    if (k < ORB_FLIGHTS_ROUTES) {
        st = (orb_route_state_t)s_routes[k].state;
        if (st == ORB_ROUTE_FOUND && out) *out = s_routes[k].r;
    } else {
        route_slot_t *e = &s_routes[s_route_next];          /* the oldest goes */
        s_route_next = (s_route_next + 1) % ORB_FLIGHTS_ROUTES;
        memset(e, 0, sizeof *e);
        snprintf(e->cs, sizeof e->cs, "%s", callsign);
        e->state = ORB_ROUTE_PENDING;
        queued = true;
    }
    xSemaphoreGive(s_mux);
    if (queued) xTaskNotifyGive(s_task);
    return st;
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
        "\"refused\":%u,\"failovers\":%u,\"rest_s\":[%u,%u],\"src\":%u,\"seq\":%u,\"interesting\":%d,"
        "\"opens\":%u,\"status\":%d,\"err\":%d,\"open_ms\":%u,\"total_ms\":%u,\"max_total_ms\":%u,"
        "\"bytes\":%u,\"objects\":%u,\"kept\":%u,\"int_free_min\":%u,\"int_largest_min\":%u},"
        "\"route\":{\"asked\":%u,\"found\":%u,\"none\":%u,\"fail\":%u,\"refused\":%u,\"status\":%d,\"ms\":%u},"
        "\"heap\":{\"tls_int\":%d,\"int_free\":%u,\"int_largest\":%u,\"int_lowwater\":%u,\"psram_free\":%u},"
        "\"jank\":{\"ticks\":%u,\"le125\":%u,\"le150\":%u,\"le200\":%u,\"le300\":%u,\"gt300\":%u,"
        "\"max_gap_ms\":%u,\"period_ms\":%u,\"rung\":%u,\"rung_moves\":%u,\"burst_ignored\":%u},\"ac\":[",
        cfg.enabled, cfg.lat, cfg.lon, (unsigned)cfg.radius_nm, (unsigned)cfg.period_ms,
        (unsigned)cfg.bg_period_ms, cfg.visible, cfg.boost, cfg.hide_ground,
        (unsigned)cfg.source, cfg.keep_alive,
        (unsigned)st.polls, (unsigned)st.ok, (unsigned)st.fail, (unsigned)st.skip_net,
        (unsigned)st.skip_jobs, (unsigned)st.skip_card, (unsigned)st.skip_heap,
        (unsigned)st.skip_rest, (unsigned)st.refused, (unsigned)st.failovers,
        (unsigned)rest_left_s(0), (unsigned)rest_left_s(1),
        (unsigned)st.last_src,
        (unsigned)st.seq, st.interesting,
        (unsigned)st.opens, st.last_status, st.last_err, (unsigned)st.last_open_ms,
        (unsigned)st.last_total_ms, (unsigned)st.max_total_ms, (unsigned)st.last_bytes,
        (unsigned)st.last_objects, (unsigned)st.last_kept, (unsigned)st.poll_int_free_min,
        (unsigned)st.poll_int_largest_min,
        (unsigned)s_rst.asked, (unsigned)s_rst.found, (unsigned)s_rst.none, (unsigned)s_rst.fail,
        (unsigned)s_rst.refused, s_rst.last_status, (unsigned)s_rst.last_ms,
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
