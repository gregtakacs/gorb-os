# D42 (proposed): one row for everything that moves

**Status: RULED (owner, 2026-11-01, §11). E1 built and seen on orb-B 2026-11-01
(§8.2). E2 (the composer) is next.**

**Superseded for the theme editor (owner, 2026-11-01): see D43,
`docs/composer-stack-plan.md`.** For the composer and the design it authors, D43
replaces this file's E2 and E5 and adopts §7's warnings as its R8. This file still
governs the wire, the firmware and the reader.

It builds on D41 (`docs/history/hand-motion-plan.md`): D41's drive (source,
cadence, ease) is kept, and this file replaces the parts of D41 that still treat a
hand, a pointer and a ring as different things. **AUX pointer depth (D41 §4, "What G2
left") is held for this ruling** and lands as part of slice E1, not on the current
split.

**No legacy (owner, 2026-11-01).** Every theme is rebuilt as v6. The reader, the
schema, the baker and the hub refuse every older stamp. Nothing in this file keeps an
old theme drawing, and every D41 sentence that promises "a row that names nothing
keeps today's behaviour" is void under v6 (§8.1).

**The owner's brief (2026-11-01):**

- Treat every hand as a complication. No main hands versus sub-dial hands.
- Every movement is a lateral or rotational move from a start pose to an end pose
  over a period of time. Sliding, rotating and retrograde hands are then the same
  thing with different numbers. A full rotation is a retrograde of 360° with no
  lateral move.
- A dial hand is a hand whose shape is a ring instead of an arm. It usually turns
  under a blocking dial with a window.
- A calendar can be a double rotator (units and tens on separate rings) or a
  single 1–31 ring.
- Mechanical constraints, such as a hand sweeping through another hand's pivot
  shaft, may or may not be enforced.
- Digital displays: show any value in a chosen font and background.
- Ruled order: design this before the hand layering work, so depth is built once.
- No legacy: old themes are rejected, not converted (rulings in §11).

## 1. What is split today

Read at `f58f217`. Re-verify the anchors before editing.

| Concern | Main hands | Pointers | Rings and windows |
|---|---|---|---|
| Identity | HAND class, slots 0–5 (`CLOCK_SLOTS` in `tools/theme_schema.py`) | AUX class, `ptr0`–`ptr7` (`CLK_AUX_MAX` 8) | BANK class, `comp0`–`comp15` (`CLK_COMP_MAX` 16) |
| Firmware record | `clk_theme_t.hand[5]`, `shadow[5]`, `hand_drive[5]` (`main/art/clk_types.h`) | `clk_aux_t` | `clk_comp_t` |
| Drive | `HAND_ROW` pre-filled `clk_drive_t`, `drive_hand_deg` | `drive_aux` | whole values only |
| Paths | rotate, retrograde (a slide is refused on a HAND row; the composer bakes a sliding centre hand as a pointer) | rotate, retrograde, indexed, slide-x, slide-y | the plate a value picks |
| Depth | `order[5]` among the five | one block, wherever `aux_insert_at` puts it (`clock_view.c:1583`) | `order` as a bucket under a live layer (`orb_clock3.c:887`) |
| Shadow | its own sprite row (`sh_hour` …) | the next AUX row, flagged `EF_AUX_SHADOW` | none |
| Fold into the plate | `matte_mask` bit per slot | never | — |
| Animation | slot 5 only: the carriage's frame bank (`tourbillon[]`), turned by the seconds angle | none | — |
| Tempo | `second_sweep`, `tick_rate`, `second_railway` scalars, held to the seconds row's cadence (D41 §4) | — | — |

The composer has the same split. Centre hands live in `design.hands['centre-*']` with
a `motion` block. A sub-dial's hand is implied by the sub-dial's `scale` and carrier
(`SUB_CARRIERS` in `watch-engine.js`), and is offered as a pointer only on the
seconds and hours scales.

**Dead weight found while reading.** `plate_follow` and `text_over_hands` are
projected by `orb_clock3.c` and drawn by nothing in `clock_view.c`. v6 drops both
(§8.1): a turning plate is a ring row, and text over the hands is a depth.

**Two paint paths must agree on depth.** The classic path's z order is LVGL widget
creation order (`clock_view_init`). The L4 path's z order is the layer table's job
order (`clock_layers_refresh`). L4 owns the glass only when that table claims every
row (`orb_sched_layer_claimed`), so any row kind this file adds must be claimable by
the table, or the dial falls back to the slow path.

## 2. The model

A dial is a list of **rows**. A row has five parts, each independent of the others.

| Part | Says | Replaces |
|---|---|---|
| **Shape** | what the art is: an arm, a ring or disc, a text cell, or a value plate | the HAND / AUX / BANK class split |
| **Motion** | how a value in 0…1 becomes a pose: a start pose, an end pose, and an end rule | D34's path names (§3) |
| **Drive** | which value, when it changes, and how it eases: source, cadence, ease | D41, unchanged (§4) |
| **Placement** | where it is attached and how deep: pivot, parent row, depth | `order[5]`, `aux_insert_at`, bank buckets |
| **Paint** | how it is drawn: shadow, fold into the plate, smoothing, frames | `shadow[5]`, `matte_mask`, `EF_SMOOTH`, `tourbillon[]` |

"Main hand" and "sub-dial hand" stop being kinds. The hour hand is an arm row
pivoting at the dial centre and driven by hour12. A sub-dial seconds hand is an arm
row pivoting at the sub-dial's centre and driven by second. The firmware has one
array of rows and one loop over it.

## 3. Motion: two poses and an end rule

**A pose** is an angle θ plus an offset (x, y) of the pivot. The art is moved by the
offset first, then turned about the moved pivot.

**A motion** is a start pose P0, an end pose P1, and an end rule. The drive gives a
fraction *t* in 0…1 across the source's domain, and the row draws at P0 + *t*·(P1 − P0).

| End rule | Past the end of the domain | Use |
|---|---|---|
| `wrap` | the next unit starts again at P0 with no visible jump. Legal only when P1 looks the same as P0: Δθ a multiple of 360° and no offset | an ordinary hand, a disc, a ring |
| `flyback` | returns to P0 at the domain's reset, animated by the row's `ease` | a retrograde fan, a sliding bar |
| `hold` | rests at P1 until the source resets | a power-reserve or battery bar |

**Every D41 path is a set of numbers in this model:**

| D41 path | P0 → P1 | End rule |
|---|---|---|
| `rotate` | θ0 → θ0 + 360°, offset 0 | wrap |
| `retrograde` | θ0 → θ0 + span, offset 0 | flyback (hold for a measured source) |
| `slide-x`, `slide-y` | offset (x0, y0) → (x0 + dx, y0 + dy), θ fixed | flyback |
| composer `disc` | a rotate with a ring shape and Δθ = −360° (the scale turns under a fixed index) | wrap |
| `indexed` | rotate with cadence `jump` | wrap |

**New combinations come free:** a slide on a diagonal, a pointer that slides and tilts
at once, a fan whose pivot drifts.

**A wrap with an offset, or with Δθ not a multiple of 360°, is refused (owner,
2026-11-01).** It cannot draw without jumping on screen. It is refused at design time
(the composer does not offer it, and the schema and baker refuse it) **and again by
the firmware reader**, so a theme made by any other means is refused at the door (D26)
instead of drawn wrong.

**Units.** Firmware angles stay in tenths of a degree and offsets in pixels, as
D34's `MOTION_UNIT` says. The retrograde clamp encoding the composer uses today (D41
§5.1, zero written below the span) goes away, because the reader clamps at *t* = 1
instead of at an absolute angle.

