/* Kept bodies (radar-plan.md §2.3): an app keeps a landed response as an OS buffer in
 * PSRAM and names it by a small slot number, because its own 64-128 KB cannot hold a
 * radar's frames. The OS owns the bytes; the app reads them only through OS draws
 * (orb_cv_png8) or by length.
 *
 * 24 slots and 768 KB per app. The blobs live while the app is on the glass and for 15
 * minutes after it leaves, then the OS frees them; uninstall frees them at once.
 *
 * radar-plan §7 (owner, 2026-11-09): a body can also wait on the card. The app parks it
 * (its PSRAM goes once the copy is written) and loads it back before it draws it; a park is
 * the card's last priority (it stands aside for the carousels, an app switch, a job, an
 * upload), a load its next to first. The 768 KB counts PSRAM only, a load from its ask. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ORB_BLOB_N        24
#define ORB_BLOB_BUDGET   (768u * 1024u)
#define ORB_BLOB_LINGER_S (15 * 60)

/* orb_blob_state's answers, as the SDK has them (apps/sdk/orb.h). */
enum { ORB_BLOB_EMPTY = 0, ORB_BLOB_RAM = 1, ORB_BLOB_CARD = 2, ORB_BLOB_MOVING = 3 };

/* Take ownership of a PSRAM buffer as app `app_slot`'s blob `i`, freeing what was there.
 * ESP_ERR_NO_MEM past the budget (the caller keeps the buffer), ESP_ERR_INVALID_ARG for a
 * bad slot or index. */
esp_err_t orb_blob_put(int app_slot, int i, uint8_t *buf, uint32_t len);

/* The blob's bytes and length; NULL and 0 when empty. Valid until the next put or free
 * of that blob, on the task that owns the app's calls. */
const uint8_t *orb_blob_get(int app_slot, int i, uint32_t *len);

/* The body's length in any tier (0 = empty). */
uint32_t orb_blob_len(int app_slot, int i);

void orb_blob_free(int app_slot, int i);

/* Keep body `i` on the card; its PSRAM goes once written. ESP_ERR_NOT_SUPPORTED with no
 * usable card (the body stays in PSRAM), ESP_ERR_NOT_FOUND for an empty slot. LVGL task. */
esp_err_t orb_blob_park(int app_slot, int i);
/* Bring a parked body back to PSRAM. ESP_ERR_NO_MEM past the budget. LVGL task. */
esp_err_t orb_blob_load(int app_slot, int i);
/* ORB_BLOB_EMPTY / _RAM / _CARD / _MOVING. */
int orb_blob_state(int app_slot, int i);
/* What the card worker finished, adopted on the LVGL task: once a service pass. */
void orb_blob_service(int app_slot);
void orb_blob_free_all(int app_slot);
uint32_t orb_blob_used(int app_slot);

/* The app's glass state, once a service pass: frees every blob after ORB_BLOB_LINGER_S
 * off the glass. */
void orb_blob_note_glass(int app_slot, bool on_glass, int64_t now_us);
