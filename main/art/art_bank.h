/**
 * art_bank.h — the clock art bank, resident in the `assets` flash partition.
 *
 * The bank (GIF keyframe + delta rects + cropped planar-RGB565A8 hand sprites,
 * produced by tools/bake_clock.py into assets/clock_assets.bin) used to be
 * embedded in the app image via target_add_binary_data(), which meant it was
 * linked into .rodata and shipped inside every OTA payload. It now lives in its
 * own `assets` data partition and is mapped once at boot with
 * esp_partition_mmap(); clock_view.c's blob() returns the mapped base, so every
 * sprite read is a plain flash-mapped pointer dereference exactly as before.
 *
 * Why this matters beyond a tidier app image:
 *   - OTA payloads drop from ~2.6 MB to the ~0.74 MB of actual code, so the two
 *     app slots can be 2 MB each and the freed flash becomes theme storage.
 *   - Themes stop being a firmware release. Installing one is a partition write
 *     plus a remap, not a reflash.
 *   - The map is byte-addressable (ESP_PARTITION_MMAP_DATA / Vaddr0), which the
 *     unaligned sprite offsets require; the instruction MMU would not.
 *
 * Lifetime: mapped in art_bank_init() and deliberately never unmapped — the
 * LVGL image descriptors built in clock_view.c cache raw pointers into it, so
 * the mapping must outlive every screen. Call it before clock_view_init().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ART_BANK_PARTITION_NAME "assets"

/**
 * Find the `assets` partition and map the first `need_bytes` of it.
 *
 * Only `need_bytes` is mapped, not the whole partition: MMU entries in the DROM
 * window are shared with the app's own .rodata, and the 8 MB partition is far
 * larger than any single theme's bank.
 *
 * Idempotent — a second call with a size the current mapping already covers is a
 * no-op. Returns ESP_ERR_NOT_FOUND if the partition is absent (old partition
 * table still on the device), ESP_ERR_INVALID_SIZE if it is too small for the
 * theme, or the esp_partition_mmap failure code.
 */
esp_err_t art_bank_init(size_t need_bytes);

/** Mapped base pointer, or NULL if art_bank_init() has not succeeded. */
const uint8_t *art_bank_ptr(void);

/** Bytes currently mapped; 0 until art_bank_init() succeeds. */
size_t art_bank_bytes(void);

/** True when the bank is mapped and safe to read. */
bool art_bank_ready(void);
