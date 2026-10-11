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

/* M2 bench probe — the residency measurement of docs/app-platform-plan.md §6.
 * Set to 1, rebuild, serial-flash, and read the boot log: it maps the bench's
 * secondary banks CRC-verified, pushes flagship-sized mappings until the page
 * budget refuses one, then raw until the flash MMU itself refuses, and prints
 * free PSRAM at the failing instant. The same one-line compile switch as
 * DISPLAY_BENCH in display/display.h; ships 0 — the numbers it prints belong
 * in §10, not in the shipping image's size. */
#define ORB_ART_M2_PROBE 0

/** Run the M2 protocol once, at boot, after the theme has mapped the primary
 *  bank and before the display comes up: the dial's own fps lines later in the
 *  same boot log are then made with every probed bank still resident, which is
 *  the pass test ("stays resident while the dial runs at tempo"), not a log
 *  saying it mapped. No-op unless ORB_ART_M2_PROBE is 1. */
void art_bank_m2_probe(void);

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

/** ---- residency: the page budget and secondary banks (P2, §3.3 mechanism A) ----
 *
 * The primary bank above is the dial's own. A SECONDARY bank is another app's
 * pixels — mapped from the same `assets` partition, read through
 * art_bank_extra_ptr() by that app's compositor, and unmapped only once its
 * cached LVGL descriptors are gone. Every resident bank costs
 * ceil(bytes / MMU page) pages out of the 256 the S3 gives DROM, shared with
 * the image's own .text/.rodata; the budget below is checked on the running
 * total at every map and every reservation, so no bank is ever granted that
 * overflows the window on the next one.
 */

/** MMU pages a bank of `bytes` costs the flash DROM window: ceil(bytes /
 *  64 KB). Also the round-up callers need before orb_art_reserve(). */
size_t art_bank_pages_for(size_t bytes);

/** Upper bound on simultaneously mapped secondary banks: one per installed
 *  app, capped at the 8-app ruling (B5) — every installed app can hold its
 *  bank resident at boot, and the page budget, not this number, is what runs
 *  out first for flagship-sized art. The §3.2 three-slot flash layout and the
 *  §3.3 mechanism-B arena both live inside this count, not outside it. */
#define ART_BANK_EXTRA_MAX 8

/**
 * Map `need_bytes` of the `assets` partition at `offset` as a secondary bank,
 * kept resident until art_bank_unmap_extra(). `offset` must be a multiple of
 * the 64 KB MMU page (a ragged map spends an entry at each end, which is also
 * why the flash slots are cut on boundaries); `need_bytes` may be ragged and
 * is charged as the whole pages it touches, exactly like the primary.
 *
 * Re-mapping the same offset at a size the entry already covers is a no-op
 * that returns that slot; at a larger size it re-grows that entry in place —
 * the caller owns the question of whether its compositor is still reading
 * the old map, which is the same promise the primary's never-unmap lifetime
 * makes for the dial. On success *out_slot (if not NULL) receives the slot
 * index; pass a reservation of the same page-rounded size first
 * (orb_art_reserve) and this consumes it, so reserve-then-map charges the
 * budget once, not twice.
 *
 * Returns ESP_ERR_NO_MEM if the running page total would exceed the budget
 * (the refusal counter in orb_art_refusals() counts these), ESP_ERR_NOT_FOUND
 * if the partition is absent or every slot holds a bank, ESP_ERR_INVALID_SIZE
 * on a zero size or ragged offset, or the esp_partition_mmap failure code.
 */
esp_err_t art_bank_map_extra(size_t offset, size_t need_bytes, int *out_slot);

/** Mapped base of secondary slot `slot`, or NULL if it is empty or `slot` is
 *  out of range. NULL must mean "this app has no art", not "map it again" —
 *  a second map of a live bank is a second, separately-billed MMU region. */
const uint8_t *art_bank_extra_ptr(int slot);

/** Bytes mapped into secondary slot `slot`; 0 when empty or out of range. */
size_t art_bank_extra_bytes(int slot);

/** How many secondary banks are mapped right now (0..ART_BANK_EXTRA_MAX). */
unsigned art_bank_extra_count(void);

/** Release secondary slot `slot`. Every cached pointer into its mapping must
 *  be gone first — the app's compositor holds that lifetime, exactly as
 *  clock_view.c holds the primary's never-unmap promise. Unmapping a bank
 *  something still reads is a silent garbage paint, the same failure the
 *  primary's header comment has fenced off since it was written. */
esp_err_t art_bank_unmap_extra(int slot);

/** Pages the flash MMU is carrying for art right now: the live primary, every
 *  resident secondary, and every outstanding reservation, each rounded up
 *  separately because each occupies its own virtual region. The image's own
 *  22 pages are NOT in this number — it is what art costs the window. */
size_t art_bank_mmu_pages(void);

/**
 * Account for a bank the app host intends to keep resident, without mapping
 * it: the installer books it at install time, and art_bank_map_extra()
 * converts the booking into the resident bank at boot. Reserving a bank that
 * is then never mapped keeps its pages budgeted — an app that installed and
 * whose art failed to load still holds the window's accounting, which is the
 * honest reading of "how many banks could boot this board right now".
 *
 * It enforces the two rules the residency arithmetic depends on, so an
 * install can be refused with a reason instead of turning into a page fault
 * at boot:
 *
 *  - ESP_ERR_INVALID_SIZE: `bytes` is not a multiple of the 64 KB MMU page. A
 *    ragged request wastes a whole page at each end of the mapping, which is
 *    also why the `assets` slots are cut on 64 KB boundaries.
 *  - ESP_ERR_NO_MEM: the running total across the live banks and all accepted
 *    reservations would exceed the DROM window's 234 budgeted pages (256 less
 *    the 22 the image's own .text and .rodata consume), or the reservation
 *    table is full. The cap is on the total, deliberately: two banks that
 *    each fit alone are what overflows the window. Ask before mapping, and
 *    hold the error — an app that learns the budget at install can say
 *    "install fewer themes"; one that learns it at boot cannot.
 *
 * Call with the PAGE-ROUNDED size, not the file size. No shipped bank is
 * aligned — the flagship on the bench 2026-10-11 is 2,503,747 B, which must
 * be passed as 2,555,904 B — so a caller that forwards an on-disk length gets
 * ESP_ERR_INVALID_SIZE for a perfectly good theme. Round up first:
 * art_bank_pages_for() is the same ceil-by-64-KB the implementation charges
 * for.
 */
esp_err_t orb_art_reserve(size_t bytes);

/** How many banks are resident or reserved — primary, secondaries, and the
 *  reservations not yet converted into a mapping — and the bytes outstanding
 *  in reservations right now (consumed bookings are not counted). */
size_t orb_art_resident_count(void);
size_t orb_art_reserved_bytes(void);

/** How many residency requests — orb_art_reserve() or art_bank_map_extra() —
 *  have been refused for want of MMU pages. */
unsigned orb_art_refusals(void);

/**
 * The `assets` partition itself, found by label — for the erase/write path in
 * orb_theme.c. NULL if the partition is not in the table at all.
 */
const esp_partition_t *art_bank_partition(void);
