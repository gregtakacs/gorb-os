/* Outbound HTTPS client — contract in orb_https.h. */
#include "orb_https.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>          /* strcasecmp: header names are case-insensitive */

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
    uint32_t  retry_after_s;  /* this response's Retry-After, 0 = none or a date */
    char      hdrs[768];      /* this response's headers, "name: value\n" each, cut
                               * at the buffer's end (D51: apps read them) */
    size_t    hlen;
};

static esp_err_t on_event(esp_http_client_event_t *e)
{
    orb_https_t *h = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) {
        if (h) h->opens++;
    } else if (e->event_id == HTTP_EVENT_ON_HEADER && h && e->header_key && e->header_value) {
        const int n = snprintf(h->hdrs + h->hlen, sizeof h->hdrs - h->hlen, "%s: %s\n",
                               e->header_key, e->header_value);
        if (n > 0 && h->hlen + (size_t)n < sizeof h->hdrs) h->hlen += (size_t)n;
        else h->hdrs[h->hlen] = 0;                   /* a header that does not fit is dropped */
    }
    if (e->event_id == HTTP_EVENT_ON_HEADER && h && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "Retry-After")) {
        /* Delta-seconds only: the HTTP-date form reads as 0, and the caller's
         * own back-off then stands. */
        char *end = NULL;
        const unsigned long v = strtoul(e->header_value, &end, 10);
        h->retry_after_s = (end && end != e->header_value && v < 86400UL * 31) ? (uint32_t)v : 0;
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
    snprintf(url, sizeof url, "%s/", origin);           /* http:// or https:// (D51) */
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

esp_err_t orb_https_set_header(orb_https_t *h, const char *name, const char *value)
{
    if (!h || !h->c || !name || !value) return ESP_ERR_INVALID_ARG;
    return esp_http_client_set_header(h->c, name, value);
}

void orb_https_free(orb_https_t *h)
{
    if (!h) return;
    if (h->c) esp_http_client_cleanup(h->c);
    heap_caps_free(h);
}

uint32_t orb_https_opens(const orb_https_t *h) { return h ? h->opens : 0; }

static esp_err_t get_once(orb_https_t *h, orb_https_method_t method, const char *path,
                          const void *body, size_t body_len,
                          orb_https_sink_fn sink, void *ctx, orb_https_result_t *r)
{
    static const esp_http_client_method_t M[] = {
        [ORB_HTTPS_GET] = HTTP_METHOD_GET, [ORB_HTTPS_POST] = HTTP_METHOD_POST,
        [ORB_HTTPS_PUT] = HTTP_METHOD_PUT, [ORB_HTTPS_DELETE] = HTTP_METHOD_DELETE,
        [ORB_HTTPS_HEAD] = HTTP_METHOD_HEAD,
    };
    const int64_t t0 = esp_timer_get_time();
    const uint32_t opens0 = h->opens;
    h->retry_after_s = 0;
    h->hlen = 0;
    h->hdrs[0] = 0;
    char url[320];
    snprintf(url, sizeof url, "%s%s", h->origin, path);
    esp_http_client_set_url(h->c, url);
    esp_http_client_set_method(h->c, (unsigned)method <= ORB_HTTPS_HEAD ? M[method] : HTTP_METHOD_GET);

    /* No live connection means this open is a TLS handshake: tell the dial's
     * scheduler to ride it out rather than re-rate (orb_sched_bg_burst). */
    const bool handshake = !h->live;
    if (handshake) orb_sched_bg_burst(true);
    esp_err_t err = esp_http_client_open(h->c, body ? (int)body_len : 0);
    if (handshake) orb_sched_bg_burst(false);
    if (err == ESP_OK && body && body_len &&
        esp_http_client_write(h->c, body, (int)body_len) != (int)body_len)
        err = ESP_ERR_HTTP_WRITE_DATA;
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
    r->retry_after_s = h->retry_after_s;
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
        /* Every status's body reaches the sink: an app may want the error page. */
        if (sink && !sink(buf, (size_t)n, ctx)) { keep = false; break; }
    }
    r->complete = esp_http_client_is_complete_data_received(h->c);
    if (!r->complete) keep = false;
    if (!h->keep_alive || !keep) esp_http_client_close(h->c);
    h->live = h->keep_alive && keep;
    r->total_ms = ms_since(t0);
    return r->complete ? ESP_OK : (err != ESP_OK ? err : ESP_FAIL);
}

esp_err_t orb_https_request(orb_https_t *h, orb_https_method_t method, const char *path,
                            const void *body, size_t body_len,
                            orb_https_sink_fn sink, void *ctx, orb_https_result_t *res)
{
    orb_https_result_t r = { .int_free_min = SIZE_MAX, .int_largest_min = SIZE_MAX };
    esp_err_t err = get_once(h, method, path, body, body_len, sink, ctx, &r);
    if (err != ESP_OK && r.reused && r.bytes == 0) {
        ESP_LOGI(TAG, "kept connection was closed by the server; reconnecting");
        h->live = false;
        orb_https_result_t r2 = { .int_free_min = r.int_free_min,
                                  .int_largest_min = r.int_largest_min };
        err = get_once(h, method, path, body, body_len, sink, ctx, &r2);
        r = r2;
    }
    r.err = err;
    if (res) *res = r;
    return err;
}

esp_err_t orb_https_get(orb_https_t *h, const char *path, orb_https_sink_fn sink, void *ctx,
                        orb_https_result_t *res)
{
    return orb_https_request(h, ORB_HTTPS_GET, path, NULL, 0, sink, ctx, res);
}

const char *orb_https_resp_headers(const orb_https_t *h) { return h ? h->hdrs : ""; }

esp_err_t orb_https_del_header(orb_https_t *h, const char *name)
{
    if (!h || !h->c || !name) return ESP_ERR_INVALID_ARG;
    return esp_http_client_delete_header(h->c, name);
}
