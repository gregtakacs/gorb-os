# D41: Driven elements — every hand, pointer and ring names its source, path, cadence and depth

**Why this file exists.** Before this ruling, a dial's motion was a set of special
cases:

- the second hand had a sweep or tick choice;
- the minute could jump only through the second hand's railway word;
- the hour always crept;
- pointers could take a path (rotate, retrograde, slide) but always interpolated
  and all sat at one fixed depth;
- rings only ever jumped.

The owner wants all of it general: a retrograde hour or second hand, an hour disc
turning behind a window, a jumping minute, a dragging date, the station's waiting
second hand, all from one vocabulary.

The cadence-only version of D41 was ruled on 2026-10-30. On 2026-10-31 the owner
widened it to the four axes below, after the branch was rebased onto the merged
`main` (`96ef302`). This file records the model, the encoding, today's code it
replaces, and the slices. Read it before touching `hand_angles`,
`aux_value_1000` or `aux_angle_10`, FORMAT 3's element columns, or the
composer's Hands and Comps tabs.

**Status: RULED (owner, 2026-10-30, generalized 2026-10-31). G1 and G2 built 2026-11-01 (G2 without AUX depth, ease or bank cadences — see §4); G3–G5 not built.**

| Parameter | Ruled value |
|---|---|
| The model | Every element that shows a value is a **driven row**: a source, a path, a cadence and a placement, independent of each other (§2) |
| Which rows | HAND (all six slots: hour, minute, second, dial, counterweight, carriage), AUX (pointers), and BANK rings. Windows, strips and flares keep their whole-value plates |
| Who owns it | The theme. Columns in the three-way mirror, read by the clock app. **No OS change** |
| Old themes | A row that names nothing keeps today's behaviour exactly, so no shipped theme moves differently |
| Encoding | Three element columns appended under a new clock stamp (v5). HAND rows gain the drive columns AUX rows already have. No section scalar moves (§4) |
| Sound | A step on a minute, hour or calendar row is an event with a slot (D39 amendment, `docs/sound-plan.md` §3.1) |

## 1. What the code does on `main` today

| Element | Behaviour | Where |
|---|---|---|
| Second hand | Sweeps (`second_sweep`), or steps at `tick_rate` per second | `main/clock/clock_view.c` `hand_angles`, the `secF` block |
| Minute hand | Creeps. It jumps only when `second_railway` **and** `second_sweep` are both set, and the composer always bakes `second_railway: 0` (`orbtheme-baker.js:1592`). Only the legacy `tools/bake_clock.py:2564` can set it | `hand_angles`, `minF` |
| Hour hand | Always creeps | `hand_angles`, `hrF` |
| Dial and counterweight slots (3, 4) | Never turn: `ang[3] = ang[4] = 0` | `hand_angles` |
| Carriage (slot 5) | A hand-shaped layer whose frames animate the escapement while the seconds angle turns it (D32 ruling C) | `orb_clock3.c:289`, `clk_row_role` |
| Pointer (AUX row) | Takes a path from D34's `MOTION_NAMES` (`rotate`, `retrograde`, `indexed`, `slide-x`, `slide-y`) and any source. In FORMAT 3 it **always interpolates**: no column names a jump, so the projection writes `a->flags = 0` and says the flag "rides the next ABI append" | `orb_clock3.c:1034` |
| Pointer depth | One block for every pointer, under the first live hand and above the last live static | `aux_insert_at` |
| Ring (BANK `ring`) | Plates on a circle, turned by whole values: it jumps | `theme_schema.py` `KINDS` |
| Hand rows' `kind` and `src` | Refused if non-zero: how a row moves is not a hand row's to say | `theme_schema.py:816` |

**Retrograde already works for pointers.** `aux_angle_10` clamps at the span,
and the source's own reset at the top of its domain brings the pointer home, so a
retrograde date or seconds pointer draws today. The composer just has no control
for it: `retrograde` appears only in the baker's tables.

**Delta-bank cadence.** On a delta-bank dial (`bg_mode` 1; the code still calls
it `orb_bg_gif_mode()`), `hand_angles` steps the second hand on the tick
counter (`hand_beat_ticks`), so beats stay evenly spaced on a pipe whose tick is
not a divisor of the beat. GIF itself is gone as an authoring option; the name
survives on the delta-bank path. §3's drive function keeps that quantisation
for every row it drives.

## 2. The model: four independent axes

