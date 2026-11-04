# The theme pool: several apps' themes in the one `assets` area

Status: **ruled 2026-11-03 (owner, §0). Building on `feat/stage-app-themes`.** This is
the firmware work flight-tracker-design §11.2 found: the tracker is the second
theme-aware app, and the device holds one theme. It goes onto `main` when the OS work
is done, so gorb-os-2 (the theme editor kit, the per-app Themes page) can merge it.

## 0. Owner rulings, 2026-11-03

1. **One shared pool, packed, always defragmented.** The 8 MB `assets` area is shared by
   every theme-aware app (§43.1's ruling, now built). The OS decides where each app's
   theme goes and keeps a table of what is where; an app asks for its own theme and is
   told where it is. Adding and removing a theme leaves no holes: the pool is always
   packed from the start.
2. **No app embeds its own theme, ever.** A theme is always pool content, installed
   through the store. Clarified by the owner the same day: "the tracker carries its own
   theme" meant that the hub has no editor for it yet, so its themes are baked by hand
   into `.orbtheme` files (from `apps/flights/art/`) and stored like any other, never
   compiled into the app or the firmware. gorb-os-2's theme-editor-kit-plan §0.1 ruling 5
   ("flights' interim themes are hard-coded in firmware") read it the other way and
   needs the same correction in its own clone.
3. **32 moving rows** (`CLK_ROW_MAX`, was 24), for the tracker's plane pool.
4. **A row may carry frames the app picks** (flight-tracker-design §11.2 item 2), so one
   plane row shows any category and band.

## 1. What is there today (read 2026-11-03)

- **One slot at partition offset 0** (`orb_theme.c` `SLOT_HDR 0x0`, `SLOT_GEOM_OFF
  0x1000`, `SLOT_ART_OFF 0x10000`, `SLOT_CAP 0x800000`). A 128 B header (no slug), the
  bundle's index at 0x1000, banks from 0x10000. `ORB_THEME_SLOT_CAP` is 0x7F0000.
- **Boot** maps the art span once (`art_bank_map(SLOT_ART_OFF, span)`, no PSRAM copy),
  projects the one section with `orb_clock3_parse`, and publishes singletons: `s_geo`,
  `s_live_sec`, `s_active_*`, the `clk_theme` macros.
- **Store** erases the header, the index and the art span from 0x10000 on, streams the
  card's banks in, CRCs the span back, writes the header last, reboots. The previous
  theme is gone.
- **Clock-only gates**: the card verifier binds `"clock"`, the scan lists `clock-*` for
  `app="clock"`, the router remembers `"clock"` after a store.
- **The stage is one instance**, latched to the app whose slug matches
  `orb_theme_live_slug()`; it caches the art base in two places (`orb_stage.c` `s_art`,
  `orb_sched.c` `s_art`).

## 2. Design

### 2.1 The table

The **last two sectors** of `assets` (0x7FE000 and 0x7FF000) hold two copies of the
**pool table**, A and B, each 4 KB: a head (magic, format, serial, entry count, CRC) and
up to **8 entries**, one per theme-aware app (B5's app cap). The live table is the valid
copy with the higher serial; a write goes to the other copy, so a table write is atomic.
At the end, not the start, so the one slot's layout (index at 0x1000, banks at 0x10000)
is already a pool entry and the carry moves no bytes (§2.3). Each entry:

| field | meaning |
|---|---|
| slug | the app the theme belongs to (16 B, the section's own slug) |
| name | the theme's name (56 B, as the slot header carries it) |
| off, size | the entry's extent in the pool: its index region, then its banks |
| base | the offset the entry stood at when its index was rebased |
| geom_size, geom_crc | the index region and its CRC (post-rebase) |
| art_off, art_size, art_crc | the banks, entry-relative, and the read-back CRC |
| file_crc, serial | the card file's identity, the store counter |
| state | LIVE, or MOVING while a compaction rewrites it |

Entries are kept **in pool order, contiguous from offset 0**, each a whole number of
4 KB sectors (the erase unit). The pool's free space is always one run at the end, up to
the tables. **An entry is laid out as the one slot was**: its 128 B slot header in its
first sector, the bundle's index from +0x1000, its banks after the index (from
+0x1000 plus the index rounded to sectors; a carried slot keeps its 0x10000). The table
calls everything before the banks the entry's head, CRC'd as one; the store, the staged
witness and the identity words go on reading a slot header, now one per entry.

**Moving an entry rewrites nothing in it.** Its index records bank offsets as they were
at rebase, when the entry stood at `base`; where the entry stands now is `off`, so a bank
is at `record - base + off`. A compaction is a byte copy, and every CRC stays true.

### 2.2 Store, remove, compact

- **Store app X's theme**: if X has an entry, remove it first; then append the new one
  at the end of the packed run, if it fits. Fit is checked before anything is erased.
- **Remove an entry**: every entry after it moves down by its size, one at a time, in
  pool order. A move goes sector by sector in ascending address, so an overlapping move
  downward never erases a sector it has not copied yet.
- **Power loss**: before an entry moves, the table marks it MOVING; after its last
  sector is copied and read back, the table records its new offset as LIVE. A cut leaves
  at most one entry MOVING. Boot's repair reads that entry back where the table says it
  was: a move to a place clear of itself never touched those bytes, so the entry is LIVE
  again and the compaction moves it anew. Only a move by less than the entry's own size
  overwrites its first sectors; then the entry is dropped, that app falls to its base
  face, and its theme is still on the card to store again. The torn-store promise D30
  already gives the clock, kept per app, and narrower: most cuts lose nothing.
- **Reboot after a store**, as today: the map, the projections and the stage are built
  at boot from the table.

### 2.3 Boot

Read the live table; map the packed run once, from offset 0 (8 MB is 128 of the 234 MMU
pages); for each LIVE entry parse its index, check its section's slug is the entry's,
and project it into that app's own geometry (rows, comps, rects, sounds in PSRAM). An
entry that fails is dropped, its app on its base face. A bank's address is the map base
plus `record - base + off`.

**Migration from the one slot.** A board with no valid table and a slot header at
offset 0 gets one carry at boot: the slot becomes entry 0 (slug from its section, off and
base 0, its banks already rebased slot-relative, which is pool-relative with the slot at
0), written as table A. Nothing moves and nothing is erased. A cut before the table
lands leaves the slot to carry on the next boot. An older firmware still finds entry 0's
header at offset 0 and draws it if it is a clock theme; a downgrade with another app's
theme first is the one case it would misread.

### 2.4 The stage

One stage, **bound to the app on the glass**. When a theme-aware app is activated the
stage binds to that app's geometry and art base and builds its layers on that app's
screen; when it is suspended the stage parks and releases them. `orb_theme_geo()` and
the `clk_theme` macros read the bound app's geometry, so the paint path is unchanged.
`wasm_init` gives the stage to an app that has a LIVE entry, by its own slug.

### 2.5 Per-app everywhere else

The card verifier binds the theme's own app (the stem's `<slug>-` prefix and the
section's slug must agree, and the slug must be an installed app); the scan lists one
app's themes; delete and deactivate act on one entry; the router remembers the storing
app; status reports name the app.

### 2.6 Frames the app picks (ruling 4)

A moving row with several frames and **no frame time** (`delay` 0) is a row whose frames
the app picks. Today the reader refuses that combination ("animates %u frames and names
no frame time"), so no shipped theme carries it, and a carriage keeps its meaning (frames
and a frame time: the OS animates it). The frames are consecutive catalogue planes of one
size, sharing the row's geometry. The frame count lives in an OS-only table
(`clk_theme_t.pick_n`, beside `drum_r`), not in `clk_row_t`, whose size is the app ABI.
The app picks with a new import, `orb_stage_frame(i, f)`: it answers the row's frame
count, and shows frame `f` when `f` is in range (`f < 0` only asks).

## 3. Slices

| Slice | Content | Gate |
|---|---|---|
| **P1** | `CLK_ROW_MAX` 32; app-picked frames: schema, reader, stage, import, SDK | host suites green; mutants for the new refusals |
| **P2** | the pool table and its compaction as pure C (`main/art/orb_pool.c`), every flash access through an io | `tools/host_pool_test.py` over a simulated NOR flash: 13 malformed tables refused by name; a cut at every step of a replacement (504), a removal (264) and an overlapping move (1,526), each with a second cut in the repair, losing no other theme except the one an overlapping move tore; 8 of 8 mutants caught — **built 2026-11-03** |
| **P3** | store and delete through the pool; the one-slot carry | host suites; bench: two apps' themes stored, one removed, the other intact — **built and benched on Bronze 2026-11-04** (§4); two apps' themes at once waits on P5 |
| **P4** | boot adoption of every entry, per-app geometry, the stage bound to the app on the glass | bench: the clock and flights each on their own theme, switching between them — **built and benched on Bronze 2026-11-04** (§4) |
| **P5** | per-app scan, verify, delete, router, status | host suites; the hub's lists still parse — **built and benched on Bronze 2026-11-04** (§4) |

The JS baker (`baker@1`, gorb-os-2's lane) keeps its 24-row cap until it needs more: a
cap there is a refusal, so it cannot emit a theme this firmware rejects.

## 4. As built

### P3 — the store and boot through the pool (2026-11-04)

- **An entry is a slot at its offset.** Every one-slot function already took a slot base
  (the A/B pair's legacy), so the store, boot, the carry and the scan pass an entry's
  offset where they passed 0, and a theme's bank records stay entry-relative, rebased to
  `SLOT_ART_OFF` as before. So `blob_off`, `live_sec` and the lines
  `host_orblayers_test.py` pins are unchanged: a bound theme maps from its entry plus
  `SLOT_ART_OFF`. The table's `base` is 0 for every entry. The cost is the slot's 64 KB
  head per entry, of which an index uses a few KB: up to about 56 KB a theme.
- **Store**: the fit is asked before the dial parks (`orb_pool_fits`, the room in KB in
  the refusal); after the unmap the pool removes the app's old theme and compacts the
  rest, the bundle is copied to the packed run's end, its header written, and the table
  names it last. The serial counts up over the app's own last header.
- **Boot**: load the table, carry the one slot if there is none, repair, then draw one
  theme as before: the clock's, else the first entry. A pool that will not load is the
  base face. An empty pool with a store's witness left keeps the witness up.
- **Clear** (deleting the worn theme): the pool removes the worn app's entry and compacts.
- **Still the clock's alone**: the card verifier, the scan and the router still bind
  `"clock"` (P5), and the stage still draws one theme per boot (P4). With one app storing,
  the pool holds one entry, carried or stored, and every path above runs.
- **Mapping**: an entry is sector-aligned, not page-aligned; `esp_partition_mmap` rounds
  the map down to its page and offsets the pointer (`partition_target.c:170-175`), so an
  unaligned entry maps right. art_bank's page bill can then be one page short.
- **Gates**: every host suite; `host_sd_test.py`'s store lint now pins the pool's rules
  (the target is `orb_pool_reserve`'s answer alone, the fit before the park, the table
  after the header), with three mutants for them, caught. Firmware builds clean.
- **Benched on Bronze (orb-A), `v1.2.7+c47644e`, 2026-11-04.** Bronze came in on
  `v1.2.7+238c503` wearing Speedster (stored), Skeleton on the card.
  - **The carry.** First boot on the pool firmware kept Speedster on the glass (11
    layers, 1,409,024 B mapped, no faults) and the scan marked it stored, which now reads
    the clock's pool entry. That boot's log was not captured (the flash rebooted it before
    a capture opened); the next boot read `pool: 1 theme, 1474560 of 8380416 B used`
    (the 64 KB head and Speedster's banks exactly) with no carry line, so the table the
    first boot wrote was found and the carry did not repeat.
  - **A store through the pool** (`store_check.sh quiet`, Skeleton over Speedster): 18,380
    ms, no panic, no witness; the reboot read `pool: 1 theme, 2797568 B` and drew
    Skeleton (42 MMU pages).
  - **A cut mid-store** (Speedster's store, the board reset 8 s in by opening the capture
    port): the boot read `pool: 0 themes` and drew the base face, no panic, and the
    witness said `last store did not finish - press again to complete it`. The store had
    already let Skeleton go when it made room; the new theme was never named. A cut costs
    the storing app its theme, as the one slot did, and nothing else.
  - **The upload door** (`store_check.sh upload`, a renamed Skeleton "Bench Pool" sent as
    the hub sends): stored into an empty pool in 18,872 ms; the successful store retired
    the cut's witness.
  - **A delete of the worn theme** (Bench Pool): the card file deleted, `clock's entry
    left the pool`, the reboot read `pool: 0 themes`, base face, no panic.
  - **Restored**: Speedster stored again (9,946 ms), worn, no faults, no witness.
  - **Not reached on the bench**: a compaction that moves another app's theme, and a cut
    during one. With the clock the only app that can store (P5), the pool never holds
    two entries on a board; `host_pool_test.py` carries that proof until P5.

### P5 — any installed app's theme (2026-11-04)

- **Built.** The card doors take the app from the stem and refuse an app that is not
  installed or a file that themes another app than its name says; one OS-wide themes
  source walks the shared directory; `orb_theme_scan` lists every app's themes and
  `orb_theme_scan_app` one app's (an app's own service asks for its own); a row's stored
  mark reads its app's pool entry; a delete lets its app's entry go; the store's reboot
  remembers the storing app. The stage still draws one theme per boot, the clock's (P4).
- **Benched on Bronze, `v1.2.7+856cd15`.** The flights theme was Skeleton's art re-filed
  under `flights` with `orbbundle.write_bundle` ("Pool Bench"); the first try dropped the
  record's panel fields and the orb refused it by name ("a clock section made for a
  different panel"), which is the door doing its job.
  - **Two apps' themes at once**: the upload stored it in 17,047 ms; the reboot read
    `pool: 2 themes, 4272128 B` (Speedster's 1,474,560 and the bench's 2,797,568), the clock
    still drew Speedster, and the listing marked both stored.
  - **A compaction that moves another app's theme**, overlapping: Skeleton stored for
    the clock let Speedster's entry at 0 go and slid the flights theme down by less than
    its own size, every byte read back against its CRCs; 42,778 ms in all; the reboot
    read `pool: 2 themes, 5595136 B`, packed, Skeleton drawn, the flights theme still
    stored.
  - **A cut during a compaction**: re-storing the flights theme let its entry at 0 go
    and moved the clock's Skeleton down; the board was reset 10 s in. The boot read
    `pool: 1 theme` 24.4 s in instead of 1.2 s — the repair redid the 2.8 MB move before
    the display came up — and drew Skeleton; the flights store's witness stood. Which
    side of the move the cut landed on the log could not say, so boot now names a theme
    found mid-move and a pool found unpacked.
  - **Cleanup**: the bench file deleted (not stored at that moment, so no reboot), and
    Speedster restored (9,835 ms). Bronze ends as it began, on this build.
- **The two notes, taken care of (2026-11-04, benched on Bronze, `v1.2.7+281fa3e`).**
  The boot was not dark: the overlay stood on "mapping" while the repair ran, which says
  nothing. The pool's io now takes an optional progress hook, called per sector moved;
  boot shows **"repairing themes"** with the bar, a store's compaction **"making room"**,
  and a clear's compaction "making room" too. Boot also logs a theme found mid-move and a
  pool found unpacked. On Bronze: a cut mid-move logged `clock's theme was mid-move when
  the power went; reading it back`, showed "repairing themes" for the 23.5 s repair, and
  kept Skeleton. The first cut of the bar repainted per sector and cost a 2.8 MB move about
  ten seconds (a store 42.8 s became 52.1 s; "making room" 37.2 s); one paint per percent
  brought the move to 28.8 s. A delete of the flights theme while the clock was on the
  glass let flights' entry go and moved Speedster down to 0, the clock's face untouched;
  its log said the base face would draw, which was the clock-era sentence, and now names
  which app's theme left.

### P4 — each app wears its own theme (2026-11-04)

- **Built.** One stage, bound to whichever theme-aware app is building or on the glass
  (`stage_bind`). A bind releases what the stage built for the last app (its layer-table
  rows through the broker, its widgets, the plate and the 434 KB delta canvas, its voices),
  binds the new app's pool entry (`orb_theme_bind`: projected into the dial's tables, its
  banks mapped in place of the old) and builds on the new app's page, beneath the app's own
  widgets. The scheduler keeps one frame timer across rebuilds. The wasm host gives the
  stage to any app with a pool entry, binds it on init, activate and resume, and gates every
  stage import on the binding, so an app ticking in the background answers as the base face.
  Boot still adopts the clock's entry first; each themed app's init binds its own, and the
  app the router resumes binds last.
- **Benched on Bronze, `v1.2.7+564bd27`.** With the clock alone themed, boot is unchanged
  (`stage bound to clock`, 11 layers). With the bench flights theme stored: boot bound the
  clock, then flights' init released it and bound "Pool Bench" (8 layers), and the router
  resumed flights. Switching by `/api/v1/screen?to=` moved the theme with the glass every
  time (`stage released flights`, `bound clock's theme "Speedster"`): **20 round trips**,
  the layers always clock 11 / flights 0 or the reverse, no faults, no reboot; PSRAM flat
  from cycle 5 to 20 on both sides (6,498,672 / 6,056,556 B), LVGL's pool flat, internal RAM
  4 KB lower after the first cycles and flat after. `/api/v1/fb` grabs show Speedster drawn
  right before and after a trip through flights. `wasm_slips` went 3 → 4 over the run: one
  guest call overran its budget somewhere; not traced.
- **Found on the glass: the direct compositor paints over the app's widgets.** On flights'
  page a rectangle of the bench theme's art lay over the flights app's own canvas. L4
  writes the theme's pixels into the panel mirror for the boxes it redraws, whatever
  LVGL's stacking says, so an app's opaque canvas does not hide its theme there. The bench
  theme is a clock face flights never poses, and flights still paints its v1 canvas, so
  this is the item-5 rule, now measured: an app that wears a theme drops the canvas, and
  its own widgets over the theme (the card, the header) need checking against L4's boxes.
- **Cleanup**: the bench theme deleted (its entry left the pool); Bronze ends on Speedster.