## 4. Drive: D41's, plus digit sources

Source, cadence (`sweep`, `step` *N*, `jump`, `stop-to-go`, `drag`) and ease keep
D41's meanings, refusals and sound events.

**`inherit` goes.** It meant "what this row did before D41", and with no old themes
there is nothing to inherit. A moving v6 row names its source and its cadence; a row
with no source is a still layer and names no cadence (§8). The composer always writes
both, so this costs the author nothing.

**The seconds hand's speed comes from its row (owner, 2026-11-01).** The scheduler
reads the dial's tempo from the rows: the fastest stepping or sweeping seconds row
sets the beat (`step` *N* is *N* beats a second, `sweep` and `stop-to-go` ask for the
sweep tempo). The composer still offers the same choices it does now: 1, 2, 4 and 8
ticks a second, and Sweep. It writes them as the row's cadence and nowhere else.

**One addition: a digit selector.** A row may read one decimal digit of its source
instead of the whole value: units, tens, or the whole value (default).

- A **double-rotator date** is two ring rows. The units ring reads date digit 0
  over 0–9. The tens ring reads date digit 1 over 0–3. Both jump at midnight.
  On the 31st → 1st the units digit stays 1 and the tens ring turns 3 → 0, which
  is what the mechanism does.
- A **1–31 ring** needs nothing new: a ring row over date, 31 positions, `jump` or
  `drag`. In a 30-day month it goes 30 → 1 like an annual calendar's ring.
