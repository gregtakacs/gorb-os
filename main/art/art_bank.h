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
#include "esp_partition.h"   /* esp_partition_t, for art_bank_partition() below */

#define ART_BANK_PARTITION_NAME "assets"

/**
 * Find the `assets` partition and map `need_bytes` of it starting at `offset`.
 *
 * Only `need_bytes` is mapped, not the whole partition: MMU entries in the DROM
 * window are shared with the app's own .rodata, and the 8 MB partition is far
 * larger than any single theme's bank.
 *
 * `offset` exists because a theme installed from the card does not live at the
 * start of the partition — see the A/B slot layout in main/art/orb_theme.h. Both
 * slot payload offsets are 64 KB aligned, which is what esp_partition_mmap wants
 * anyway; passing anything else wastes an MMU entry on rounding at each end, and
 * orb_theme never does.
 *
 * Idempotent — a second call with a size the current mapping already covers is a
 * no-op. Returns ESP_ERR_NOT_FOUND if the partition is absent (old partition
 * table still on the device), ESP_ERR_INVALID_SIZE if it is too small for the
 * theme, or the esp_partition_mmap failure code.
 */
esp_err_t art_bank_init(size_t offset, size_t need_bytes);

/** The traditional layout: the bank at partition offset 0, as esptool writes it. */
static inline esp_err_t art_bank_init_at_0(size_t need_bytes)
{
    return art_bank_init(0, need_bytes);
}

/** Mapped base pointer, or NULL if art_bank_init() has not succeeded. */
const uint8_t *art_bank_ptr(void);

/** Bytes currently mapped; 0 until art_bank_init() succeeds. */
size_t art_bank_bytes(void);

/** True when the bank is mapped and safe to read. */
bool art_bank_ready(void);

/** ---- residency accounting (P1, docs/app-platform-plan.md §3) ----
 *
 * MMU pages a bank of `bytes` costs the flash DROM window: 256 pages exist, and
 * every resident bank plus the app's own .rodata spends from them.
 */
size_t art_bank_pages_for(size_t bytes);

/** Pages the art bank plus all accepted reservations occupy now. */
size_t art_bank_mmu_pages(void);

/**
 * Account for a bank the app host intends to keep resident. Accounts only —
 * mapping secondary banks is P2's job — but it enforces the two rules the residency
 * arithmetic depends on, so an install can be refused with a reason instead of
 * turning into a page fault at boot:
 *
 *  - ESP_ERR_INVALID_SIZE: `bytes` is not a multiple of the 64 KB MMU page. A ragged
 *    request wastes a whole page at each end of the mapping, which is also why the
 *    `assets` slots are cut on 64 KB boundaries.
 *  - ESP_ERR_NO_MEM: the running total across the live bank and all accepted
 *    reservations would exceed the DROM window's 234 budgeted pages (256 less the 22
 *    the image's own .text and .rodata consume). The cap is on the total, deliberately:
 *    two banks that each
 *    fit alone are what overflows the window. Ask before mapping, and hold the error —
 *    an app that learns the budget at install can say "install fewer themes"; one that
 *    learns it at boot cannot.
 */
esp_err_t orb_art_reserve(size_t bytes);

/** How many banks are resident or reserved, and the bytes reserved so far. */
size_t orb_art_resident_count(void);
size_t orb_art_reserved_bytes(void);

/** How many orb_art_reserve() calls have been refused for want of MMU pages. */
unsigned orb_art_refusals(void);

/**
 * The `assets` partition itself, found by label — for the erase/write path in
 * orb_theme.c. NULL if the partition is not in the table at all.
 */
const esp_partition_t *art_bank_partition(void);
