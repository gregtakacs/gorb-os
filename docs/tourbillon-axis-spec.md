# Tourbillon carriage axis spec

Who this is for: the agent or tool that emits an orbital watch kit zip for
`tools/prep_watch_kit.py`, and anyone reviewing such a zip before it is baked.

## The invariant

The carriage's red staff jewel is the tourbillon's physical rotation axis. Its
alpha-weighted centroid must land **exactly** on the integer `centre` the
manifest declares for the `tourbillon` entry.

```
centroid(tourbillon/frame_NN.png red staff jewel) == centre   for every frame
```

Two consequences the art must satisfy, not the firmware:

1. `centre` is an **integer**. `prep_watch_kit.py` copies it into
   `clock_style.json` as `pivotX`/`pivotY`/`centerX`/`centerY`, and
   `bake_clock.py:547` reads them with `int()`. A fractional `centre` truncates
   and silently loses the correction. Never "fix" an off-axis jewel by writing
   `232.5`.
2. The jewel's ink bbox is **odd x odd**. A 466-px canvas has its centre at
   texel index `(466-1)/2 = 232.5` — a corner. An even-width blob centred on a
   corner has a fractional centroid, so **no** integer `centre` can be correct.
   Only the art can move.

## Why an error is so visible

`clk_sprite_t` stores `pivotx`/`pivoty` as `int16_t`. The firmware pins that
pivot on the layer centre and inverse-maps every screen texel through it. So a
pivot a distance `d` from the real axis makes the jewel orbit a circle of radius
`d`: `2d` px peak-to-peak, every tick, with the frame's dirty rect jittering
along with it. The observed kit2 error was `(-0.5, -0.5)`, radius 0.707 px:
1.41 px of orbit geometry demands plus ~0.28 px of texel quantisation = **1.70 px
p2p bob**, visible as the hub jewel twitching vertically at 8 fps.

## Measured, on the shipped bytes

| metric | kit2 (rejected) | kit3 (accepted) |
|---|---|---|
| staff bbox | 8 x 8 (even) | 7 x 7 (odd) |
| alpha centroid | (232.500, 232.500) | (233.000, 233.000) |
| centroid − declared centre | (−0.5, −0.5) | (0.000, 0.000) |
| baked pivot-to-axis distance | 0.707 px | **0.0000 px** |
| hub travel, nearest path | 1.70 px, rows [232,233,234] | **0.27 px, row [233]** |

The kit2 row is from an earlier harness whose alpha cutoff was `> 12`; the
committed `tools/sim_tourb_bob.py` cutoff is `> 13`, which admits one more edge
texel and reports 0.27 px instead of 0.00. Both are far inside a texel, and the
decisive facts are identical: the pivot-to-axis distance is exactly 0.0000 px and
the hub visits ONE screen row (233) across all 1800 sampled orbit angles.

**Read that last line with care.** It says the jewel is centred. It does not say
the carriage is still — see "Two different bobs" below, which the device itself
settled and which this metric cannot see.

The fix was purely art-side: re-render the carriage shifted `(+0.5, +0.5)` texel
from the 2x source. Zero runtime instructions changed.

## The bob that remained was never in the clock at all

Kit3's centred pivot removed the geometric orbit, and every carriage instrument
then came back clean — correctly. The visible bob survived because it was never a
carriage artifact. It lived in the **display flush path**: `dma_dirty_bands()`
coalesces dirty rows into up to `DB_MAX_BANDS` bands and hands each to
`esp_lcd_panel_draw_bitmap(0, r0, 466, r1+1, ...)`, whose `RASET` the CO5300
driver programs verbatim from `(r0, r1)`. A band that began on an **odd** row
addressed its window one row off, painting *correct pixels onto wrong rows*. Since
the carriage's box moves by whole pixels every tick, the set of odd `r0` values
changed every tick: entire rows of the dial — bezel, background, everything —
shifted vertically by one pixel in a way that tracked the carriage's dirty rect and
therefore looked like a carriage artifact.