- The same selector serves a digital readout's fields (§6), so it is one column
  used by two shapes, not a calendar special case.

## 5. Placement and paint

**Depth.** Every row has one depth, 0–63 (the element row's existing `order` column
and `ELEM_ORDER_N`). Lower paints first. Ties keep row order. The plate is always
below every row, the glass always above. Both paint paths build their stack from
this one number, so the classic path and L4 cannot disagree.

**Parent.** A row may ride another row's pose (C4's `parent` column, already on the
wire). A sub-dial hand whose sub-dial slides, or the escapement riding the
carriage, is a parent link, not a special case.

**A window over a ring** is three rows by depth: the ring, then a blocking layer with
an aperture cut in it (art in a live row, not the restore plate; D41 §4 explains why
the plate cannot carry it), then whatever sits on top. The composer offers that
stack as one "disc behind a window" helper, but the format sees only rows.

**Paint attributes, per row:**

| Attribute | Generalizes |
|---|---|
| `shadow`: the row this row is the cast shadow of, turned by its owner's pose | `sh_*` sprite rows and `EF_AUX_SHADOW` |
| `fold`: a still row baked into the plate | `matte_mask` (only rows that never move may fold) |
| `smooth`: bilinear sampling | `EF_SMOOTH`, unchanged |
| `frames` and `delay`: an animated row | the carriage's frame bank (slot 5 is then an arm row driven by second, with frames) |

## 6. Digital readouts: a text shape

**What exists.** The dial already draws FORMAT 2 strip complications from per-glyph
sprites (`comp_strip_positions`, up to `CLK_STRIP_GLYPHS_MAX` 8 cells). FORMAT 3,
the format the composer writes, refuses a strip (`orb_clock3.c:1027`) because the
element row has no glyph fields. The composer's date windows with a custom format
bake one whole plate per value. That works for 31 dates or 7 days, but a time
readout would need 1,440 plates.

**Proposed.** A text row is:

- a **glyph set**, baked once in the chosen font, size, colour and finish: 0–9, the
  separators the format uses, and space. Weekday and month names become whole-word
  glyphs (7 and 12 plates);
- an optional **background plate**, a row of its own beneath it, so the readout can
  sit on a lit panel, a window, or nothing;
- a **format**: fixed-width fields over sources and digit selectors (`HH:MM`,
  `ddd DD`, `SS`) plus literal text. The composer's `fmtDate` tokens are the
  vocabulary, limited to fixed-width fields so cells never shift.

A text row has a motion like any other row, so a readout can slide or sit on a
turning ring. A change repaints only the cells whose glyph changed.

**Encoding cost.** A format string does not fit the fixed-width element row. It needs
a small string table in the clock section that the row points into. That is a new
section, and the reader gates it the same way it gates every other one.

## 7. Mechanical constraints: composer warnings, never refusals

The firmware does not check physics. The composer has a **Mechanical** switch, off by
default. When on, it warns, and a warning never blocks a bake:

| Warning | When |
|---|---|
| **Through an arbor** | a row's swept area (its whole fan, slide or turn) covers the pivot of a row stacked above it. That row's arbor rises from the dial to its own depth, so the lower hand would hit it |
| **Flyback through an arbor** | the same check, on the return path |
| **Coaxial order** | rows sharing a pivot are stacked in an order a tube-in-tube arbor cannot build: a slower row above a faster one on the same pivot (shown as information) |
| **Ring with nothing above it** | a ring row that no blocking row with an aperture covers. A bare disc dial (Orbit) is valid, so this is only a hint toward the window helper |

The swept-area test reuses the composer's existing swept-box code (the same boxes the
cost estimate uses), so it costs no new geometry.

## 8. Encoding: v6

**The wire (settled while building E1, 2026-11-01).**

