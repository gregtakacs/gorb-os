# The theme pool as a cache: the card holds the themes, the flash holds the ones in use

Status: **ruled 2026-11-08 (§0), nothing built.** Follows theme-pool-plan.md (the pool, built
2026-11-04).

The owner (2026-11-08): "Is there a way to quick load/unload themes from sd card to flash so
the dormant apps do not use it?" and then: "the resident theme flash caching rather than
constant." Today every app's chosen theme lives in the 8 MB `assets` pool for good, so the
shelf's four defaults together must fit in 8 MB (Skeleton 3.48, Scope flights 1.09, weather
1.31, radar 1.91), and a fifth app, or a heavier theme, has no room.

## 0. Owner rulings, 2026-11-08

1. **A fill is live: absolutely no reboot.** An app whose theme left the pool opens and its
   theme comes back while it is on the glass.
2. **While it fills, the app's preview**, held in memory, at 300 px, with the blue progress
   slice around it that a theme preview shows while it loads (theme_strip.c §61's ring),
   **then the zoom into the real app** (the app carousel's, orb_strip.c).
3. **Least recently on the glass goes first; nothing is pinned.**
4. **Skeleton stays the factory clock.**
5. **A fill pauses when the owner leaves the app**, and resumes when the app comes back
   (2026-11-08, after C0).
6. **Sound plays off the glass.** The owner, 2026-11-08: Gold, set to alternate between the
   clock and weather, swapped to weather in the middle of a chime and cut it off. "Background
   sound should be allowed while an app is not on the display. I think yes." So a theme has
   **two tiers**: what an app needs off the glass (its sounds), **always resident**, and what
   it needs only on the glass (its art), which the cache may drop (§2.6). Settled before the
   cache is built, because it changes what an eviction may take. Its details, ruled the same
   day: **one-shots play on and may start off the glass; loops (the clock's ticking) only on
   the glass**; **a voice bank is capped at 1 MB an app**; **the display tier stays until a
   fill needs its room** (least recently on the glass first), never dropped just for leaving.

## 1. What is there today (read 2026-11-08)

- **The pool** (`main/art/orb_pool.c`, theme-pool-plan §2): up to 8 entries, one per app,
  packed from offset 0, its table in two 4 KB copies at the partition's end. An entry is
  the app's stored theme; there is no other record of which theme an app wears.
- **A store** (`orb_theme_install_bundle`): fit checked first (`orb_pool_fits`), the dial
  parked, the art unmapped, the app's old entry removed and the rest compacted, the card
  file copied to the packed run's end, CRC'd back, the table written, **reboot**. About
  6.5 s/MB (Speedster 1.47 MB 9.9 s; Skeleton 2.8 MB 18.4 s).
- **A bind** (`orb_theme_bind`, theme-pool-plan P4) maps **only the bound app's entry**
  (`art_bank_map` unmaps the last). The bound theme is the app on the glass. No other
  entry has a pointer into it: moving or erasing an unbound entry needs no reboot.
- **Flash writes stall both cores.** Code runs from flash (no XIP from PSRAM), so every
  erase and program disables the cache. Erases yield every 20 ms
  (`CONFIG_SPI_FLASH_YIELD_DURING_ERASE`), so a copy under a running dial is a stutter,
  not a freeze. Never measured on the glass.
- **What a theme costs in PSRAM** is the bound theme's alone (its projected tables, label
  planes, the glass's expanded disc, the clock's delta canvas): the cache changes none of
  it. This work frees **flash**, not PSRAM.

## 2. Design

### 2.1 The record and the cache

- **Each app's choice becomes its own record**, kept in NVS beside the pool: the card stem
  and the file's CRC. The choice survives its entry leaving the pool. A store writes it;
  a delete of the chosen file clears it.
- **The pool holds copies.** An entry may be **evicted**: it leaves the table and the run
  is compacted; the card file and the choice stay. The packed-pool ruling holds: an
  eviction compacts like a removal.
- **Least recently on the glass goes first.** The order of last binds lives in RAM, saved
  to NVS on an eviction and at most every few minutes otherwise: the table's 128 B entry
  has no spare word, and an app switch should cost no flash write. After a power cut the
  order is the last saved one; an app it does not name counts as older than any it does.
  The app on the glass is never evicted.

### 2.2 Making room

`orb_pool_reserve_lru(slug, size, order, keep)`: the evictions are chosen from the table
first (`orb_pool_evictions`, pure), least recent first, never `keep`; if even every entry it
may evict leaves no room, it refuses with nothing touched. Else every eviction is one table
write, then the compaction. The compaction may move `keep`, so nothing may read the pool
through a mapping while it runs: a fill runs with nothing bound (§2.4).

### 2.3 A store

The fit check becomes **"this theme fits the pool"**, not "it fits beside the others":
the store makes room by eviction. The store keeps its park and reboot (it may move the
bound entry). The shelf's budget checks (`bake_check.py`, `rebake_shelf.py`) become one
theme against 8 MB.

### 2.4 A fill on arrival

An app whose choice is recorded but not in the pool arrives behind **its preview, 300 px,
with the progress slice** (ruling 2), and the OS fills its entry from the card on the
worker: make room, copy, CRC back, write the table. No reboot: nothing unbound has
pointers, and the arriving app is not bound yet. When the entry lands, the stage binds it,
and the preview **zooms into the app** as the carousel's does. The app runs bare behind the
preview meanwhile and finds its theme at its next arrival (the path a theme sent from the
editor already takes: the app re-reads its stage at the first frame after an arrival).

- **The preview** is already in memory: the app carousel keeps each app's deflated preview
  (`orb_strip.c` `pv_t.z`), read from the **card** by the chosen stem, never from the pool,
  so an evicted theme keeps its picture. The fill screen inflates it at 300 px.
- **Leaving the app mid-fill pauses the fill** (proposed after C0, §6): a fill under a live
  app halves its frame rate however it is paced, so it runs only while the preview is up
  and resumes when the app comes back. A card that is out, or a file that is gone, drops
  the preview and leaves the app bare; its settings page says why.
- **The fill's shape** (from C0): a 64 KB block erased, then its sixteen sectors written,
  block by block, so the ring keeps moving through what was a 2.5 s erase up front; the
  whole fill marked as heavy I/O (`orb_io_heavy_*`), so a guest call the flash stalls is
  forgiven its budget rather than trapped.

### 2.5 Boot

Unchanged, except that the app the orb opens may have no entry: it opens bare and fills.

### 2.6 Two tiers: the voice bank and the display entry (ruling 6)

**Why the chime stopped** (read 2026-11-08): three things end a sound when its app leaves
the glass.
1. The stage's release calls `orb_sound_stop_all()`: voices read their PCM out of the bound
   theme's mapping, which the next bind replaces (D39).
2. Every voice is `ORB_SOUND_LEASED`, and only the stage's tick renews the lease: 1.5 s
   after the app leaves, its voices lapse.
3. A sound starts only through the stage (`orb_stage_snd_*`), gated on the stage being bound
   to the app: an app off the glass cannot start one. (It does run: every app gets
   `orb_service` on and off the glass.)

**What a theme holds, by tier** (the shelf, 2026-11-08): Skeleton 3.4 MB, of which its six
sounds are **844 KB**; flights, weather and the radar ship no sounds. Nothing else a theme
carries is read off the glass (its values blob draws; the preview is the card's).

**The design:**
- **Tier 1, the voice bank:** at a store, the OS copies the theme's SOUND elements and their
  table into a **second pool entry for the app**, its voice bank. It is never evicted while
  the theme is the app's choice; it goes when the choice changes, the file is deleted or
  the app is uninstalled. The pool keeps every voice bank first, packed, and the display
  entries after them, so evictions and fills never move a voice bank; only a store does,
  and a store parks and reboots.
- **The voice banks stay mapped** from boot (Skeleton's is 14 of the 64 KB MMU pages), so a
  voice reads its PCM wherever the stage is. A bind no longer stops voices (D39 is about
  the display entry only).
- **Tier 2, the display entry:** everything else, the cache's to keep or drop (§2.1-2.4). A
  fill copies the display tier alone, so Skeleton fills 2.6 MB, not 3.4.
- **Sounds start off the stage:** a sound import that answers by the app's own voice bank,
  bound or not, under the `sound` cap, the OS volume and quiet hours as today. The clock's
  chime decision moves from its tick to its service, so it chimes on the hour whatever is
  on the glass.
- **Leases by kind** (ruled): a one-shot (a chime, a strike, an alert) plays to its end and may start
  off the glass; a loop (the clock's ticking) lives only while its app is on the glass.
- **The table** grows a kind per entry (voice, display) and room for both per app: format
  2, sixteen entries (32 B head + 16 × 128 B, still one sector), with a carry from format 1.
- **The budget:** the voice banks are permanent pool residents, so each is capped at **1 MB
  an app** (ruled); a display entry must fit the pool beside every voice bank.

## 3. Costs

- **A fill:** ~6.5 s/MB, so ~2-4 s for a 1 MB theme, ~20 s for Skeleton, plus the
  eviction's compaction when it moves another entry (up to ~6.5 s/MB moved). The glass
  shows the preview and its slice, so the stutter matters only for the slice's pace (C0
  measures it).
- **Wear:** a fill and an eviction cost erases only when the pool turns over; an app switch
  costs none. With four apps that fit, nothing is ever evicted.
- **Size:** each theme must fit the pool alone (8 MB less the tables).

## 4. Slices

| Slice | Content | Gate |
|---|---|---|
| **C0** | Measure a live copy on Bronze: a card→pool copy with the radar's sweep on the glass (fps, longest stall, copy time) | numbers in §6 before C3 is built — **measured 2026-11-08** (§6) |
| **C1** | `orb_pool_evictions`, `orb_pool_reserve_lru` (pure C; the table format unchanged) | `host_pool_test.py`: the order, keep, the table's slots, refusals untouched, a cut at every step of two fills — **built 2026-11-08** (§6) |
| **C2** | The choice record (NVS) and the last-on-glass order (RAM, saved lazily); stores make room instead of refusing; the shelf's budgets per theme | bench: a fifth theme stored into a full pool evicts the least recent; host suites |
| **C3** | The fill on arrival behind the preview and its slice, the zoom into the app, the card-missing case | bench: four apps cycled through a pool that holds three; a card pulled mid-fill |
| **C4** | The hub: "in the pool / on the card only" on the Themes page | the hub's lists still parse |

## 5. Open

- **A store still reboots** (it may move the bound entry). Ruling 1 names fills; whether a
  store goes reboot-free too (unbind, store, rebind) is the owner's call after C3.

- **A trap not reproduced:** on the C0 bench day the radar failed with two faults, "wasm
  trapped in orb_input", at the first knob turns after a reboot, before any fill; three
  repeats of the same sequence did not trap. The fault record now names m3's trap message
  (`orb_input: [trap] …`), so the next one says what it was.

## 6. As built

### C0 — a fill under a running dial (2026-11-08, Bronze, XMC flash 0x204018)

`GET /api/v1/debug/fill?id=<stem>&chunk=<KB>&pace=<ms>` (debug builds) copies a card theme
into the pool's free space as a fill would and never writes the table; `?result=1` answers
the last run's line. The radar's Scope (1,762,348 B) with the radar on the glass, its loop
held (20 fps), 1 s windows from the dial bench:

| | erase | copy (card / flash) | read-back | in all | the glass meanwhile |
|---|---|---|---|---|---|
| full speed, 32 KB | 2.5 s | 8.0 s (1.5 / 6.5) | 0.3 s | 10.5 s, 6.2 s/MB | 0 fps through the erase, 5-6 fps through the copy |
| a sector at a time, 20 ms between | - | ~20 s | 0.3 s | ~20 s | 7-11 fps |
| a sector at a time, 50 ms between | - | 37.2 s (1.3 / 14.5) | 0.3 s | 37.5 s, 22 s/MB | 8-13 fps |

- **The card is not the cost:** 1.3-1.5 s of every fill. The flash is: ~6.5 s/MB written,
  and every erase and program stops both cores' cache. A sector's erase and program is
  ~33 ms of stall, two thirds of a 20 fps frame, so no pacing keeps a live sweep at rate.
- **What it means for the design:** a fill behind the static preview and its ring can run at
  full speed (a 1.76 MB theme in ~11 s, Skeleton's 3.48 MB in ~22 s); a fill under a live app
  costs that app half its frames for as long as it runs, so it pauses (§2.4).
- The read-back matched every run; the table was never touched; the copies sit in space the
  pool calls free.
- **Flash auto-suspend, tried and dropped** (the owner: "you have an orb on usb, try away").
  Bronze's flash qualifies by ESP-IDF's own test: esptool's `read-flash-sfdp` read a valid
  table ("SFDP", revision 1.6) with byte 0x32 = 0xF9, bit 3 set, the XMC D-series mark
  `spi_flash_chip_generic_get_caps` grants suspend on. With `CONFIG_SPI_FLASH_AUTO_SUSPEND`
  the board booted and every copy read back right, but the dial did worse: the full-speed
  erase took 7.7 s at 0-1 fps (a 1 s commit), and a sector cost ~60 ms of flash time instead
  of ~33, the paced fills at 5-9 fps (20 ms) and 3-6 fps (1 ms). Each cache miss suspends
  and resumes the operation, so the operation stretches and the frame waits on it.
  Reverted; Bronze is back on the build without it.

### C1 — the eviction (2026-11-08)

`main/art/orb_pool.c`: `orb_pool_evictions` (pure) and `orb_pool_reserve_lru`, beside
`orb_pool_reserve`, which stays as the store's for now. `tools/host_pool_test.py` gained the
`lru` scenarios (least recent first; an unnamed app oldest, in pool order; `keep` passed over,
or a refusal with flash untouched; the app's own old entry uncounted; a ninth app evicting by
the table's slots; a theme bigger than the pool) and two cut runs: a fill evicting two themes
(4,100 steps, each cut again in its repair, no other theme lost) and one evicting the small
first theme, so the clock slides over itself (4,104 steps; a cut can cost the sliding theme,
the pool's promise for any move shorter than the theme, theme-pool-plan §2.2, and no other).
Four new mutants (keep, the order, unnamed oldest, the table's slots): 13 of 13 caught.
