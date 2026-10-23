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
 * Lifetime: mapped by the theme path before clock_view_init() and never
 * unmapped while the dial reads it — the LVGL image descriptors built in
 * clock_view.c cache raw pointers into it, so the mapping must outlive every
 * screen. One release exists and it is ordered, not opportunistic:
 * art_bank_unmap() runs between the store door's park and its erase, and the
 * reboot the store always asks for re-establishes the map from the bytes on
 * the flash (docs/theme-delta-mmap-plan.md §5.3). The theme's residency is
 * mechanism A at the CALL — orb_theme.c calls art_bank_map() directly — so
 * ORB_ART_MECH_B governs app banks only.
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

/* Suspend/resume reclaim-latency probe — the release-vs-carry question of
 * docs/app-platform-plan.md §6 (M-carrier). Set to 1, rebuild, serial-flash, and
 * read the boot log. It prices the two operations the app-suspension choice
 * turns on and off: art_bank_unmap_extra() is what a suspend RELEASES (an
 * esp_partition_munmap), and art_bank_map_extra() is what a resume RECLAIMS (an
 * esp_partition_mmap). If the dial instead CARRIES a parked bank, neither runs on
 * that switch and the reclaim cost is zero — at the price of holding its pages
 * against the page budget, which is the other half of the decision. mmap bills
 * address ranges, not contents, so a size is all the probe needs of a bank; each
 * bank is mapped at a page-aligned offset clear of the primary and fully unmapped
 * between iterations, so every sample is a genuine cold re-claim, and the repeated
 * steady-state samples model the carousel being switched back to over and over.
 * Read these against §12.7's M1 app-activate figure (63-87 ms): the MMU op below is
 * the ONLY thing the release choice adds on top of that. Same one-line switch as
 * ORB_ART_M2_PROBE and DISPLAY_BENCH; ships 0. */
#define ORB_ART_LAT_PROBE 0

/** Run the reclaim-latency protocol once, at boot, after the theme has mapped
 *  the primary bank and before display_init(). No-op unless ORB_ART_LAT_PROBE 1. */
void art_bank_lat_probe(void);

/* ---- mechanism B: the PSRAM arena (docs/app-platform-plan.md §3.3, §6 M3) ----
 * Set to 1 and art_bank_map_extra() fills a secondary bank by COPYING the bank
 * from the `assets` partition into a PSRAM arena (esp_partition_read, chunked,
 * on the calling task — the worker at boot for the shipping path) instead of
 * claiming flash MMU pages for it. The compositor contract is unchanged:
 * art_bank_extra_ptr() hands out a readable base either way, so an app never
 * learns which mechanism fed its pixels — the §3.3 promise that keeps A and B
 * one measurement apart rather than two code paths. The bank then costs ZERO
 * pages of the DROM window (art_bank_mmu_pages() does not count it; the map
 * never enters it) and costs PSRAM instead: the arena gate is free PSRAM minus
 * ORB_ART_ARENA_MARGIN, the margin the LVGL task, the worker and the stack
 * still need at boot — the M2 line that printed 7.9 MB free is the figure this
 * gate was cut against. An arena bank that will not fit is refused with
 * ESP_ERR_NO_MEM like a page-budget overflow, with the free figure in the log:
 * "refuse the app with a reason and continue booting" (§3.3), not a surprise.
 *
 * The LAT probe priced the MMU op itself and found it scales with pages, not
 * bytes — mechanism B exists for the OTHER half of the cost: every sprite read
 * through the map is a flash-cache miss against the window, where every read
 * from the arena is a plain PSRAM dereference. Which one wins on the glass is
 * the M3 measurement (ORB_ART_M3_PROBE), not a code change: the two M3 builds
 * are this define at 0 and at 1 with everything else held equal.
 *
 * Deliberately compile-time, not a per-bank flag: a mixed regime is P4's
 * installer question (per-theme residency flags), and a runtime switch today
 * would be a second residency table the host mirror cannot pin.
 *
 * D30 (docs/app-platform-plan.md §20.4) lit it for themes too; the theme move
 * to mmap (docs/theme-delta-mmap-plan.md §5.3, C3) took it back off. An
 * installed theme's pixels are MAPPED from boot onward — one art_bank_map()
 * through orb_theme's slot-A door, which verifies the FORMAT 3 header before
 * mapping and can refuse. The define stays 1 because app banks keep the arena
 * (§9's out-of-scope ruling): each is one copy, the count is small, and a
 * revoke must stay able to release a bank without cycling MMU pages. The two
 * M3 probe builds this define priced are still exactly this define at 0 and at
 * 1; the theme's move does not retire the measurement. */