- **One class for every row that can move: AUX (class 8), slots 0–23.** A BANK row stays
  the value-plate (window) row. The clock section refuses HAND (class 2) and SPRITE
  (class 1) rows outright.
- **A v6 clock stamp (6), 33-word element rows.** Five columns are appended to the 28:

  | Column | Type | Meaning on an AUX row |
  |---|---|---|
  | `dx`, `dy` | i16 | the travel of the pivot from P0 to P1, in px (±466) |
  | `end` | u8 | 0 wrap, 1 flyback, 2 hold |
  | `digit` | u8 | 0 the whole value, 1 its units digit, 2 its tens digit |
  | `str` | u16 | a text row's offset into the string table; 0 until E4 |

- **Existing columns, read anew on an AUX row:**
  - `kind` is the **shape**: 0 arm, 1 ring, 2 text (refused until E4).
  - `zero` is θ0 and `span` is Δθ, both in tenths of a degree, each within ±3600.
  - The row's own `x`, `y` and pivot place the art **at P0**, so P0 carries no offset of
    its own. The bake draws a hand where its pose at the start of the domain puts it.
  - `vmin` and `vmax` are the domain, read as *t* = (*u* − vmin) / (vmax − vmin). A
    wrapping row's domain is half-open, so a 1–31 date ring is vmin 1, vmax 32.
  - `order` is the depth (0–63) on one scale shared with the BANK rows. The plate is
    below everything and the glass above everything.
  - `parent` names another AUX row whose pose this one adds to its own. A parent that
    turns must share the child's centre.
- **Still rows are legal.** An AUX row with source `none` never moves: a bridge, a
  dial with a window, a counterweight. Its cadence, travel, domain, end and digit are
  all 0. A moving row names a source **and** a cadence. `inherit` is gone: cadence 0
  now means "none" and is legal only on a still row.
- **Shadows stay twins.** A shadow is the AUX row directly after its hand, flagged
  `EF_AUX_SHADOW`, and takes its hand's whole pose. No `shadow_of` column is needed.
- **The animated row.** At most one AUX row may carry frames (`n_frames` > 1, up to 16,
  with `delay` its frame time in ms). It is the carriage, and its `zero` is the start
  orientation the old `degrees` carried.
- **Scalars.** The section keeps its 26 words. Words 1–8, 14–20, 24 and 25
  (`plate_follow`, the shadow trio, `second_sweep`, `second_railway`,
  `text_over_hands`, `tick_rate`, the order table, `matte_mask`, the tourbillon pair)
  must be 0 under v6. The reader **derives** the tempo the scheduler reads (`second_sweep`,
  `tick_rate`) from the seconds rows, and the carriage pair from the animated row.
- **Folding.** A still layer the bake folds into the plate is simply not shipped as a
  row, so no mask is needed.
- **The container header** accepts GEOM_ABI 3 (app bundles, which carry no clock
  section) and 6. A bundle stamped 4 or 5 could only carry an old clock and is refused.

### 8.1 What v6 deletes

With no old themes to read, these go from every door (the firmware reader, the Python
schema and packer, the composer's baker, the hub's container reader in
`tools/orb-manager/internal/orbnet/themecontainer.go`):

- **Clock stamps 3, 4 and 5**, and the narrower strides each admitted
  (`ELEMENT_WORDS_CLOCK_V3/V4/V5`, `ELEMENT_WORDS_MIN`). A card holding an older theme
  shows it on the theme page as refused, with the reason, as a damaged entry already is.
- **Scalars:** `second_sweep`, `second_railway` and `tick_rate` (the tempo is read from
  the rows, §4), `plate_follow`, `text_over_hands`, `order[5]` and `order_n`.
- **HAND-slot machinery:** `hand[5]`, `shadow[5]`, `hand_drive[5]`, `HAND_ROW`,
  `aux_insert_at`, `matte_mask`'s per-slot bits, `tourbillon[]` as a seconds-slot
  special case, and the `CLOCK_SLOTS` hand and `sh_*` entries.
- **The `inherit` cadence** and its per-class arithmetic in `drive_units`, along with the
  v4-parity half of `tools/host_drive_test.py`, which proved a promise v6 no longer makes.
- **The `clk_comp_t` ring, strip and reserve fields** that only the retired FORMAT 2
  manifest filled.
