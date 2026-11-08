#pragma once
/* The OS's outbound HTTPS client (docs/flight-tracker-design.md §4) — what
 * orb_svc_t.http_get will sit on when an app's `net` capability is wired.
 *
 * One connection per caller, kept alive between calls when the server agrees,
 * so a poll every few seconds pays one TLS handshake, not one per poll. The
 * body is never held: it streams to the caller's sink in socket-sized pieces.
 *
 * Memory: with CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC the TLS session's buffers
 * (16 KB in, 4 KB out, plus the handshake's working set) live in PSRAM, so a
 * connection costs the internal pool only what lwIP and the radio spend on its
 * packets. The call samples the internal pool while it runs, so every result
 * says what it actually cost the display's neighbours. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Return false to stop reading; the connection is then closed, not reused. */
/* buf NULL: the body's length, told once before it comes, when the server gave one. */
typedef bool (*orb_https_sink_fn)(const char *buf, size_t len, void *ctx);

typedef struct orb_https orb_https_t;

typedef struct {
    esp_err_t err;            /* ESP_OK when a status line arrived            */
    int       status;         /* HTTP status, 0 on transport failure          */
    int64_t   content_len;    /* -1 when chunked or absent                    */
    size_t    bytes;          /* body bytes handed to the sink                */
    bool      complete;       /* the whole body arrived                       */
    bool      reused;         /* no new connection (and so no handshake)      */
    uint32_t  open_ms;        /* connect + handshake + request sent           */
    uint32_t  ttfb_ms;        /* request sent -> headers parsed               */
    uint32_t  total_ms;
    size_t    int_free_min;   /* internal heap, lowest seen during the call   */
    size_t    int_largest_min;/* internal largest free block, lowest seen     */
    uint32_t  retry_after_s;  /* the response's Retry-After seconds, 0 = none */
} orb_https_result_t;

/* host is the whole origin, "https://api.adsb.lol". keep_alive=false closes
 * after every call (the bench's handshake-per-poll arm). */
orb_https_t *orb_https_new(const char *origin, const char *user_agent, bool keep_alive,
                           uint32_t timeout_ms);
void         orb_https_free(orb_https_t *h);

/* GET origin+path. Retries once on a fresh connection when a reused one turns
 * out to have been closed by the server while idle. */
esp_err_t orb_https_get(orb_https_t *h, const char *path, orb_https_sink_fn sink, void *ctx,
                        orb_https_result_t *res);

/* One more request header on every call this client makes (an API key). The
 * value is copied. Set it before the first call; a changed value means a new
 * client, so a kept connection never carries the old one. */
esp_err_t    orb_https_set_header(orb_https_t *h, const char *name, const char *value);
esp_err_t    orb_https_del_header(orb_https_t *h, const char *name);

/* D51: any method, an optional request body, every status's body to the sink, and the
 * response's headers kept ("name: value\n" lines, 768 B) until the next request.
 * The origin may be http:// as well as https://. orb_https_get is GET with no body. */
typedef enum {
    ORB_HTTPS_GET = 0, ORB_HTTPS_POST, ORB_HTTPS_PUT, ORB_HTTPS_DELETE, ORB_HTTPS_HEAD
} orb_https_method_t;
esp_err_t orb_https_request(orb_https_t *h, orb_https_method_t method, const char *path,
                            const void *body, size_t body_len,
                            orb_https_sink_fn sink, void *ctx, orb_https_result_t *res);
const char *orb_https_resp_headers(const orb_https_t *h);

/* Connections opened since orb_https_new (each one a handshake). */
uint32_t orb_https_opens(const orb_https_t *h);
