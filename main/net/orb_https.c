/* Outbound HTTPS client — contract in orb_https.h. */
#include "orb_https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "orb_sched.h"

static const char *TAG = "orb_https";

struct orb_https {
    esp_http_client_handle_t c;
    char      origin[64];
    bool      keep_alive;
    bool      live;           /* a kept connection is believed open */
    uint32_t  opens;          /* HTTP_EVENT_ON_CONNECTED count */
};

static esp_err_t on_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) {
        orb_https_t *h = e->user_data;
        if (h) h->opens++;
    }
    return ESP_OK;
}

static void sample(orb_https_result_t *r)
{
    const size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (f < r->int_free_min) r->int_free_min = f;
    if (l < r->int_largest_min) r->int_largest_min = l;
}

static uint32_t ms_since(int64_t t0)
{
    return (uint32_t)((esp_timer_get_time() - t0) / 1000);
}

orb_https_t *orb_https_new(const char *origin, const char *user_agent, bool keep_alive,
                           uint32_t timeout_ms)
{
    /* PSRAM: nothing in here is touched by DMA or with the cache off. */
    orb_https_t *h = heap_caps_calloc(1, sizeof *h, MALLOC_CAP_SPIRAM);
    if (!h) return NULL;
    snprintf(h->origin, sizeof h->origin, "%s", origin);
    h->keep_alive = keep_alive;
    char url[96];
    snprintf(url, sizeof url, "%s/", origin);
    const esp_http_client_config_t cfg = {
        .url = url,
        .user_agent = user_agent,
        .timeout_ms = (int)timeout_ms,
        .keep_alive_enable = keep_alive,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = h,
        /* Small on purpose: these come from malloc, and anything under
         * SPIRAM_MALLOC_ALWAYSINTERNAL lands in the internal pool first. The
         * sink reads in its own 1 KB pieces off the PSRAM stack. */
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    h->c = esp_http_client_init(&cfg);
    if (!h->c) {
        heap_caps_free(h);
        return NULL;
    }
    esp_http_client_set_header(h->c, "Accept", "application/json");
    return h;
}

void orb_https_free(orb_https_t *h)
{
    if (!h) return;
    if (h->c) esp_http_client_cleanup(h->c);
    heap_caps_free(h);
}

uint32_t orb_https_opens(const orb_https_t *h) { return h ? h->opens : 0; }

static esp_err_t get_once(orb_https_t *h, const char *path, orb_https_sink_fn sink, void *ctx,
                          orb_https_result_t *r)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t opens0 = h->opens;
    char url[192];
    snprintf(url, sizeof url, "%s%s", h->origin, path);
    esp_http_client_set_url(h->c, url);

    /* No live connection means this open is a TLS handshake: tell the dial's
     * scheduler to ride it out rather than re-rate (orb_sched_bg_burst). */
    const bool handshake = !h->live;
    if (handshake) orb_sched_bg_burst(true);
    esp_err_t err = esp_http_client_open(h->c, 0);
    if (handshake) orb_sched_bg_burst(false);
    r->reused = (h->opens == opens0);
    r->open_ms = ms_since(t0);
    sample(r);
    if (err != ESP_OK) {
        esp_http_client_close(h->c);
        h->live = false;
        return err;
    }
    const int64_t t1 = esp_timer_get_time();
    r->content_len = esp_http_client_fetch_headers(h->c);
    r->ttfb_ms = ms_since(t1);
    r->status = esp_http_client_get_status_code(h->c);
    sample(r);
    if (r->content_len < 0 && r->status == 0) {
        esp_http_client_close(h->c);
        h->live = false;
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    char buf[1024];
    bool keep = true;
    for (;;) {
        const int n = esp_http_client_read(h->c, buf, sizeof buf);
        if (n < 0) { keep = false; err = ESP_ERR_HTTP_EAGAIN; break; }
        if (n == 0) break;
        r->bytes += (size_t)n;
        sample(r);
        if (r->status == 200 && sink && !sink(buf, (size_t)n, ctx)) { keep = false; break; }
    }
    r->complete = esp_http_client_is_complete_data_received(h->c);
    if (!r->complete) keep = false;
    if (!h->keep_alive || !keep) esp_http_client_close(h->c);
    h->live = h->keep_alive && keep;
    r->total_ms = ms_since(t0);
    return r->complete ? ESP_OK : (err != ESP_OK ? err : ESP_FAIL);
}

esp_err_t orb_https_get(orb_https_t *h, const char *path, orb_https_sink_fn sink, void *ctx,
                        orb_https_result_t *res)
{
    orb_https_result_t r = { .int_free_min = SIZE_MAX, .int_largest_min = SIZE_MAX };
    esp_err_t err = get_once(h, path, sink, ctx, &r);
    if (err != ESP_OK && r.reused && r.bytes == 0) {
        /* The server dropped the idle connection; that is keep-alive's normal
         * end, not a failure. One fresh try, and its numbers are the result. */
        ESP_LOGI(TAG, "kept connection was closed by the server; reconnecting");
        h->live = false;
        orb_https_result_t r2 = { .int_free_min = r.int_free_min,
                                  .int_largest_min = r.int_largest_min };
        err = get_once(h, path, sink, ctx, &r2);
        r = r2;
    }
    r.err = err;
    if (res) *res = r;
    return err;
}
