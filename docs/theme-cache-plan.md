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
- Leaving the app mid-fill lets the fill finish. A card that is out, or a file that is
  gone, drops the preview and leaves the app bare; its settings page says why.

### 2.5 Boot

Unchanged, except that the app the orb opens may have no entry: it opens bare and fills.

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
| **C0** | Measure a live copy on Bronze: a 1.3 MB card→pool copy with the radar's sweep on the glass (fps, longest stall, copy time) | numbers in §6 before C3 is built |
| **C1** | `orb_pool_evictions`, `orb_pool_reserve_lru` (pure C; the table format unchanged) | `host_pool_test.py`: the order, keep, the table's slots, refusals untouched, a cut at every step of two fills — **built 2026-11-08** (§6) |
| **C2** | The choice record (NVS) and the last-on-glass order (RAM, saved lazily); stores make room instead of refusing; the shelf's budgets per theme | bench: a fifth theme stored into a full pool evicts the least recent; host suites |
| **C3** | The fill on arrival behind the preview and its slice, the zoom into the app, the card-missing case | bench: four apps cycled through a pool that holds three; a card pulled mid-fill |
| **C4** | The hub: "in the pool / on the card only" on the Themes page | the hub's lists still parse |

## 5. Open

- **A store still reboots** (it may move the bound entry). Ruling 1 names fills; whether a
  store goes reboot-free too (unbind, store, rebind) is the owner's call after C3.

## 6. As built

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