| Axis | Values | Column |
|---|---|---|
| **Source**: what value drives it | `SOURCES`: hour12, hour24, minute, second, date, weekday, month, moon_age, battery, power_reserve, day_of_year, week, quarter and the rest | `src` (exists) |
| **Path**: how that value becomes a pose | `rotate`, `retrograde`, `slide-x`, `slide-y`, with `span`, `zero`, `vmin` and `vmax` saying how far, from where, over what domain. `indexed` stays on the wire as an old spelling of `rotate` plus `jump` | `kind`, `span`, `zero`, `vmin`, `vmax` (exist on AUX; new on HAND) |
| **Cadence**: when it moves | `sweep`, `step` *N*, `jump`, `stop-to-go`, `drag`, plus an ease time (§2.1) | `cadence`, `cad_arg`, `ease` (new, §4) |
| **Placement**: where it turns, and how deep | pivot and centre (exist), `parent`, which hangs a row on another row (C4, exists), and depth: `order` as a bucket under one of the dial's live layers, as a bank row already uses it | `order` (exists; newly honoured on AUX) |

The axes are independent, so any combination is a theme, as long as the
combination means something (§2.2). These are the owner's examples, and others:

| Element | Source | Path | Cadence |
|---|---|---|---|
| Station minute | minute | rotate | `jump` |
| Station second | second | rotate | `stop-to-go` |
| Retrograde hour, a 180° fan | hour12 (vmax 12) | retrograde (span 1800) | `sweep`, with a flyback ease |
| Retrograde seconds | second | retrograde | `step` 1 |
| Wandering or jump hour disc behind a window | hour12 | rotate, on the dial slot (3) or an AUX row, at a depth under the layer that carries the aperture | `jump` (jump hour) or `sweep` (wandering) |
| Sweeping date pointer | date | rotate | `sweep` |
| Mechanical date ring | date | BANK `ring` | `drag` |
| Four-minute tourbillon | minute (vmax 4) | rotate (span 3600) | `sweep`. Rotate wraps at 3600, and 60 is a multiple of 4, so it runs through the hour without a seam |
| Half-minute slave-clock minute | minute | rotate | `step` 2 |

### 2.1 Cadence, exactly

**One rule covers every source.** A row's value is its source plus a fraction of
the next finer field. Cadence decides how much of that fraction it shows.

