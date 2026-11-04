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
   through the store. This replaces gorb-os-2's theme-editor-kit-plan §0.1 ruling 5
   ("flights' interim themes are hard-coded in firmware"); that file needs the same
   correction in its own clone. Flights' first themes are baked from
   `apps/flights/art/` and installed like any other.
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

Entries are kept **in pool order, contiguous from 0x1000**, each a whole number of
4 KB sectors (the erase unit). The pool's free space is always one run at the end, up to
the tables. Sector 0 is retired: it held the one slot's header, and the carry erases it,
so an older firmware finds no slot and draws its base face rather than misreading a pool.

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

**Migration from the one slot.** A board with no valid table and the old 128 B slot
header in sector 0 gets one carry at boot: the header becomes entry 0 (slug from its
section, off and base 0x1000: the old index is at 0x1000 and its banks were rebased
slot-relative, which is pool-relative with the slot at 0), written as table A; then
sector 0 is erased. No pixel moves. A cut before the table lands leaves the old slot to
carry on the next boot; a cut after it leaves a valid table.

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
| **P3** | store and delete through the pool; the one-slot carry | host suites; bench: two apps' themes stored, one removed, the other intact |
| **P4** | boot adoption of every entry, per-app geometry, the stage bound to the app on the glass | bench: the clock and flights each on their own theme, switching between them |
| **P5** | per-app scan, verify, delete, router, status | host suites; the hub's lists still parse |

The JS baker (`baker@1`, gorb-os-2's lane) keeps its 24-row cap until it needs more: a
cap there is a refusal, so it cannot emit a theme this firmware rejects.
