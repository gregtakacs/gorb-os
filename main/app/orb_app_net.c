/**
 * orb_app_net.c — the app network service and credential store. Contract in
 * orb_app_net.h.
 *
 * Lifted from the flight feed (main/app/orb_flights.c, retired by §43): the gate, the
 * PSRAM-stacked task with its TLS sessions in PSRAM, and the key store — now with no
 * idea what a flight is. Nothing is made at boot: the task, its stack and every
 * buffer appear on the first request, because the boot ladder bottoms out the
 * internal pool at wifi_start and a service nobody has asked for must not be in that
 * queue (the measurement the feed's own init recorded).
 */
#include "orb_app_net.h"

#include <stdio.h>
#include <stdlib.h>          /* strtoul: the sweep reads its own key names */
#include <string.h>
#include <strings.h>      /* strncasecmp: header names are case-insensitive */

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "orb_app.h"
#include "orb_svc.h"         /* ORB_SET_SECRET, orb_svc_cfg_touch */
#include "orb_https.h"
#include "orb_json.h"
#include "orb_jobs.h"
#include "orb_net.h"
#include "orb_sd.h"
#include "orb_tls_mem.h"
#include "orb_usb_msc.h"

static const char *TAG = "appnet";

/* Below orb_net/orb_dns (4), the jobs worker (3) and lvgl (5): a fetch only ever
 * waits on the network, and must never delay a frame. */
#define NET_PRIO          2
#define NET_STACK         8192      /* mbedTLS handshake + cert verify; PSRAM */
#define NET_TIMEOUT_MS    10000
/* A card session's tail: the display refunds its ring up to 1.5 s after the card
 * goes quiet, and a handshake inside that window starves both. */
#define NET_CARD_QUIET_MS 2000
/* The internal block a request needs to start: a few lwIP pbufs and the radio's. */
#define NET_MIN_LARGEST   1536
/* How long a request waits on the gate before it fails as "not now". */
#define NET_GATE_WAIT_MS  20000

typedef struct {
    volatile int32_t state;          /* ORB_NET_*; written by both sides, see below */
    bool     queued;                 /* LVGL task -> worker: a request is waiting */
    bool     dead;                   /* released while busy: the worker frees */
    char     origin[72];
    /* 480: weather-plan §4 item 1, Open-Meteo's one request for four places runs about
     * 465 B. The request line "GET <path> HTTP/1.1\r\n", and after it each header, must
     * fit esp_http_client's tx buffer (orb_https.c: 768 since OS v1.2.17; at 512 a 442 B
     * path left no room for the User-Agent and the request went unsent). */
    char     path[480];
    char     hname[ORB_APP_CRED_HDR_MAX + 1];
    char     hval[ORB_APP_CRED_MAX + 1];
    char     slug[32];
    bool     cred;
    uint8_t  method;                 /* orb_https_method_t */
    char     reqh[ORB_APP_NET_HDRS_MAX + 1];   /* the request's own "Name: v\n" lines */
    uint8_t *req_body;               /* PSRAM copy of the request body, or NULL */
    uint32_t req_len;
    char     resph[768];             /* the response's headers, copied on landing */
    orb_json_t json;                 /* the reader over body or jbuf */
    char    *jbuf;                   /* an app buffer copied for the reader, PSRAM */
    uint8_t *body;
    uint32_t len, cap;
    bool     over;                   /* the body passed ORB_APP_NET_BODY_MAX */
    int32_t  status, err;
    uint32_t retry_after_s, total_ms;
    orb_https_t *cli;                /* kept between requests to the same origin */
    char     cli_origin[72];
    char     cli_hdr[ORB_APP_CRED_HDR_MAX + ORB_APP_CRED_MAX + 2];
} net_slot_t;

static net_slot_t  *s_net[ORB_APP_CAP];
static TaskHandle_t s_task;

/* ---- the gate ------------------------------------------------------------------ */