| Cadence | Shows | `cad_arg` | `ease` |
|---|---|---|---|
| `inherit` (0) | Today's behaviour for that row (§2.3) | 0 | 0 |
| `sweep` | The whole fraction | 0 | 0 |
| `step` | The fraction quantised to 1/*N* of a unit | *N* steps per unit of the source | settle ms on each step |
| `jump` | No fraction: moves at the boundary | 0 | settle ms |
| `stop-to-go` | One turn in *P*, then rests until the unit boundary | *P* in tenths of a second (default 585, Swiss railway) | 0 |
| `drag` | Holds, then creeps across a window that ends at the boundary | window in minutes (default 120) | 0 |

- **`jump` is `step` 1**, named for readability.
- **The second hand's tick choices are `step` 1, 2, 4 and 8**, and the tick
  loop's beat follows that *N* (D39 §4.2).
- **`ease` on a `retrograde` path is the flyback.** The return to `zero` is
  animated over that many ms instead of landing in one frame. On a step or jump
  it is the settle: a small overshoot, about 1.5° over about 120 ms by default.
  Either costs repaints only while it runs.

**The next finer field, per source.**

| Source | Next finer field |
|---|---|
| second | the wall fraction |
| minute | seconds/60 |
| hour | minutes/60 |
| date and weekday | the hour of the day/24 |
| month | (date − 1) over the month's length |
| day_of_year | the hour/24 |

Battery, power reserve and the other measured sources have no finer field. On
those, `sweep` is the same as `jump`.

### 2.2 Refused at the door (D26), never guessed

- A cadence that cannot apply to the row:
  - `stop-to-go` on anything but a second source;
  - `drag` on anything but a calendar source;
  - `step` *N* with *N* = 0;
  - any cadence but `jump` or `drag` on a BANK window, strip or flare (their
    plates are whole values);
  - `sweep` on a ring is allowed, and turns the ring continuously.
- `stop-to-go` on a dial with no `jump` minute row. Its release is the minute's
  jump.
- A second-source row with `step` *N* when *N* differs from the section's
  `tick_rate`. The scheduler's tempo and the hand must agree (§4).
- `indexed` combined with any cadence but `inherit` or `jump`.
- An `ease` on a row whose cadence has no instant to ease.

### 2.3 `inherit`: today's behaviour, row by row

| Row | `inherit` means |
|---|---|
| HAND hour, minute and second (slots 0–2) with `src` 0 | Today's `hand_angles`: hour creeps; minute creeps, or jumps under `second_railway` + `second_sweep`; second from `second_sweep` / `tick_rate` |
| HAND dial and counterweight (3, 4) with `src` 0 | Still, as today |
| HAND carriage (5) with `src` 0 | Turned by the seconds angle, as today |
| AUX | Sweep (FORMAT 3 has always interpolated pointers) |
| BANK | Jump (whole plates) |

So a v4 theme projects to v5 rows with every new column at 0 and draws
identically. The host suite proves that by angle, not by eye (slice G1).

## 3. One drive function

**Built (G1, 2026-11-01).** `clock_view.c`: `clk_drive_t` (in `clk_types.h`) is
the row; `HAND_ROW[5]` is the five slots pre-filled; `drive_hand_deg` and
`drive_aux` are the two entries, sharing `drive_finer` (the next finer field),
`drive_units` (the cadence) and `drive_travel` (the path). `CLK_CAD_INHERIT` is
each class's pre-D41 arithmetic, verbatim, which is what makes the parity exact;
the other cadences are live code that no v4 row can reach until G2's columns.

`hand_angles` and `aux_value_1000`/`aux_angle_10` become one function in
`clock_view.c`: a row's source, its next finer field, its cadence, then its path,
giving an angle or a displacement. A HAND row with `src` 0 is first given the
row its slot has always meant (hour12, rotate, span 3600, vmax 12; and so on),
so the legacy hands are just three pre-filled rows rather than a second code
path. The delta-bank tick quantisation of §1 applies inside it to every swept
row.

## 4. The encoding

**A v5 clock stamp appends three element columns.**

| Column | Type | Meaning |
|---|---|---|
| `cadence` | u8 | §2.1's id: 0 inherit, 1 sweep, 2 step, 3 jump, 4 stop-to-go, 5 drag |
| `cad_arg` | u16 | *N*, *P* or the drag window, per cadence |
| `ease` | u16 | settle or flyback ms; 0 = none |

- **The stride grows from 25 to 28 words.** That is C4's precedent exactly:
  `ELEMENT_WORDS_CLOCK_V4 = (25,)` becomes a v5 entry of `(28,)`, the stamp gate
  in `orb_clock3.c` and `checkClockStamp` admit it, and a reader that predates v5
  refuses the theme at the stamp gate.
- **HAND rows may carry `kind`, `src`, `span`, `zero`, `vmin` and `vmax` under
  v5.** `theme_schema.py:816`'s refusal of a non-zero `kind` on a HAND row
  becomes "non-zero only under v5". All-zero still means the slot's own mapping.
- **AUX rows honour `order` under v5** as a depth bucket under one of the live
  layers, the rule bank rows already follow (`orb_clock3.c:887`). A v4 AUX row
  keeps `aux_insert_at`'s single block.
- **No section scalar moves.** `ORB_BUNDLE_SCALARS_N` stays 26. `second_sweep`,
  `second_railway` and `tick_rate` stay: the scheduler reads `tick_rate` as the
  dial's tempo (`orb_clock3.c:1126`), so the baker keeps writing it and §2.2
  refuses a second row that disagrees. Under v5, the rows are the authority for
  motion and the scalars are the tempo hint.

**This replaces the open item the cadence-only D41 carried.** It weighed
widening `second_railway` against appending scalars. Columns answer it for every
row class at once and leave the scalar gate alone.

**Built (G2, 2026-11-01).** `theme_schema.py` (`ELEMENT_WORDS` 28, `CADENCES`,
`v5_claim`, `pack_elements_v5`, `check_clock_drive`), `orbbundle.py` (`GEOM_ABI5`),
`orb_bundle.{c,h}` (`ORB_BUNDLE3_ABI3`), `orb_clock3.c` (`cadence_check`, the hand
drive projection into `clk_theme_t.hand_drive[5]`, the pointer's `cadence`/`cad_arg`),
`orbtheme-baker.js` and the hub's container reader. A bake packs v5 exactly when a
row claims a motion column; every other bake's bytes do not move. Two rules were
settled while building:

- **The carriage names no motion of its own.** It turns with the seconds row, so
  the seconds row carries the cadence.
- **A seconds row's cadence must be the section's tempo.** Sweep and stop-to-go
  need `second_sweep`, step *N* needs `tick_rate` *N*, and jump needs `tick_rate` 1
  with no sweep. The scalars stay the scheduler's tempo hint.

**What G2 left, refused rather than silently ignored:**

- **`ease`.** The column rides the wire, but every reader refuses a non-zero
  ease until settle and flyback are drawn (G4).
- **Bank cadences other than inherit and jump.** FORMAT 3 projects only windows,
  whose plates are whole values. Sweep and drag wait for a ring the dial can turn.
- **AUX `order` as depth.** Pointers still join `aux_insert_at`'s one block. A
  disc behind a window needs it, and it lands with G4's bench.

**A disc behind a window is a depth question, and the bench answers it.** The
aperture has to be art in a live layer above the disc's bucket, not a hole in
the restore plate. The commit pass restores marked rows from the plate
(`orb_sched_restore_src`) and paints layers above it, so a disc under the plate
itself would be painted over. Slice G4 measures what a turning disc under a
cut-out layer costs per frame.

## 5. The composer

- **The Hands tab gains, per hand:**
  - **Source:** defaults to the slot's own.
  - **Path:** `Rotate` or `Retrograde`, with the arc's start and span.
  - **Cadence:** offers what §2.2 allows. The second hand's existing control
    (`Watch Composer.dc.html:3558`: 1, 2, 4, 8 per second, or Sweep) becomes its
    `step` *N* and `sweep`, plus `Stop-to-go` while the minute is `Jump`.
  - **Ease:** settle on a step or jump, flyback on a retrograde.
  - **The dial slot can turn**, which is the wandering or jump hour.
- **The Comps tab gives every pointer and ring the same three controls** and a
  depth choice.
- **The preview moves as the dial will.** `tickTurn` (`watch-engine.js`) and the
  pointer driver call a JS twin of §3's drive function. The mirror test samples
  both at fixed times and compares angles.
- **Presets.** The station preset becomes a jumping minute plus a stop-to-go
  second. Every other preset keeps `inherit`, so its renders do not move.

## 6. Why this pays off

1. **One vocabulary.** A retrograde hour, a jump hour, a sweeping date and a
   station minute are the same four columns with different values. The next
   complication needs no new special case.
2. **Sound is free on a step.** A step is a wall-time event, so the clock app
   plays the matching slot on the tick that re-poses the row
   (`docs/sound-plan.md` §3.1). That needs none of the loop anchoring a tick
   loop needs.
3. **It is cheaper to draw.** A hand re-pose repaints its whole rotated box,
   measured at 38 ms/frame when every hand re-posed each tick. The note is the HARD GATE comment at `clock_view.c:428`.
   A minute hand that jumps is re-posed 60 times less often than one that
   creeps.

## 7. Slices

| Slice | Content | Gate |
|---|---|---|
| **G1** ✔ | §3's drive function: `hand_angles` and the AUX path become one, with HAND slots as pre-filled rows. No format change | the host test samples a day of times and proves every v4 dial's angles identical before and after — **DONE 2026-11-01**: `tools/host_drive_test.py`, a day at 12 fps under all 40 v4 hand-word combinations (sweep × railway × tick_rate 0/1/2/4/8 × delta-bank beat), bit-identical to the pre-G1 `hand_angles`; eight v4 pointers identical to `aux_value_1000`; the new cadences pinned by hand-worked vectors; 14 mutants caught |
| **G2** ✔ (part) | v5: the three columns, drive columns on HAND rows, depth on AUX rows, §2.2's refusals. Mirror, reader and baker move together | host suites green, one mutant caught per new refusal — **DONE 2026-11-01** except AUX depth, ease and bank cadences (§4, "What G2 left"): `host_clock3_test.py`'s D41 battery (13 refusals at both ends, 6 mutants), `host_drive_test.py` (17 mutants), `headless-bundle3` (11 D41 checks), the hub's container reader takes v5 |
| **G3** | Composer: per-hand and per-pointer Source / Path / Cadence / Ease / depth, the JS drive twin, the station preset | the preview-versus-reader angle check; `_selftest` green |
| **G4** | Bench: stop-to-go plus a jumping minute, a retrograde hour's flyback, a disc under a cut-out layer, settle cost | rests and releases on the wall minute; frame cost recorded here |
| **G5** | Step sounds, landed with D39's S6 | the clunk lands on the jump's frame |

## 8. The one sentence to remember

Every hand, pointer and ring is a driven row (a source, a path, a cadence and a
depth) that defaults to exactly what it does today.