- **The retrograde clamp encoding** in the composer (§3).

The bench themes (`skeleton-tick`, `skeleton-step` and the composer presets) are
rebuilt as v6. After the first v6 flash, orb-B draws the base face until a v6 theme
is installed.

**Firmware memory.** One `clk_row_t` array of 24 rows (§11.1) replaces `hand[5]`,
`shadow[5]`, `hand_drive[5]`, `clk_aux_t[8]` and the tourbillon bookkeeping, with one
widget or layer job per row. A theme with more rows is refused. The rotated poses are
still not cached in internal RAM (owner's earlier ruling).

### 8.2 What E1 built (2026-11-01)

**The wire.** `theme_schema.py` (the 33-word row, `SHAPES`/`ENDS`/`DIGITS`, cadence 0 as
`none`, `check_element`'s moving-row rules, `check_clock_rows`, `clock_tempo`,
`CLOCK_RETIRED_SCALARS`, 24 `ptr` slots, `ELEM_MAX` 64), `orbbundle.py` (`GEOM_ABI6`),
`orb_bundle.{c,h}` (`ORB_BUNDLE3_ABI6`, the five columns read), `orb_clock3.c`
(`row_check`, the row projection, the tempo and carriage derivations), the baker's
mirror, and the hub's `themecontainer.go` and fake device. The container header is 6
when a v6 section rides along and 3 otherwise; 4 and 5 are refused at every door.

**The dial.** `clk_row_t` (`clk_types.h`) replaces the hand slots, `clk_drive_t` and
`clk_aux_t`; the row table and the dial's per-row state live in PSRAM (internal `.bss`
fell by 2.1 kB). `clock_view.c` has one drive (`drive_units` with digits, `drive_t`,
`row_pose`, `row_ease` in fraction space), one update pass (`rows_update`: own poses,
parents, twins, marks, widgets) and one depth key (`depth_key`) that the classic
creation walk and the L4 layer table both sort by. The carriage is the animated row:
its frames swap on its own widget and its orbit still marks by ink chords. A twin takes
its row's sampling, and the carriage never takes the filter.

**Settled while building, beyond §8's list:**

- **A sweep names its own frame rate** in `cad_arg` (0 = none, at most 30). The beat
  the scheduler reads is the sweep's rate when it names one, else the fastest rate the
  dial's animation names — the plate's `bg_fps`, the carriage's — else the sweep tier,
  15. A stepping seconds row's beat is its N; a dial with no moving seconds row beats
  once a second.
- **Domains are half-open on a wrap.** *t* = (*u* − vmin) / (vmax − vmin), so a 1–31
  date ring is vmin 1, vmax 32 and the 31st sits one step short of P0.
- **The composer's inputs are unchanged until E2.** `bakeClock3` still takes hands by
  slot, shadows as sprites and pointers with D34's motion names, and translates each
  into rows (`v6Drive`): a rotation over a whole turn wraps, a partial one holds, a
  retrograde and a slide fly back, and a slide's start offset moves the placement.
- **Old bundles in the repo** were rewritten by `tools/migrate_v6.py`, a one-way
  host-side converter with the same translation (the pixel bank is untouched): the
  hub's default Skeleton and the golden pair. Nothing on a device or hub path reads its
  input format.