#define ORB_ART_MECH_B 1

/* PSRAM the arena gate keeps off the table on top of the bank itself. 2048 KiB is the
 * shipping decision (D30); the Kconfig option in front of it is §20.5(4)'s bench pin,
 * there so the refusal can be FORCED rather than hoped for. Reading the default from
 * a define rather than a literal is what lets a dev build set it without editing this
 * file, which is the difference between a knob and a mistake. */
#define ORB_ART_ARENA_MARGIN (CONFIG_ORB_ART_ARENA_MARGIN_KB * 1024u)

/* ---- M3 bench probe: overflow bank, re-map (A) vs arena (B) ----------------
 * The §6 M3 protocol, same one-line-switch family as ORB_ART_M2_PROBE,
 * ORB_ART_LAT_PROBE and DISPLAY_BENCH; ships 0. It measures the ONE bank the
 * §3.2 slot layout has no room for — the third flagship, here the kit3 bytes
 * flashed at a slot-free offset of the `assets` partition — on both arms:
 *
 *   arm A (ORB_ART_MECH_B 0): one app switch = art_bank_unmap_extra() +
 *     cold art_bank_map_extra() + the first full-page commit through the
 *     fresh mapping — the page-miss storm the plan's "one full-page commit +
 *     fresh mmap" prices. Read against the LAT probe's map/unmap medians: the
 *     first commit is what the µs-scale MMU op hides on top of.
 *   arm B (ORB_ART_MECH_B 1): the bank is copied once at boot (timed, printed
 *     as the boot cost with free-PSRAM before/after), and a switch is the
 *     re-point the ABI performs around this API — no MMU op, no copy — so its
 *     switch cost is the same first-read pattern WITHOUT the cold mapping.
 *
 * The first-commit pass blends through the very gfx_blend_px() the layers use,
 * at DISPLAY_BENCH_ALPHA 128 (255 prices the wrong op — .clinerules), with the
 * block sources scattered across the WHOLE bank so every one of its 59 pages
 * is touched each pass: a bank half-read from cache measures neither arm. The
 * bank is then left resident, so the dial's own fps lines later in the same
 * boot log — and the DISPLAY_BENCH ladder if it is swept on — are the
 * rotation steady state, exactly as the M2 probe's residency verdict works.
 *
 * The bank must be REAL: unlike the LAT probe (which bills address ranges,
 * not contents), M3 reads the pixels. The probe CRC-checks the mapping against
 * the read path and refuses to measure — loudly, with the esptool command —
 * when the offset holds erased flash, so a forgotten bench write can never
 * turn into a number that looks like the other arm.
 *
 * It also runs m3_refusal_hunt(): §11 left M3 the job of finding the flash
 * MMU allocator's TRUE refusal point (M2 never reached it — the partition ran
 * out first), by mapping www plus overlapping regions of assets and unmapping
 * everything again before the dial starts. Its one line answers whether
 * ORB_ART_PAGE_BUDGET can rise; the budget itself is not touched here. */
#define ORB_ART_M3_PROBE 0

/** Run the A-vs-B overflow protocol once, at boot, after the theme has mapped
 *  the primary bank and before display_init(). No-op unless ORB_ART_M3_PROBE 1. */
void art_bank_m3_probe(void);

/** The resident bench bank's base — the flash map (arm A) or the PSRAM arena
 *  (arm B) — for clock_view.c's DISPLAY_BENCH blocks to read their colours
 *  from, so the steady-state ladder measures the SAME source the probe just
 *  priced. NULL/0/false until the probe has mapped successfully: a bench build
 *  whose plane never arrived logs that fact rather than silently blending its
 *  synthetic mag plane under an "M3" label. */
const uint8_t *art_bank_m3_plane(void);
size_t art_bank_m3_bytes(void);
bool art_bank_m3_is_psram(void);

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

/** The traditional layout: the bank at partition offset 0, by whichever
 *  mechanism ORB_ART_MECH_B lights. */
static inline esp_err_t art_bank_init_at_0(size_t need_bytes)
{
    return art_bank_init(0, need_bytes);
}

/* There is no always-map default entry point any more: §26 slice 2 moved the
 * default leg behind orb_theme's slot-A door, which verifies the FORMAT 3
 * header first and falls to the base render when the bytes there refuse. */