static const char *gate(void)
{
    if (!orb_net_is_up()) return "no network";
    if (orb_jobs_busy()) return "a job is running";
    if (orb_sd_lock_idle_ms() < NET_CARD_QUIET_MS) return "the card is busy";
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NET_MIN_LARGEST)
        return "internal heap is thin";
    return NULL;
}

/* ---- the worker ---------------------------------------------------------------- */

static bool sink(const char *buf, size_t len, void *ctx)
{
    net_slot_t *n = ctx;
    if (!buf) {                       /* the length, told first (§6.21): one block that size */
        if (len <= ORB_APP_NET_BODY_MAX && len > n->cap) {
            uint8_t *nb = heap_caps_realloc(n->body, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (nb) { n->body = nb; n->cap = (uint32_t)len; }
        }
        return true;                  /* a refusal here is the doubling's to meet */
    }
    if (n->len + len > ORB_APP_NET_BODY_MAX) { n->over = true; return false; }
    if (n->len + len > n->cap) {
        uint32_t want = n->cap ? n->cap : 16384u;
        while (want < n->len + len) want *= 2;
        if (want > ORB_APP_NET_BODY_MAX) want = ORB_APP_NET_BODY_MAX;
        uint8_t *nb = heap_caps_realloc(n->body, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!nb) { n->over = true; return false; }
        n->body = nb;
        n->cap = want;
    }
    memcpy(n->body + n->len, buf, len);
    n->len += (uint32_t)len;
    return true;
}

static void fetch(net_slot_t *n)
{
    /* A USB disk window: the owner has handed the card to a computer and can do
     * nothing else with the orb until the reboot that ends it (owner, 2026-11-09), so
     * the apps' fetches stand down and their kept connections close, which gives
     * their TLS sessions' memory back for the window. The app sees a failure, as it
     * would with no network. */
    if (orb_usb_msc_window_open()) {
        if (n->cli) { orb_https_free(n->cli); n->cli = NULL; }
        n->err = ESP_ERR_INVALID_STATE;
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const char *why;
    while ((why = gate()) != NULL) {
        if (esp_timer_get_time() - t0 > (int64_t)NET_GATE_WAIT_MS * 1000) {
            ESP_LOGI(TAG, "%s: not fetched: %s", n->slug, why);
            n->err = ESP_ERR_TIMEOUT;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    /* One kept connection per app, rebuilt when the origin or the header moves. */
    char hdr[sizeof n->cli_hdr];
    snprintf(hdr, sizeof hdr, "%s:%s", n->hname, n->hval);
    if (n->cli && (strcmp(n->cli_origin, n->origin) || strcmp(n->cli_hdr, hdr))) {
        orb_https_free(n->cli);
        n->cli = NULL;
    }
    if (!n->cli) {
        char ua[96];
        snprintf(ua, sizeof ua, "gorb-os/%s (%s)", esp_app_get_description()->version, n->slug);
        n->cli = orb_https_new(n->origin, ua, true, NET_TIMEOUT_MS);
        if (!n->cli) { n->err = ESP_ERR_NO_MEM; return; }
        snprintf(n->cli_origin, sizeof n->cli_origin, "%s", n->origin);
        snprintf(n->cli_hdr, sizeof n->cli_hdr, "%s", hdr);
        if (n->hname[0] && n->hval[0]) orb_https_set_header(n->cli, n->hname, n->hval);
    }
    /* The request's own headers ride this request only: set, sent, then removed so a
     * kept connection never carries them into the app's next request. */
    char names[8][40];
    int nn = 0;
    for (char *line = n->reqh; *line && nn < 8;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *colon = strchr(line, ':');
        if (colon && colon > line && colon - line < 40) {
            *colon = 0;
            const char *v = colon + 1;
            while (*v == ' ') v++;
            if (orb_https_set_header(n->cli, line, v) == ESP_OK) {
                memcpy(names[nn], line, (size_t)(colon - line) + 1);   /* fits: checked above */
                nn++;
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    orb_https_result_t r;
    memset(&r, 0, sizeof r);
    const esp_err_t e = orb_https_request(n->cli, (orb_https_method_t)n->method, n->path,
                                          n->req_body, n->req_len, sink, n, &r);
    snprintf(n->resph, sizeof n->resph, "%s", orb_https_resp_headers(n->cli));
    for (int i = 0; i < nn; i++) (void)orb_https_del_header(n->cli, names[i]);
    heap_caps_free(n->req_body);
    n->req_body = NULL;
    n->status = r.status;
    n->retry_after_s = r.retry_after_s;
    n->total_ms = r.total_ms;
    n->err = n->over ? ESP_ERR_INVALID_SIZE : e;
    if (e != ESP_OK || !r.complete) {          /* a torn connection is not kept */
        orb_https_free(n->cli);
        n->cli = NULL;
    }
    ESP_LOGI(TAG, "%s: GET %s%s -> %d %s, %u B in %u ms", n->slug, n->origin, n->path,
             r.status, esp_err_to_name(n->err), (unsigned)n->len, (unsigned)r.total_ms);
}

static void net_task(void *arg)
{
    (void)arg;
    orb_tls_mem_psram_for_me();              /* this task's TLS sessions live in PSRAM */
    for (;;) {
        bool any = false;
        for (int i = 0; i < ORB_APP_CAP; i++) {
            net_slot_t *n = s_net[i];
            if (!n || !n->queued) continue;
            n->queued = false;
            any = true;
            fetch(n);
            if (n->dead) {
                if (n->cli) orb_https_free(n->cli);
                heap_caps_free(n->body);
                heap_caps_free(n);
                continue;
            }
            const bool ok = n->err == ESP_OK && n->status > 0;
            __atomic_store_n(&n->state, ok ? ORB_NET_DONE : ORB_NET_FAILED, __ATOMIC_RELEASE);
        }
        if (!any) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

static bool task_up(void)
{
    if (s_task) return true;
    StackType_t *stk = heap_caps_malloc(NET_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = heap_caps_calloc(1, sizeof *tcb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stk || !tcb) {
        heap_caps_free(stk);
        heap_caps_free(tcb);
        return false;
    }
    s_task = xTaskCreateStatic(net_task, "appnet", NET_STACK, NULL, NET_PRIO, stk, tcb);
    return s_task != NULL;
}

/* ---- the request side (LVGL task) ------------------------------------------------ */

static net_slot_t *slot_get(int slot, bool create)
{
    if (slot < 0 || slot >= ORB_APP_CAP) return NULL;
    if (!s_net[slot] && create)
        s_net[slot] = heap_caps_calloc(1, sizeof *s_net[slot], MALLOC_CAP_SPIRAM);
    return s_net[slot];
}

static esp_err_t cred_attach(const orb_app_t *app, const char *id, const char *url,
                             char *hname, size_t hcap, char *hval, size_t vcap);

esp_err_t orb_app_net_request(int slot, const orb_app_t *app, uint32_t method, const char *url,
                              const char *headers, const void *body, uint32_t body_len,
                              const char *cred)
{
    if (!app || !url || method > ORB_HTTPS_HEAD) return ESP_ERR_INVALID_ARG;
    const bool tls = !strncmp(url, "https://", 8);
    if (!tls && strncmp(url, "http://", 7)) return ESP_ERR_INVALID_ARG;
    /* origin = scheme://host[:port], path = the rest ("/" when there is none) */
    const char *host = url + (tls ? 8 : 7);
    const char *slash = strchr(host, '/');
    const size_t olen = slash ? (size_t)(slash - url) : strlen(url);
    const char *path = slash ? slash : "/";
    if (olen >= sizeof ((net_slot_t *)0)->origin || strlen(path) >= sizeof ((net_slot_t *)0)->path ||
        (headers && strlen(headers) > ORB_APP_NET_HDRS_MAX) || body_len > ORB_APP_NET_REQ_MAX)
        return ESP_ERR_INVALID_SIZE;
    net_slot_t *n = slot_get(slot, true);
    if (!n) return ESP_ERR_NO_MEM;
    if (__atomic_load_n(&n->state, __ATOMIC_ACQUIRE) == ORB_NET_BUSY) return ESP_ERR_INVALID_STATE;
    if (!task_up()) return ESP_ERR_NO_MEM;
    uint8_t *copy = NULL;
    if (body && body_len) {
        copy = heap_caps_malloc(body_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!copy) return ESP_ERR_NO_MEM;
        memcpy(copy, body, body_len);
    }
    memcpy(n->origin, url, olen);
    n->origin[olen] = 0;
    snprintf(n->path, sizeof n->path, "%s", path);
    snprintf(n->slug, sizeof n->slug, "%s", app->slug ? app->slug : "?");
    snprintf(n->reqh, sizeof n->reqh, "%s", headers ? headers : "");
    n->method = (uint8_t)method;
    n->hname[0] = n->hval[0] = 0;
    /* §43: a secret goes only to the https host its declaration names. A request
     * that names one it may not carry is refused outright — never sent without it. */
    if (cred) {
        const esp_err_t ce = cred_attach(app, cred, url, n->hname, sizeof n->hname,
                                         n->hval, sizeof n->hval);
        if (ce != ESP_OK) {
            heap_caps_free(copy);
            n->hname[0] = n->hval[0] = 0;
            return ce;
        }
    }
    heap_caps_free(n->req_body);
    n->req_body = copy;
    n->req_len = copy ? body_len : 0;
    n->len = 0;
    n->over = false;
    n->status = 0;
    n->err = ESP_OK;
    n->retry_after_s = n->total_ms = 0;
    n->resph[0] = 0;
    __atomic_store_n(&n->state, ORB_NET_BUSY, __ATOMIC_RELEASE);
    n->queued = true;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

int32_t orb_app_net_header(int slot, const char *name, char *out, uint32_t cap)
{
    net_slot_t *n = slot_get(slot, false);
    if (!n || !name || !cap || __atomic_load_n(&n->state, __ATOMIC_ACQUIRE) == ORB_NET_BUSY) return -1;
    const size_t nl = strlen(name);
    for (const char *l = n->resph; *l;) {
        const char *e = strchr(l, '\n');
        if (!e) break;
        if ((size_t)(e - l) > nl + 1 && !strncasecmp(l, name, nl) && l[nl] == ':') {
            const char *v = l + nl + 1;
            while (*v == ' ') v++;
            const size_t vl = (size_t)(e - v);
            const size_t k = vl < cap - 1 ? vl : cap - 1;
            memcpy(out, v, k);
            out[k] = 0;
            return (int32_t)vl;
        }
        l = e + 1;
    }
    return -1;
}

/* ---- JSON over a landed body or an app buffer (D51) ------------------------------ */

esp_err_t orb_app_json_begin(int slot, const void *buf, uint32_t len)
{
    net_slot_t *n = slot_get(slot, true);
    if (!n) return ESP_ERR_NO_MEM;
    heap_caps_free(n->jbuf);
    n->jbuf = NULL;
    if (!buf) {                                   /* the landed body */
        if (__atomic_load_n(&n->state, __ATOMIC_ACQUIRE) != ORB_NET_DONE || !n->body)
            return ESP_ERR_INVALID_STATE;
        orb_json_begin(&n->json, (const char *)n->body, n->len);
        return ESP_OK;
    }
    if (len > ORB_APP_JSON_BUF_MAX) return ESP_ERR_INVALID_SIZE;
    n->jbuf = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!n->jbuf) return ESP_ERR_NO_MEM;
    memcpy(n->jbuf, buf, len);
    orb_json_begin(&n->json, n->jbuf, len);
    return ESP_OK;
}

int32_t orb_app_json_next(int slot, orb_json_tok_t *t)
{
    net_slot_t *n = slot_get(slot, false);
    if (!n) { memset(t, 0, sizeof *t); return ORB_JSON_ERROR; }
    return orb_json_next(&n->json, t);
}

int32_t orb_app_json_skip(int slot)
{
    net_slot_t *n = slot_get(slot, false);
    return n ? orb_json_skip(&n->json) : ORB_JSON_ERROR;
}

void orb_app_net_info(int slot, orb_app_net_info_t *out)
{
    memset(out, 0, sizeof *out);
    net_slot_t *n = slot_get(slot, false);
    if (!n) return;
    out->state = __atomic_load_n(&n->state, __ATOMIC_ACQUIRE);
    if (out->state == ORB_NET_BUSY) return;
    out->status = n->status;
    out->len = n->len;
    out->retry_after_s = n->retry_after_s;
    out->total_ms = n->total_ms;
    out->err = n->err;
}

uint32_t orb_app_net_read(int slot, uint32_t off, void *dst, uint32_t cap)
{
    net_slot_t *n = slot_get(slot, false);
    if (!n || __atomic_load_n(&n->state, __ATOMIC_ACQUIRE) != ORB_NET_DONE ||
        !n->body || off >= n->len)
        return 0;
    const uint32_t k = n->len - off < cap ? n->len - off : cap;
    memcpy(dst, n->body + off, k);
    return k;
}

void orb_app_net_free(int slot)
{
    net_slot_t *n = slot_get(slot, false);
    if (!n || __atomic_load_n(&n->state, __ATOMIC_ACQUIRE) == ORB_NET_BUSY) return;
    heap_caps_free(n->body);
    n->body = NULL;
    n->len = n->cap = 0;
    if (!n->jbuf) orb_json_begin(&n->json, "", 0);   /* a reader over the body ends here */
    __atomic_store_n(&n->state, ORB_NET_IDLE, __ATOMIC_RELEASE);
}

uint8_t *orb_app_net_take(int slot, uint32_t *len)
{
    *len = 0;
    net_slot_t *n = slot_get(slot, false);
    if (!n || __atomic_load_n(&n->state, __ATOMIC_ACQUIRE) != ORB_NET_DONE || !n->body || !n->len || n->over)
        return NULL;
    uint8_t *b = n->body;
    if (n->cap > n->len) {                       /* give back the doubling's slack */
        uint8_t *t = heap_caps_realloc(b, n->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (t) b = t;
    }
    *len = n->len;
    n->body = NULL;
    n->len = n->cap = 0;
    if (!n->jbuf) orb_json_begin(&n->json, "", 0);
    __atomic_store_n(&n->state, ORB_NET_IDLE, __ATOMIC_RELEASE);
    return b;
}

void orb_app_net_release(int slot)
{
    net_slot_t *n = slot_get(slot, false);
    if (!n) return;
    s_net[slot] = NULL;
    if (__atomic_load_n(&n->state, __ATOMIC_ACQUIRE) == ORB_NET_BUSY) {
        n->dead = true;                        /* the worker frees it when it lands */
        return;
    }
    if (n->cli) orb_https_free(n->cli);
    heap_caps_free(n->body);
    heap_caps_free(n->jbuf);
    heap_caps_free(n->req_body);
    heap_caps_free(n);
}

/* ---- credentials (§43: an app's declared secrets) ----------------------------------
 *
 * One entry per (app, secret setting id). The flight tracker's `key` keeps the NVS
 * location it had while the feed lived in the firmware (orb.flights / lol_key,
 * lol_hdr), so a board that already holds one keeps it; every other secret lives in
 * "orb.cred" under keys hashed from "slug/id" (NVS keys are 15 characters).
 *
 * NVS is read only from a task on an internal stack: a flash read with the stack in
 * PSRAM trips IDF's assert in spi_flash_disable_interrupts_caches_and_other_cpu, and
 * both the guest's stack and this service's are PSRAM. So a request answers from a
 * RAM copy that the bind (boot, an internal stack) and the
 * LAN door fill, and never from NVS. */

#define CRED_PART "storage"
#define CRED_DEFAULT_HDR "x-api-key"
#define CRED_MAX_N (ORB_APP_CAP * 2)

static uint32_t cred_hash(const char *slug, const char *id)
{
    uint32_t h = 2166136261u;                   /* FNV-1a over "slug/id" */
    for (const char *p = slug; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ '/') * 16777619u;
    for (const char *p = id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return h;
}

static bool cred_is_legacy(const char *slug, const char *id)
{
    return !strcmp(slug, "flights") && !strcmp(id, "key");
}

static void cred_names(const char *slug, const char *id, char ns[16], char kk[16], char hk[16])
{
    if (cred_is_legacy(slug, id)) {
        snprintf(ns, 16, "orb.flights");
        snprintf(kk, 16, "lol_key");
        snprintf(hk, 16, "lol_hdr");
        return;
    }
    const uint32_t h = cred_hash(slug, id);
    snprintf(ns, 16, "orb.cred");
    snprintf(kk, 16, "k%08lx", (unsigned long)h);
    snprintf(hk, 16, "h%08lx", (unsigned long)h);
}

static bool cred_load(const char *slug, const char *id, char *hdr, size_t hcap,
                      char *key, size_t kcap)
{
    char ns[16], kk[16], hk[16];
    cred_names(slug, id, ns, kk, hk);
    key[0] = hdr[0] = 0;
    nvs_handle_t h;
    if (nvs_open_from_partition(CRED_PART, ns, NVS_READONLY, &h) == ESP_OK) {
        size_t n = kcap, m = hcap;
        if (nvs_get_str(h, kk, key, &n) != ESP_OK) key[0] = 0;
        if (nvs_get_str(h, hk, hdr, &m) != ESP_OK) hdr[0] = 0;
        nvs_close(h);
    }
    if (!hdr[0]) snprintf(hdr, hcap, "%s", CRED_DEFAULT_HDR);
    return key[0] != 0;
}

typedef struct {
    char slug[32];
    char id[16];
    char hdr[ORB_APP_CRED_HDR_MAX + 1];
    char key[ORB_APP_CRED_MAX + 1];
} cred_ent_t;
static cred_ent_t *s_cred;                     /* CRED_MAX_N entries, PSRAM */

static cred_ent_t *cred_ent(const char *slug, const char *id, bool create)
{
    if (!s_cred) {
        if (!create) return NULL;
        s_cred = heap_caps_calloc(CRED_MAX_N, sizeof *s_cred, MALLOC_CAP_SPIRAM);
        if (!s_cred) return NULL;
    }
    cred_ent_t *free_e = NULL;
    for (int i = 0; i < CRED_MAX_N; i++) {
        if (!strcmp(s_cred[i].slug, slug) && !strcmp(s_cred[i].id, id)) return &s_cred[i];
        if (!s_cred[i].slug[0] && !free_e) free_e = &s_cred[i];
    }
    if (!create || !free_e) return NULL;
    snprintf(free_e->slug, sizeof free_e->slug, "%s", slug);
    snprintf(free_e->id, sizeof free_e->id, "%s", id);
    return free_e;
}

/* The app's declared secret `id`, or NULL. */
static const orb_setting_desc_t *secret_of(const orb_app_t *app, const char *id)
{
    if (!app || !id) return NULL;
    for (int i = 0; i < app->n_settings; i++)
        if (app->settings[i].type == ORB_SET_SECRET &&
            !strncmp(app->settings[i].id, id, sizeof app->settings[i].id))
            return &app->settings[i];
    return NULL;
}

void orb_app_cred_preload(const orb_app_t *app)
{
    if (!app || !app->slug) return;
    for (int i = 0; i < app->n_settings; i++) {
        if (app->settings[i].type != ORB_SET_SECRET) continue;
        cred_ent_t *e = cred_ent(app->slug, app->settings[i].id, true);
        if (e) (void)cred_load(app->slug, e->id, e->hdr, sizeof e->hdr, e->key, sizeof e->key);
    }
}

bool orb_app_cred_has(const orb_app_t *app, const char *id)
{
    if (!secret_of(app, id)) return false;
    const cred_ent_t *e = cred_ent(app->slug, id, false);
    return e && e->key[0];
}

/* The host part of an https:// URL, lower-cased, into out; false when it is not one. */
static bool url_host(const char *url, char *out, size_t cap)
{
    if (strncmp(url, "https://", 8)) return false;
    const char *h = url + 8;
    size_t n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && h[n] != '?') n++;
    if (!n || n >= cap) return false;
    for (size_t i = 0; i < n; i++) out[i] = (char)((h[i] >= 'A' && h[i] <= 'Z') ? h[i] + 32 : h[i]);
    out[n] = 0;
    return true;
}

static esp_err_t cred_attach(const orb_app_t *app, const char *id, const char *url,
                             char *hname, size_t hcap, char *hval, size_t vcap)
{
    const orb_setting_desc_t *d = secret_of(app, id);
    if (!d) return ESP_ERR_NOT_FOUND;                    /* not a secret this app declared */
    char host[ORB_APP_SLUG_MAX + 40];
    if (!url_host(url, host, sizeof host) || strcmp(host, d->def_str)) {
        ESP_LOGW(TAG, "%s: secret \"%s\" refused for %s — it may go only to https://%s",
                 app->slug, id, url, d->def_str);
        return ESP_ERR_NOT_ALLOWED;
    }
    const cred_ent_t *e = cred_ent(app->slug, id, false);
    if (!e || !e->key[0]) return ESP_ERR_INVALID_STATE;  /* the owner has not set it */
    snprintf(hname, hcap, "%s", e->hdr);
    snprintf(hval, vcap, "%s", e->key);
    return ESP_OK;
}

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

const char *orb_app_cred_set(const orb_app_t *app, const char *id, const char *key,
                             const char *header)
{
    if (!secret_of(app, id)) return "the app declares no such secret";
    const bool clear = !key || !key[0];
    if (!header || !header[0]) header = CRED_DEFAULT_HDR;
    if (!clear) {
        const size_t n = strlen(key);
        if (n < 8 || n > ORB_APP_CRED_MAX) return "key must be 8 to 128 characters";
        if (!key_chars_ok(key)) return "key has a space, quote, backslash or non-ASCII character";
        if (strlen(header) > ORB_APP_CRED_HDR_MAX || !hdr_chars_ok(header))
            return "header name must be letters, digits and dashes, at most 32";
    }
    char ns[16], kk[16], hk[16];
    cred_names(app->slug, id, ns, kk, hk);
    nvs_handle_t h;
    if (nvs_open_from_partition(CRED_PART, ns, NVS_READWRITE, &h) != ESP_OK) return "key not stored (nvs open)";
    esp_err_t e;
    if (clear) {
        e = nvs_erase_key(h, kk);
        if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
        if (e == ESP_OK) (void)nvs_erase_key(h, hk);
    } else {
        e = nvs_set_str(h, kk, key);
        if (e == ESP_OK) e = nvs_set_str(h, hk, header);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return "key not stored (nvs)";
    cred_ent_t *c = cred_ent(app->slug, id, true);
    if (c) {
        snprintf(c->key, sizeof c->key, "%s", clear ? "" : key);
        snprintf(c->hdr, sizeof c->hdr, "%s", clear ? CRED_DEFAULT_HDR : header);
    }
    orb_svc_cfg_touch(app);              /* the app's cfg_gen moves: it re-asks net_cred */
    ESP_LOGI(TAG, "%s: secret \"%s\" %s (header %s)", app->slug, id,
             clear ? "cleared" : "set", clear ? CRED_DEFAULT_HDR : header);
    return NULL;
}

void orb_app_cred_forget(const orb_app_t *app)
{
    if (!app) return;
    for (int i = 0; i < app->n_settings; i++)
        if (app->settings[i].type == ORB_SET_SECRET)
            (void)orb_app_cred_set(app, app->settings[i].id, NULL, NULL);
}

bool orb_app_cred_info(const orb_app_t *app, const char *id, char *header, size_t hcap,
                       char tail[5])
{
    EXT_RAM_BSS_ATTR static char k[ORB_APP_CRED_MAX + 1];
    char hd[ORB_APP_CRED_HDR_MAX + 1];
    const bool set = secret_of(app, id) && cred_load(app->slug, id, hd, sizeof hd, k, sizeof k);
    if (!set) snprintf(hd, sizeof hd, "%s", CRED_DEFAULT_HDR);
    if (header) snprintf(header, hcap, "%s", hd);
    if (tail) {
        memset(tail, 0, 5);
        if (set) {
            const size_t n = strlen(k);
            memcpy(tail, k + (n > 4 ? n - 4 : 0), n > 4 ? 4 : n);
        }
    }
    memset(k, 0, sizeof k);
    return set;
}

/* ---- the boot sweep (§43.3) -------------------------------------------------------
 * A secret lives only as long as an installed app declares it. Uninstall erases an
 * app's secrets, but a power cut between that and the slot erase, or a store written
 * by an older firmware, can strand one; the sweep erases every entry no installed
 * app's declaration names. Boot ladder, main task (internal stack: it reads and
 * writes NVS), after resolve_boot has filled the card table. */
#define SWEEP_MAX 32

void orb_app_cred_sweep(void)
{
    uint32_t keep[ORB_APP_CAP * 16];
    size_t nk = 0;
    bool keep_legacy = false;
    for (size_t i = 0; i < orb_app_rows(); i++) {
        const orb_app_t *a = orb_app_row(i);
        if (!a || !a->slug || !orb_app_is_card(a)) continue;
        for (int k = 0; k < a->n_settings; k++) {
            if (a->settings[k].type != ORB_SET_SECRET) continue;
            if (cred_is_legacy(a->slug, a->settings[k].id)) keep_legacy = true;
            else if (nk < sizeof keep / sizeof keep[0])
                keep[nk++] = cred_hash(a->slug, a->settings[k].id);
        }
    }
    char stray[SWEEP_MAX][NVS_KEY_NAME_MAX_SIZE];
    int ns = 0;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(CRED_PART, "orb.cred", NVS_TYPE_ANY, &it);
    while (e == ESP_OK && it && ns < SWEEP_MAX) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool ours = false;
        if ((info.key[0] == 'k' || info.key[0] == 'h') && strlen(info.key) == 9) {
            const uint32_t h = (uint32_t)strtoul(info.key + 1, NULL, 16);
            for (size_t j = 0; j < nk && !ours; j++) ours = keep[j] == h;
        }
        if (!ours) snprintf(stray[ns++], sizeof stray[0], "%s", info.key);
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    int erased = 0;
    nvs_handle_t h;
    if (ns && nvs_open_from_partition(CRED_PART, "orb.cred", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < ns; i++) if (nvs_erase_key(h, stray[i]) == ESP_OK) erased++;
        nvs_commit(h);
        nvs_close(h);
    }
    if (!keep_legacy && nvs_open_from_partition(CRED_PART, "orb.flights", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, "lol_key") == ESP_OK) erased++;
        if (nvs_erase_key(h, "lol_hdr") == ESP_OK) erased++;
        nvs_commit(h);
        nvs_close(h);
    }
    if (erased) ESP_LOGI(TAG, "secret sweep: erased %d stored value%s no installed app declares",
                         erased, erased == 1 ? "" : "s");
}
