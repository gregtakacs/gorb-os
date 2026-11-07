/* Kept bodies (radar-plan.md §2.3): an app keeps a landed response as an OS buffer in
 * PSRAM and names it by a small slot number, because its own 64-128 KB cannot hold a
 * radar's frames. The OS owns the bytes; the app reads them only through OS draws
 * (orb_cv_png8) or by length.
 *
 * 24 slots and 768 KB per app. The blobs live while the app is on the glass and for 15
 * minutes after it leaves, then the OS frees them; uninstall frees them at once. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ORB_BLOB_N        24
#define ORB_BLOB_BUDGET   (768u * 1024u)
#define ORB_BLOB_LINGER_S (15 * 60)

/* Take ownership of a PSRAM buffer as app `app_slot`'s blob `i`, freeing what was there.
 * ESP_ERR_NO_MEM past the budget (the caller keeps the buffer), ESP_ERR_INVALID_ARG for a
 * bad slot or index. */
esp_err_t orb_blob_put(int app_slot, int i, uint8_t *buf, uint32_t len);

/* The blob's bytes and length; NULL and 0 when empty. Valid until the next put or free
 * of that blob, on the task that owns the app's calls. */
const uint8_t *orb_blob_get(int app_slot, int i, uint32_t *len);

void orb_blob_free(int app_slot, int i);
void orb_blob_free_all(int app_slot);
uint32_t orb_blob_used(int app_slot);

/* The app's glass state, once a service pass: frees every blob after ORB_BLOB_LINGER_S
 * off the glass. */
void orb_blob_note_glass(int app_slot, bool on_glass, int64_t now_us);
