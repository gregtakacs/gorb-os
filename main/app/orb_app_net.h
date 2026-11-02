#pragma once
/**
 * orb_app_net — the network service behind a wasm app's net_* imports, and the
 * per-app credential store (docs/app-platform-plan.md §43).
 *
 * One HTTPS GET at a time per app, run by one OS task off the UI task, over the same
 * orb_https client and the same admission gate the flight feed used when it lived in
 * the firmware: no request opens a socket while the network is down, a job is running,
 * the card is mid-session, or the internal heap is too thin for lwIP's buffers. The
 * body lands in a PSRAM buffer the app reads in pieces and frees.
 *
 * Credentials never cross into an app. The owner stores one (a header name and a
 * value) per app through the LAN door; an app asks for it to be attached to a request
 * by flag, and only ever learns whether one is set. The flight tracker's adsb.lol key
 * is the first, and /api/v1/flights/key stays its door.
 *
 * The request-side calls run on the LVGL task (from the wasm shim); the work runs on
 * the service's own task. The state word is the only thing the two share in flight.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

struct orb_app;

enum { ORB_NET_IDLE = 0, ORB_NET_BUSY = 1, ORB_NET_DONE = 2, ORB_NET_FAILED = 3 };

#define ORB_APP_NET_BODY_MAX  (256u * 1024u)   /* a body past this fails, never truncates */

typedef struct {
    int32_t  state;
    int32_t  status;
    uint32_t len;
    uint32_t retry_after_s;
    uint32_t total_ms;
    int32_t  err;
} orb_app_net_info_t;

#define ORB_APP_NET_HDRS_MAX 255             /* a request's own header lines */
#define ORB_APP_NET_REQ_MAX  (16u * 1024u)    /* a request body */
#define ORB_APP_JSON_BUF_MAX (16u * 1024u)    /* an app buffer handed to the reader */

/** Queue a request for `slot` (D46): method ORB_HTTPS_* (orb_https.h), a full http://
 *  or https:// URL, optional "Name: value\n" header lines, an optional body, and
 *  optionally the id of one of the app's declared secrets (§43) to attach: refused
 *  (ESP_ERR_NOT_ALLOWED) unless the URL is https to the host the secret names, and
 *  (ESP_ERR_INVALID_STATE) while unset or while a request is already in flight. */
esp_err_t orb_app_net_request(int slot, const struct orb_app *app, uint32_t method,
                              const char *url, const char *headers, const void *body,
                              uint32_t body_len, const char *cred);
/** One response header's value into out; answers its full length, or -1. */
int32_t   orb_app_net_header(int slot, const char *name, char *out, uint32_t cap);
void      orb_app_net_info(int slot, orb_app_net_info_t *out);
/** Copy up to `cap` body bytes from `off`; answers the count, 0 past the end. */
uint32_t  orb_app_net_read(int slot, uint32_t off, void *dst, uint32_t cap);
/** Drop the body (the kept connection stays for the next request). */
void      orb_app_net_free(int slot);
/** Release everything the slot holds, the connection included. Idempotent. */
void      orb_app_net_release(int slot);

/* ---- JSON (D46): orb_json.c over the landed body (buf NULL) or a copied buffer ---- */
#include "orb_json.h"
esp_err_t orb_app_json_begin(int slot, const void *buf, uint32_t len);
int32_t   orb_app_json_next(int slot, orb_json_tok_t *t);
int32_t   orb_app_json_skip(int slot);

/* ---- secrets (§43): an app's declared ORB_SET_SECRET settings ------------------ */

#define ORB_APP_CRED_MAX      128
#define ORB_APP_CRED_HDR_MAX  32

/** Store (or, with an empty key, clear) the app's secret `id`. NULL on success, else
 *  the refusal sentence. Moves the app's cfg generation. */
const char *orb_app_cred_set(const struct orb_app *app, const char *id, const char *key,
                             const char *header);
/** Read every declared secret of `app` into the RAM copy requests answer from.
 *  Internal-stack callers only (it reads NVS): the bind at boot and the enable door. */
void        orb_app_cred_preload(const struct orb_app *app);
/** From the RAM copy: whether the app's secret `id` is set. */
bool        orb_app_cred_has(const struct orb_app *app, const char *id);
/** Erase every stored secret no installed app declares (§43.3). Boot ladder only,
 *  after resolve_boot: it walks the card table and reads NVS. */
void        orb_app_cred_sweep(void);
/** Erase every declared secret of `app` (the uninstall door). Internal stacks only. */
void        orb_app_cred_forget(const struct orb_app *app);
/** Whether it is set; the header name and the value's last four characters. */
bool        orb_app_cred_info(const struct orb_app *app, const char *id, char *header,
                              size_t hcap, char tail[5]);