/** The primary bank as a PSRAM arena copy, optionally CRC-verified DURING the
 *  one read pass that fills it — D30's boot refill in its fused shape, the
 *  read-only twin of the flip's dual-sink write (§20.2).
 *
 *  `verify` asks for the checksum: the chunk is read from the partition, CRCed
 *  into `expect_crc`, and copied into the arena in the same loop, so a slot is
 *  proven and resident for one pass over its bytes. A mismatch frees the arena,
 *  leaves the bank unresident, and returns ESP_ERR_INVALID_CRC — a corrupt slot
 *  never reaches the paint path. verify=false is the same copy unchecked, for a
 *  caller that verified elsewhere or has nothing to verify against.
 *
 *  Gates, in order, each refusing with a reason the caller can put on a page:
 *  ESP_ERR_INVALID_ARG on a zero size, ESP_ERR_NOT_FOUND when the `assets`
 *  partition is absent, ESP_ERR_INVALID_SIZE when the request runs past its end,
 *  ESP_ERR_NO_MEM when free PSRAM is under need_bytes + ORB_ART_ARENA_MARGIN or
 *  the allocation itself fails (both name the free figure in the log, and both
 *  bump orb_art_refusals() like a page-budget refusal does). `*out_ms`, when
 *  given, receives the elapsed whole milliseconds of the pass: §20.5(2)'s boot
 *  line reads this number instead of a predicted one.
 *
 *  A previous primary is released first, by whichever mechanism holds it, and
 *  the same caller promise art_bank_init() makes applies — every cached LVGL
 *  descriptor into the old base must be rebuilt before the next paint. */
esp_err_t art_bank_fill(size_t offset, size_t need_bytes, bool verify,
                        uint32_t expect_crc, uint32_t *out_ms);

/** The primary bank by mechanism A whatever ORB_ART_MECH_B says: the mmap form
 *  art_bank_init() had before D30, kept addressable because Arm A is a real
 *  path (§20.2) and because the residency probe measures one arm per build.
 *  Same arguments, same release-and-remap behaviour, same failure codes. */
esp_err_t art_bank_map(size_t offset, size_t need_bytes);

/**
 * Release the primary bank, by whichever mechanism currently holds it, and
 * leave the bank unresident: art_bank_ptr() answers NULL and art_bank_ready()
 * answers false from here until the next map. C3 (theme-delta plan §5.3)
 * exists: the erase must not happen under a live map, because a cached DROM
 * page does not notice an erase under it and the store would reboot into a
 * stale plane. The store door therefore parks the dial, calls this, and only
 * then erases — and it never remaps in place; the reboot the store asks for
 * re-establishes the map from the bytes on the flash.
 *
 * The caller owns the promise, exactly like unmap_extra's: every cached
 * pointer into the mapping must be dead first. For the primary that means the
 * clock is PARKED (the tick timer paused and the dial's layer table emptied),
 * not merely hidden. Idempotent: an unresident bank returns ESP_OK.
 */
esp_err_t art_bank_unmap(void);

/** The whole DROM page budget for art: 256 window pages less the 22 the image's
 *  own .text and .rodata consume. The refusal sentence an install must name
 *  ("needs N pages, M free") cannot be built from art_bank_mmu_pages() alone. */
size_t art_bank_page_budget(void);

/** Budgeted pages currently unclaimed: art_bank_page_budget() less
 *  art_bank_mmu_pages(). What a reservation of this many pages-or-under is
 *  granted, and nothing bigger. */
size_t art_bank_pages_free(void);

/** Mapped base pointer, or NULL if art_bank_init() has not succeeded. */
const uint8_t *art_bank_ptr(void);

/** Bytes currently mapped; 0 until art_bank_init() succeeds. */
size_t art_bank_bytes(void);

/** True when the bank is resident and safe to read, by either mechanism. */
bool art_bank_ready(void);

/** Whole milliseconds of the last art_bank_fill() pass; 0 before the first one.
 *  Only a fused refill sets it, so a nonzero figure beside art_bank_is_arena()
 *  true is a boot that refilled from a slot, not a boot that mapped. D30's
 *  §20.5(2) number reaches the ledger through this, not through a prediction. */
uint32_t art_bank_fill_ms(void);

/** True when the dial's pixels are the PSRAM arena rather than a flash map. */
bool art_bank_is_arena(void);

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

/**
 * Account for a bank a caller intends to MAP, by its raw span: the installer's
 * shape. Rounds the span up to whole MMU pages first (the same ceil the mapping
 * itself is charged for) and then books it through orb_art_reserve(), so a
 * caller that holds a file or partition length never has to restate the page
 * size. Returns the reserve's own codes — ESP_ERR_NO_MEM names the shortage
 * through art_bank_pages_free(); art_bank_pages_for(span) names the ask.
 */
esp_err_t art_bank_reserve_span(size_t span_bytes);

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
