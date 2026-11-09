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

7. **The flash copies follow the card file** (2026-11-08). An app's voice bank and display
   entry are both made from one card file and carry its identity (the header's geometry
   CRC: `src_crc` in the bank, `file_crc` in the entry). A store builds both, so every send,
   editor commit or overwrite rebuilds them; at boot and when a card is mounted, an app whose
   card file no longer matches is rebuilt from the card, and one whose file is gone loses
   both copies.
8. **No migration: old flash copies are refused** (2026-11-08). A theme stored before voice
   banks, whose theme carries sounds, does not bind: the app runs bare and its log says to
   send the theme again, which rebuilds both halves. A theme with no sounds lacks nothing and
   binds as before. The `.orbtheme` files on the card stay valid; no format bump.

9. **The sound limit** (2026-11-08): an app sounds at most **four** at once (Skeleton at the hour
   on the glass: the tick and three ringing strikes), and its fifth replaces its own oldest
   one-shot; a sound queued for later waits in a schedule (32 an app), not in one of the
   engine's 16 voices. If every voice is ever busy, the oldest one-shot of the app holding
   the most gives way. No priority scheme beyond that.

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
- **The fill's shape** (from C0): a 64 KB block erased ahead of its writes, block by block,
  so the ring keeps moving through what was a 2.5 s erase up front; the whole fill marked as
  heavy I/O (`orb_io_heavy_*`), so a guest call the flash stalls is forgiven its budget
  rather than trapped. Built in C5b (§6), with the card let go whenever the glass owes a
  frame.

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
| **C2** | The table's format 2 (an entry's kind, voice or display; sixteen entries; the carry from format 1); voice banks packed first, never moved by an eviction | `host_pool_test.py`: the carry, evictions that never take or move a voice bank, a cut at every step of a voice bank's store — **built 2026-11-08** (§6) |
| **C3** | Sound off the glass: a store writes the voice bank (≤ 1 MB), boot maps every voice bank, a bind no longer stops voices, one-shots unleased and loops leased to the glass, a sound import that needs no stage; the clock chimes from its service | bench: the clock chiming through a switch to weather, and on the hour with weather on the glass; the tick stopping when the clock leaves — **built and benched on Bronze 2026-11-08** (§6) |
| **C4** | The choice record (NVS), the last-on-glass order, stores that make room instead of refusing, the shelf's budgets per theme | bench: a theme stored into a full pool evicts the least recent display entry — **built and benched on Bronze 2026-11-08** (§6) |
| **C5** | The fill on arrival behind the preview and its slice (full speed, block by block, heavy I/O), the zoom into the app, the pause on leaving, the card-missing case | bench: four apps cycled through a pool that holds three; leaving mid-fill; a card pulled mid-fill — **C5a, the engine, C5b, the face and the pause, C5c, the card file followed, built 2026-11-08** (§6); a card pulled mid-fill benched by the owner 2026-11-09 (§6) |
| **C6** | The hub: "in the pool / on the card only" on the Themes page | the hub's lists still parse — **built 2026-11-09** (§6) |

## 5. Open

- **An arrival is mostly moving, not copying** (C5c, §6): evicting an entry in the middle
  of the packed pool moves every entry above it down before the copy starts, ~30 s of a
  40-55 s arrival when Skeleton's 3.4 MB sits above the hole. A fill placed in the hole an
  eviction leaves (first fit, the pool no longer packed) would cost the copy alone, ~10 s.
  It changes the pool's invariant (`orb_pool_commit` wants a display entry at the used end)
  and its cut-at-every-step tests, so it is a slice of its own: proposed as **C7**.
- **A store whose voice bank changes size moves every display entry twice** (C5c's bench:
  Skeleton stored over Neon Moon took ~3 min of "making room"): the display entry is placed
  first, then the voice bank is placed at the front and lifts every display entry above it.
  Writing the voice bank first would move them once. Same slice.