`main/display/display.c`'s `DB_EVEN_BANDS` snaps each band to an even start and an
even row count. The x axis had this guard already (bands are full-width, so "no odd
pad column is ever synthesized -> the stale-hairline race is structurally
impossible"); the row axis simply never got the same discipline.

How it was pinned down, since each step was needed and none was sufficient:

| hypothesis | test | verdict |
|---|---|---|
| composite not reproducible | compose every band twice on identical inputs, diff | clean — every band reproduced exactly |
| mirror stale outside the marked box | re-composite 24 never-marked rows, compare to mirror | clean — byte-exact |
| flush gate suppresses a row that needs sending | force `send = true` for every marked row | **no change** — gate exonerated |
| background art animates wrong | freeze the GIF bank on frame 0 | **no change** — art exonerated |
| panel row addressing | flush the whole page every commit (always `r0 = 0`, even) | **bob GONE** |
| odd `RASET` start | keep bands + GIF, snap band edges to even rows | **bob GONE** — the fix |

The last two are the proof: full-page flush cured it but cost the coalescing win
(1,520,092 px/s vs 587,160), and even-band snapping cured it while keeping bands.

### Why the hub metric could not have found this

`sim_tourb_bob.py` samples the hub jewel's alpha centroid, and the jewel sits on
the rotation's fixed point — so it is structurally blind to any artifact that moves
the whole image rather than the carriage. It reported "0.27 px, one row" truthfully
and uselessly: that number demonstrated the art was centred and said nothing about
whether the rendered frame was right. It stays in the tree as an **art** gate
(`kit_check.py`'s baked-bytes twin), not as proof the display is correct. Any future
"it still jitters" report should start from the flush path, not the painter: compare
what the mirror holds against what `RASET` was told.

## A note on the 2-tap filter, for when it is revisited

`CLK_TOURB_FILTER` was restored, measured, and reverted again during this
investigation; it is **not** in the tree and it is **not** the bob's cure. Its own
commit message claimed the chord marks cover the filtered painted set — that held for
kit2's crop and not for kit3's: re-enabling it makes `tools/sim_tourb_mark.py`
**exit 1**, because the 2-tap paints ~209 extra px/commit and at ang 310.8° and
314.6° a chord mark misses one, which would ghost. Measured cost on kit3, one rebuild
per side: nearest 8 fps / carriage 564 ns per blend / flush 119 ms; filter 7 fps /
794 ns / flush 135 ms. So it is a real cost with no demonstrated benefit — the
host whole-body residual against the exact forward-map centroid is 0.27 px, which
leaves no ~1 px snap for it to remove. `8cc76f6`'s "2.23 → 0.25 px" was measured on
kit2, whose jewel was 0.707 px off-axis: that residual was the geometric orbit.

**Internal-RAM texel staging is also measured negative.** Copying each tile's texel
bounding box into `.bss` — the prerequisite `8cc76f6` named — made the carriage
dearer, not cheaper: 5 fps at 1613 ns per carriage blend against the filter's 794
unstaged. The box over-reads ~3.3x (a 32x8 device tile at 45° spans ~840 texels while
~256 blends consume them), and the existing column tiling already extracts the PSRAM
cache win. Do not retry it in that form.

## Verification

Two gates, run in this order. Both must exit 0.

```
python3 tools/kit_check.py assets/sources/watch-orbital-kit3.zip   # the ART
python3 tools/sim_tourb_bob.py                                      # the BAKED pair
```

`kit_check.py` decodes every `tourbillon/frame_NN.png`, isolates the red staff
jewel (`alpha >= 60`, `R > 100`, `R - max(G,B) > 40`), and asserts: centroid ==
declared `centre` within 0.25 px, odd bbox in both axes, identical bbox across
the bank with centroid spread ≤ 0.01 px, integral `centre`, `frames` matching the
PNG count, and the presence of the `pivot`/`size`/`hubJewel` proof fields. It runs
on a bare system python — Pillow is preferred, `tools/pngdec.py` is the fallback.

`sim_tourb_bob.py` re-measures on `assets/clock_assets.{bin,h}` — the bytes the
device gets — using the same integer maths as `tools/sim_tourb_mark.py`, so a
pivot that the baker corrupted is caught even when the art passed. It imports
`rot_sin_cos`/`cdiv`/`parse_sprites` from `sim_tourb_mark.py` and the jewel gate
from `kit_check.py`; nothing is duplicated, so the three tools cannot drift apart
and disagree. Its controls perturb the stored pivot by one texel and must show
several px of travel — they do (3.04 px at +1,+1), which proves the measurement
is geometric rather than sampling noise.

`assets/sources/watch-orbital-kit2.zip` is kept as the regression fixture:
`kit_check.py` must exit 1 on it, reporting the `(-0.5, -0.5)` defect. Broken
bytes that prove the gate fires are worth keeping.

## Negative results

Recorded so the dead ends are not re-walked:

- **Fractional `centre`** in the manifest: rejected. Both `prep_watch_kit.py` and
  `bake_clock.py:547` read these fields with `int()`, so the fraction truncates
  away. The art must move.
- **Injecting the residual into the row origin** (`x0 += ((1024-cma)*dx + sma*dy)/4`)
  instead of moving the art: reaches 0.19 px residual, not 0. Costs two adds per
  frame and hides an art bug in firmware. Rejected in favour of the rebake.
- **Chasing the carriage for the residual bob.** Every carriage instrument was
  clean and the bob persisted; the four display-path tests in the table above are
  what located it. Start there next time, not at the resampler.
- **L5 static-layer row memoization** (skip re-compositing a marked row whose
  inputs are unchanged since its accepted composite, so the static dial/glass
  stops re-blending behind the carriage): built, verified, and killed before it
  shipped — **the target set is empty.** The key was per-row: last accept's
  per-role input generations (bumped at every `s_ang_10` write, the tourbillon
  frame swap, and view rebuild), marked x-span equality, a per-row mirror write
  epoch bumped by every LVGL chunk, and exclusion of GIF-forced rows. A VERIFY
  build (composes eligible rows anyway, counts what a skip would have frozen)
  measured rows/s ~2,300, **skippable/s 0, mismatch 0** over 25 s of live sweep.
  Kit3's mark discipline already owns this win: GIF pushes force their rows
  (the canvas genuinely changed under them), carriage chord marks accompany a
  gen that moves every tick at 8 fps, and rows nothing moves are never marked
  at all — that was the `mark_all_layers` fix. The glass 167 ms/s + dial
  100 ms/s are paid blending over rows whose background genuinely changed.
  Shrinking that cost means shrinking the GIF delta rects (art/baker side),
  not skipping composites.
- **Internal-RAM texel staging** as a precondition for the filter: measured
  negative, and covered with numbers in "A note on the 2-tap filter" above.
- **Rounding vs truncating** the 8.8 row origin: no meaningful change in the
  rendered position. Consistent with there being no resampler artifact to remove.
- **Trusting the manifest** because the renderer wrote it: falsified for the
  tourbillon entry. `centre: [233,233]` was wrong by half a texel on both axes
  and every downstream tool propagated it.

## Do not change

- `size: 466`, `frames: 8`, `fps: 8` — `prep_watch_kit.py` sets `tickRate = fps`,
  so moving `fps` changes the carriage cadence and masks what you are measuring.
- `zeroRotationDeg: 0` — the rest angle where the hub is on-axis.
- Any other layer's geometry, the dial opening (`comps c1`, `size 200`), or the
  subdial (`c3`) / flare (`c2`) placement.
- The carriage's reach: its ink must stay inside the dial opening and pass under
  the tachymetre ring, or `bake_clock.py`'s `_zero_under`/fold gate refuses.

## Ink budget

Carriage blended px per commit drives the frame budget directly:

| kit | blended px/commit | bbox | r_max | median r |
|---|---|---|---|---|
| kit2 | 54,799 | 223 x 298 | 190.4 | 93.4 |
| kit3 | **62,350 (+13.8%)** | 245 x 306 | 192.8 | 99.6 |

At 687 ns per nearest-path blend the carriage goes ~37.6 → ~42.8 ms/commit. A
thinner carriage is the cheap lever if the commit passes its budget; the expensive
one is staging the carriage's texel rows in internal RAM.

## Required kit output

The `tourbillon` manifest entry carries its own measured geometry, the same
proof the hands already give:

```json
{ "layer": "tourbillon", "files": "tourbillon/frame_01…08.png",
  "centre":  [233, 233],
  "pivot":   [233, 233],
  "size":    [245, 306],
  "hubJewel": { "bbox": [230, 230, 236, 236],
                "centroid": [233.0000, 233.0000] },
  "zeroRotationDeg": 0 }
```

`centroid` is alpha-weighted, measured over the whole 466x466 frame. The hands
may carry fractional pivots because `bake_clock.py` runs `snap_pivot_to_hub` on
them (line 405). `bake_tourbillon` (line 534) never calls the snap, so the
carriage axis must already be correct in the PNG.

Keep the whole bank registered: all frames place the staff at the identical bbox
and centroid, spread ≤ 0.01 px. The baker takes ONE cycle-wide crop
(lines 558-571), so drift desyncs the dirty rects. The device applies the
carriage's rotation from `seconds/60 * 360 + zeroRotationDeg`; never bake it in.