**Gates.** Every host suite green, with its mutants: `host_clock3_test.py` (444 checks;
28 projection and 6 sound mutants caught), `host_drive_test.py` (58 vectors, 27
mutants), `host_comp_math_test.py`, `host_orb_bundle_c_test.py` (33 mutants),
`host_orbelems_test.py`, `host_sd_test.py`, `host_hand_aa_test.py` (now the row-AA
rule); the composer's headless suites (`headless-bundle3` 226 checks with the shipped C
projecting six browser bakes, `headless-estimate` with the migrated Skeleton priced at
94 ms a flush against the 95 ms measured, `headless-motion`'s new check that every
motion hand of Flux, Orbit and Station bakes to a row that poses where the preview
draws); `go test ./...`. Retired with the hand slots: `host_from_bundle_test.py` and
`mut_from_bundle.py` (the compiled pair's reverse path has no v6 meaning) and the
v4-parity half of `host_drive_test.py`.

**On orb-B (`v1.1.43+8c8cde3`).** After the flash every theme on the card listed as
refused with its stamp's sentence, and the dial fell to the base face. Four bundles
migrated to v6 were then installed through the ordinary upload door, and the
framebuffer (`tools/fbgrab.py`) read against the wall clock:

| Theme | Rows exercised | On the glass |
|---|---|---|
| Flux | a jumping hour fan and a sweeping minute fan (flyback), a sliding seconds bar (step 1, 400 ms flyback ease), three twins | 15:43 drawn as the hour on 3 at 240°, the minute at 43 (≈116°), the bar along its track; the minute boundary ran a 10-frame burst, then 1 fps; L4 claimed all 8 layer rows, none refused |
| Orbit | three discs, a whole turn backwards from a 3 o'clock index (wrap): sweep, jump, step | 15:44:09 read 3.7 h, 44 min, 9 s under the index |
| Station | a stop-to-go second released by a jumping minute with a 150 ms settle | 15:45:14 the minute exactly on 45 and the second at 13.4 s of dial for 13 s of wall time; 9.4 s later it had moved 9.4 s on |
| Skeleton (the hub's default) | a delta-bank plate, hour and minute rows over a sub-dial seconds row, twins | 15:46:51 drawn as 3.8 h, 46 min, 51 s on the sub-dial |

## 9. Cost

More rows means more to repaint, so the composer's estimate (`estimate3`, the
three-term model in `docs/history/unified-render-path-design-2026-10-30.md` §5) prices every
row, not just the five slots. Two properties keep it honest:

- A `jump` or `step` row costs only on the frame it moves. A double-rotator date
  costs nothing between midnights.
- The expensive case is a large **sweeping** ring. A disc 240 px across is about
  45 kpx of ink, roughly 9 ms a frame at the painter's measured nearest-neighbour ink
  rate. That is affordable at a few frames per second, and a jump disc pays it once a
  minute. **Estimated, not measured**; slice E6 measures it.

## 10. Slices

| Slice | Content | Gate |
|---|---|---|
| **E0** ✔ | The owner rules this file. Done 2026-11-01 | — |
| **E1** ✔ | v6 end to end, legacy deleted (§8.1): the row array and one depth walk in the firmware; the two-pose motion and end rule; depth on every row (the held pointer-depth work); digit selectors; the tempo read from the rows. Python schema, baker, C reader and Go container move together | host suites green, one mutant caught per new refusal; every older stamp refused at every door; the JS twin and the C reader agree on pose at fixed times for every preset — **DONE 2026-11-01** (§8.2); the bench on orb-B is owed |
| **E2** → D43 | Superseded by `docs/composer-stack-plan.md` (D43), slices P3–P5. Original scope: composer, one list of rows (centre and sub-dial hands alike), each with shape, motion, drive, placement and depth; tick speed and Sweep offered as today; every preset and bench theme rebuilt as v6 | carried into D43 P4: the preview-versus-reader pose check; each preset installs on orb-B |
| **E3** | Calendar presets: a double-rotator date and a 1–31 ring behind a window | renders on orb-B, a clean `jump_date` |
| **E4** | Text rows: glyph sets, background, format, string table | a `HH:MM` readout and a `ddd DD` readout on orb-B |
| **E5** → D43 | Superseded: the mechanical warnings are D43 R8, built in D43 P4 | D43 P4: headless tests per warning |
| **E6** | Bench (D41's G4 folded in): stop-to-go, a flyback, a disc under a window, a sweeping disc's frame cost, a readout's cost | figures recorded here |

## 11. Rulings (owner, 2026-11-01)

1. **Row cap: 24.** A theme that needs more is refused; that is acceptable.
2. **Tempo from the rows.** The seconds hand's speed comes from its own definition
   (§4). The composer still offers distinct tick speeds and Sweep.
3. **The Mechanical switch** is the composer setting that turns §7's
   physical-plausibility warnings on for a design. Off by default; on for presets that
   imitate a real movement. The bake goes through either way.
4. **Text fonts:** the composer's existing font list. More fonts are added later as
   needed.
5. **Wrap with an offset: refused**, at design time and by the firmware reader (§3).
6. **No legacy.** Every theme is rebuilt as v6, and every older theme is refused (§8.1).

## 12. The one sentence to remember

Everything that moves is a row with a shape, two poses and a drive, stacked by one
depth, and a theme is v6 or it is refused.