- **C7's two shapes** (owner's question, 2026-11-09: how would it work without moving?):
  *first fit* — entries stay where they land, the table keeps the holes, a fill takes the
  first hole big enough, and the eviction drops least-recent entries until one hole (two
  neighbours freed together merge) fits; when the free bytes are enough but no one hole is,
  it falls back to today's compaction. *Scattered pages* — the S3's MMU maps flash in 64 KB
  pages, and every entry is already 64 KB-aligned, so an entry could be stored as a list of
  free pages anywhere and mapped into one contiguous window page by page: no hole is ever too
  small and nothing ever moves. It costs a page list per entry in the table (a 3.4 MB theme
  is 54 pages; 128 pages in the pool) and a mapper that maps pages one by one instead of
  `esp_partition_mmap`'s single range. First fit is the smaller change; scattered pages is
  the one that never moves anything.

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

### The owner's card pull (2026-11-09)

- **Pulled mid-fill:** the fill failed at its next read, the face grew into bare radar, and
  its Themes row said "copy failed". As designed, except the words: the card module still
  called the card mounted.
- **Pushed back, radar opened again: still bare.** The card module kept its mount over the
  dead handle, so every read timed out (0x107) until a reboot. A mounted card is now asked
  its status (`sdmmc_get_status`) at most every 2 s when a caller asks for the mount; one
  that does not answer is unmounted and mounted afresh at once. The remount counts as a
  mount, so the reconcile runs again. **Re-benched by the owner the same day: passed** (a
  pull mid-fill, the card back, radar's theme back without a reboot, the carousel over radar).
- **The carousel had fallen back to the text list:** with radar on the glass PSRAM held
  295 KB and the carousel's 434 KB snapshot would not come. The carousel now opens without
  the snapshot when it must (its zoom starts from the app's preview), allocates its name
  planes for the places it shows rather than twelve, and gives back the names and the
  centre's full plane when it closes. Radar on the glass: 661 KB free (was 295-370 KB);
  with the carousel up, ~100 KB.

### Why a theme is missing (2026-11-09, the owner's ask)

- `orb_theme_absent(slug)`: a few words for the dial and a sentence for the hub, or nothing.
  **No card** (chosen, not in the pool, the card not mounted: read live); **file deleted**
  (the reconcile found the chosen file gone: kept in NVS, `theme_gone`, because the removal
  reboots); **copy failed** (this boot's last fill failure, with its reason); **send it
  again** (ruling 8's refusal). Each clears when the app is stored a theme. An app evicted
  and waiting to come back is not "missing": the hub says "on the card only".
- The app's page on the dial shows its Themes row as "Themes (no card)" and so on, even when
  the card holds none of its themes (the press is then inert); `GET /api/v1/sd` carries
  `absent` ({app, brief, why}), and the hub's Themes page adds the sentence to its line.
- **Bronze:** radar's file deleted behind the orb → "file deleted" with the sentence, kept
  across the reboot; the file written back and stored → cleared.
- **Internal RAM, again:** the notes first sat in internal RAM (~780 B), and an OTA's park
  to the menu then never painted; the stall guard rebooted the orb mid-upload, every time.
  In PSRAM the OTA staged. The guard also fired once on the switch back to the clock after a
  staged OTA (harmless: the update reboots anyway), not chased.

### C6 — the hub says where a theme is (2026-11-09)

- Each theme row of `GET /api/v1/sd` carries `chosen` (`orb_theme_chosen`: the row is its
  app's choice). With `stored` it reads three ways: chosen and stored, **in the pool**;
  chosen and not stored, **on the card only** (evicted; it comes back when the app opens);
  neither, a card file nobody wears.
- The hub decodes it (`ThemeEntry.Chosen`), names what an app wears from its choice first
  (an evicted choice is still what the app wears; older firmware falls back to the stored
  row), and the Themes page tags the chosen row "in the pool" or "on the card only", the
  latter muted, each with a sentence on hover.
- `GET /api/v1/theme_cache` is on the hub's allowlist and the fake answers it; the bench
  doors (`debug/fill`, `debug/time`) are listed as bench doors in the route contract.
- **Bronze:** flights, the clock and weather read stored and chosen; radar, evicted, chosen
  alone. The page itself was not looked at on a running hub.

### C5c — the card file followed, and four apps through a pool for three (2026-11-08, Bronze)

- **Ruling 7, at every card mount** (`orb_sd_mount_gen` counts them; `orb_theme_service` on
  the router's poll posts `reconcile_job` once per mount, after 20 s of uptime, with the
  worker free): each choice's file is read against the CRC its copies were made from. A file
  that changed is stored again (the store rebuilds both copies and reboots); a file gone from
  a card that answers takes both copies and the choice (the removal's own ladder, which
  reboots); one app a run, so the reboot brings the next. A card that did not answer, or a
  header that would not read, changes nothing. A fill that finds its file changed hands it
  to the same reconcile rather than refusing.
- **Bronze:** a choice made stale (`GET /api/v1/debug/fill?stale=<slug>`, debug) was stored
  again in 11 s and rebooted once, the next boot quiet; radar's file deleted behind the orb's
  back (`/api/v1/sd/rm`) took its display entry and its choice with one reboot, and the file
  written back and stored brought both back with the same CRC.
- **Four apps through a pool for three:** with Skeleton (3.4 MB) the clock's, the pool held
  three display entries. Weather, radar, flights and the clock opened in turn: each came back
  from the card in 39-55 s, each evicting the least recently worn, no reboot, every layer
  claimed. ~30 s of each was the eviction's room-making (§5); the face's ring now counts
  those moves as well as the copy (it read 3 % → 96 % evenly over a 51 s arrival; it sat at
  nought for the moves before). The fill's progress is in `GET /api/v1/theme_cache` (`fill`).

### C5b — the fill's face and its pause (2026-11-08, Bronze)

- **The face** (`orb_strip.c`, states FILL and GROW): the app's preview at 300 px, the §61
  ring 6 px outside it (arc 0x66DF over track 0x3186, clockwise from twelve), redrawn every
  2 % of the copy; when the fill is over (landed or failed) the face grows to the glass and
  fades into the app as the carousel's open does. It comes up two ways: the carousel's zoom
  out stops at the face for an app whose theme is on the card only (or still filling), and an
  arrival from anywhere else (a hub request, boot, a return) puts it up from
  `wasm_activate`/`wasm_resume` (`orb_strip_fill_face`). A hold on the face opens the
  carousel over it, zooming down from the face; a turn or press is spent.
- **The preview of an evicted app** comes from its choice's card file
  (`orb_theme_pool_stem` falls back to the choice): before, the carousel read previews only
  for apps in the pool, so the face of an evicted app was a bare ring.
- **The glass and the fill share the card** (`orb_theme_fill_glass`). The card lock lends
  the display's DMA ring for as long as it is held, and a fill holds it from open to close, so
  the face drew 12 frames in a 9 s fill and the carousel opened over a running fill drew none.
  While the strip has the glass and owes a frame (a moving carousel, or a frame the held
  card refused), the fill lets the card go at its next 4 KB chunk until the frame is out
  (250 ms at most), and the ring is taken back at each return rather than after the
  network's window (`DISPLAY_URGENT_FILL`). Letting go after every chunk instead cost the
  fill 14 s → 24 s; waiting on every chunk for the face cost 25-33 s.
- **The pause** (ruling 5): an app's suspend stops its fill at the next chunk and keeps a
  mark (`fill_mark_t`: the slot, the table's CRC, the section, how far it was written and
  erased). The job ends, so the worker is never held by a pause; the next fill for the same
  app goes on from the mark when the table and the slot are as it left them, the sections
  already written read back off the flash for their checksums. A store or another app's fill
  in between changes the table, and the fill starts over.
- **The pixels are erased 64 KB ahead of the writes** in a fill (a store still erases up
  front), so the ring moves through what was one 2.5 s erase.
- **Bronze, the radar's Scope (1,772 KB), evicted with `GET /api/v1/debug/fill?evict=<slug>`
  (debug: parks the dial, drops the display entry, reboots):**

  | | the fill | the face |
  |---|---|---|
  | from a hub request | 12.4 s | ring and preview, 28 frames — before the glass took the card back |
  | left after 4 s, back 4 s later | paused at 140 KB, went on from 192 KB | up again on the return |
  | carousel → face → carousel over the face → face | 12.6-13.2 s | 28-32 frames, ~130 ms each (the copy's flash writes stall the compose, ~60 ms) |

- **PSRAM is the margin under radar:** the carousel kept its 434 KB snapshot of the glass for
  as long as it was up, and a fill landing under the face found no room for the radar's layer
  table (15 of 15 rows refused). The snapshot is now freed once the carousel opens an app.
  With radar on the glass Bronze has ~370 KB of PSRAM free (1.5 MB under the clock).
- **The bench door's first cut panicked** (*Cache disabled but cached memory accessed*): it
  unmapped under the drawing clock, and the panic was charged to the clock as its second
  fault, so a later switch to the clock was refused. It parks the dial first now.

### C5a — the fill's engine (2026-11-08, Bronze)

- **A fill is the store's own body** under a flag (`s_install_fill`): the same card checks,
  rebase, verdict, eviction (least recently worn first), copy, header and table, without the
  park, the voice stop, the staged witness or the reboot. It refuses a card file whose CRC is
  no longer the choice's (a changed file is a store's: owed, C5c). Binds wait while it moves
  entries (`s_pool_busy`), and it is heavy I/O to a guest's budget.
- **It starts** when an app with a recorded choice and no display entry comes to the glass
  (`wasm_activate`/`wasm_resume` post it; `orb_theme_has` counts a choice, so such an app
  keeps its stage). **It lands** on the LVGL task: the app, still on the glass, rebinds its
  stage (`stage_rebind`) and is resumed, so it reads its theme anew.
- **Bronze:** weather, evicted, came back in ~6 s (102.9 s the fill posted, 109.0 s its 51
  rows on the glass), no reboot.
- **Three crashes found and fixed on the way:**
  1. *Cache disabled but cached memory accessed*: a bind of an app with no theme in the pool
     left the last app's adoption in place, so the stage built the clock's theme on weather's
     page and the scheduler went on applying the clock's gear frames from a bank the fill then
     unmapped. Such a bind now lets the adoption go and unmaps, on the LVGL task; the fill
     unmaps nothing.
  2. *LoadProhibited at 0x34* (`stage_run` → `orb_bg_gif_mode`): a stage bound to a bare face
     started the dial's tick over a NULL geometry. `stage_run(true)` now parks for a bare face.
  3. *A boot loop* (the owner's Neon Moon refused by ruling 8): the same NULL geometry read on
     every flush (`orb_sched_l4_active`). No art is now a zeroed theme, never NULL
     (`use_no_art`, and boot before any adoption). Bronze was flashed over USB to recover.
- **Not explained:** the owner's store of Neon Moon (from the hub, mid-bench) left it without
  a voice bank and evicted flights though it fitted; the same file stored again from the card
  made its bank (3 sounds, 668 KB). Watch for it.

### C4 — the choices, the order, and stores that make room (2026-11-08, Bronze)

- **One NVS blob** (`theme_cache`, 880 B, beside `theme_staged`): each app's choice (its card
  stem and the file's geometry CRC) and the order the apps were last on the glass, least
  recent first. A store records the choice; a clear lets it go with both of the app's copies
  (its voice bank too: a clear had left the bank behind). A bind moves its app to the end of
  the order in RAM; the order is saved with a choice or an eviction, or at most every five
  minutes, so an app switch writes nothing. A theme stored before the choices were kept has
  its choice seeded at boot from its own entry (its name gives the stem).
- **A store makes room:** the fit is `orb_pool_evictions_kind` over the theme and its sounds,
  so only a theme bigger than the pool beside every voice bank is refused; the store
  reserves with `orb_pool_reserve_lru` and the voice bank with the same order, and logs each
  app whose theme left ("least recently on the glass; its file stays on the card").
- **The shelf's budgets** (`bake_check.py`, radar and weather): a theme fits the pool alone,
  beside a full voice bank: 8 MB less 1 MB, the tables and 512 KB of slack. The other
  defaults no longer count against it.
- **`GET /api/v1/theme_cache`:** the choices, the order and the pool's entries, for the hub
  (C6) and the bench.
- **Bronze:** the pool held the clock's voice bank and the radar's, flights' and the clock's
  display entries (7.4 MB); flights, then the radar, then the clock on the glass; weather's
  Scope (1.31 MB) stored: "flights's theme left the pool", and the table read over USB is the
  clock's voice bank, the radar's and the clock's display entries and weather's, 7.24 MB.
  The next boot seeded the radar's and the clock's choices; the order survived the reboot.
  Flights, evicted the boot before seeding, has no choice: its file is on the card, and a
  store records one.

### C3 — sound off the glass (2026-11-08, Bronze)

- **The voice bank** (`main/art/orb_voice.c`): a 32 B head, a 24 B record a sound, the PCM or
  ADPCM after, everything addressed from the bank's start; `orb_voice_parse` is the one gate
  between a bank's bytes and the mixer. `tools/host_voice_test.py`: a bank round-trips; 14
  malformed ones refused by name; the builder refuses what the parser would (bytes that are
  not a sound's samples', none, more than 16, over 1 MB); 20,000 random corruptions, none
  parsed into a sound past its bytes; 10 of 10 mutants.
- **The store:** its verdict hands back the projection's sound table (`clock3_verdict_ex`);
  the fit counts the bank beside the display entry, and a theme whose sounds pass 1 MB is
  refused by name. After the display entry is committed, `store_voice` reserves the bank
  (display entries lifted, this app's never evicted), copies each sound out of the entry
  just written, flash to flash, and commits it; a theme with no sounds drops the old bank.
  The store stops every voice before its unmap: the voice banks are what it moves.
- **Boot** maps [0, the voice banks' end) once and keeps it, checks each bank against its
  entry's CRCs through the mapping, and lists it by app (`orb_theme_voice`). A theme with
  sounds and no bank from the same file does not bind (ruling 8), at boot or at a bind.
- **Playback:** every sound door answers from the calling app's voice bank, gated on the
  bank, not on the stage: `stage_sound`, `stage_snd_ready/play/play_v`, and the svc
  `sound_play` by slot (which had read through the display mapping). Stop, active and
  reanchor act on the app's own voices. The display remap no longer stops voices (D39's
  stop retired), nor does a stage release or an app's suspend (D39's "an app behind the
  dial makes no sound", overruled by ruling 6): both stop the app's leased voices alone.
- **Leases by owner:** `orb_sound_lease_renew(owner)`; a leased voice lapses when its
  owner is not the app the stage is ticking. Only a loop is leased.
- **The clock** (v1.2.30) runs its sound policy from `orb_service` on wall time when it is
  off the glass, chimes and strikes alone (the tick loop and the hands' jumps stay with the
  visible dial).
- **The engine logs each voice** (its start, and how it ended: played out, lapsed, quiet
  hours), the evidence below.
- **Bronze:** Skeleton stored with its bank (the table read over USB: the clock's voice bank
  868 KB at 0, the display entries lifted after it). Bronze's clock runs 23 s behind the
  host's. At its 20:30 the half chime was started by the clock's service with weather on the
  glass and played out (8,452 ms). At 20:45 the Q3 chime was cut 2.5 s in by the switch to
  weather (the owner heard it): the suspend's stop-everything, fixed above. At 21:00, the
  hour chime and its nine strikes, queued with the clock on the glass and switched to
  weather 3 s in: every one played (the owner: "every gong and the chime played while
  swapping onto the weather app").
- **The sound limit** (ruling 9, `orb_sound.c`): a sound queued more than 1.5 s ahead waits in
  a schedule (64 entries, 32 an app) and takes a voice 1.5 s before its instant (the lead the
  jumps used, so the amp is up); with nothing sounding the mixer sleeps until the schedule's
  next lead instead of running the amp for a strike 20 s off. A voice starting to sound with
  four of its app's already sounding replaces that app's oldest one-shot; a full table
  replaces the oldest one-shot of the app holding the most. Stop, active, reanchor, leases and
  quiet hours reach a scheduled sound as they reach a voice. The bench door's `burst` (six
  dings at one instant, forty a second apart): two "replaced (its app's fifth sound)", four
  and then twenty-six played, the rest refused, "32 of 46 taken". The hour on the glass, the
  orb's clock warped to 21:58:40 (`GET /api/v1/debug/time?warp=<s>`, debug builds, moves every
  wall-clock reading): at 22:00 the hour chime (14.7 s) and ten strikes, scheduled and promoted
  1.5 s ahead, beside the running tick: all eleven played out, none replaced, none lapsed.
- **Found on the way:** every refusal from the play doors was a positive `esp_err_t`
  (ESP_ERR_NO_MEM is 0x101), against orb_sound.h's, orb_svc.h's and the SDK's "a handle > 0,
  or a negative error": a caller reading > 0 as a handle took a refusal for a sound. The
  engine, the app door and the svc door now answer the negated code.
- **Still owed:** the alerts design's arbitration between apps (§55); the flights, weather
  and radar themes ship no sounds, so they have none to play yet.

### C2 — the table's format 2 (2026-11-08)

- **The table:** an entry is 132 B, format 1's 128 and its kind (`ORB_POOL_DISPLAY`,
  `ORB_POOL_VOICE`); sixteen entries (32 B head + 16 × 132 B, one sector). A format 1 table
  still decodes, its entries display entries, and the next table write is format 2: the
  carry is that write, nothing more. Decode refuses a voice bank after a display entry, two
  entries of one kind for an app, and a kind it does not know.
- **The code:** `orb_pool_find` keeps its meaning (an app's display entry), so every
  firmware caller is unchanged; `orb_pool_find_kind`, `orb_pool_remove_kind`,
  `orb_pool_voice_end`, `orb_pool_evictions_kind`, and `orb_pool_reserve_voice`, which
  evicts display entries if it must, drops the app's old voice bank, compacts, then lifts
  every display entry by the new bank's size, the last first. `move_entry` copies
  descending for a move up, ascending for a move down. `orb_pool_commit` places either kind.
  The firmware's two entry builders (a store, the one-slot carry) mark a display entry; boot's
  fallback and the themes listing pass over voice banks.
- **The promise under cuts:** no voice bank but the one being stored is ever lost. A display
  entry may be: a voice bank's store lifts the display entries over themselves, and a cut
  in a lift plus a second in the repair's compaction can cost two display entries, which the
  cache refills from the card. Every other operation keeps the pool's old bound of one.
- **Host suite:** the carry (a format 1 table read, written back as format 2), voice banks
  first and lifted display entries intact, a voice bank never evicted (a display fill that
  would need one out is refused, flash untouched), display work that never moves a voice
  bank, a voice bank's store that evicts the least recent display entry, a voice bank
  replaced; a cut at every one of a voice bank store's 1,574 steps, each again in its repair,
  with the clock's voice bank already in the pool: no voice bank lost, at most two display
  entries. Four new mutants (never evicted, voice banks first, the descending lift, the
  format 1 carry) beside the copy's direction rewritten for the two-way loop.
- **Bronze:** the build read its format 1 table (both themes bound, the radar's 17 rows);
  re-storing flights' Blueprint wrote format 2, read back over USB with esptool: both copies
  format 2, the live one (serial 100) the radar's Scope at 0 and flights' Blueprint after it,
  display entries.
- **A downgrade** to a firmware before format 2 reads no table ("a pool table of another
  format") and boots every app bare until a store; the themes are on the card.

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
