/* Kept bodies — contract in orb_app_blob.h. */
#include "orb_app_blob.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "orb_app.h"   /* ORB_APP_CAP */
#include "orb_glass.h"   /* §6.21: a kept body lives in the glass region when it fits */

static const char *TAG = "blob";

typedef struct {
    uint8_t *buf[ORB_BLOB_N];
    uint32_t len[ORB_BLOB_N];
    uint32_t used;
    int64_t  off_since_us;        /* 0 while on the glass or empty */
} app_blobs_t;

/* PSRAM: a few hundred bytes nobody reads on the frame path. */
EXT_RAM_BSS_ATTR static app_blobs_t s_b[ORB_APP_CAP];

static app_blobs_t *of(int app_slot) { return app_slot >= 0 && app_slot < ORB_APP_CAP ? &s_b[app_slot] : NULL; }

esp_err_t orb_blob_put(int app_slot, int i, uint8_t *buf, uint32_t len)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N || !buf) return ESP_ERR_INVALID_ARG;
    if (b->used - b->len[i] + len > ORB_BLOB_BUDGET) return ESP_ERR_NO_MEM;
    orb_glass_free(b->buf[i]);
    b->used = b->used - b->len[i] + len;
    b->buf[i] = buf;
    b->len[i] = len;
    return ESP_OK;
}

const uint8_t *orb_blob_get(int app_slot, int i, uint32_t *len)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N || !b->buf[i]) { if (len) *len = 0; return NULL; }
    if (len) *len = b->len[i];
    return b->buf[i];
}

void orb_blob_free(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N || !b->buf[i]) return;
    orb_glass_free(b->buf[i]);
    b->used -= b->len[i];
    b->buf[i] = NULL;
    b->len[i] = 0;
}

void orb_blob_free_all(int app_slot)
{
    app_blobs_t *b = of(app_slot);
    if (!b) return;
    for (int i = 0; i < ORB_BLOB_N; i++) orb_glass_free(b->buf[i]);
    memset(b, 0, sizeof *b);
}

uint32_t orb_blob_used(int app_slot)
{
    const app_blobs_t *b = of(app_slot);
    return b ? b->used : 0;
}

void orb_blob_note_glass(int app_slot, bool on_glass, int64_t now_us)
{
    app_blobs_t *b = of(app_slot);
    if (!b || !b->used) { if (b) b->off_since_us = 0; return; }
    if (on_glass) { b->off_since_us = 0; return; }
    if (!b->off_since_us) { b->off_since_us = now_us; return; }
    if (now_us - b->off_since_us >= (int64_t)ORB_BLOB_LINGER_S * 1000000) {
        ESP_LOGI(TAG, "slot %d: %u B freed after %d min off the glass", app_slot, (unsigned)b->used,
                 ORB_BLOB_LINGER_S / 60);
        orb_blob_free_all(app_slot);
    }
}
