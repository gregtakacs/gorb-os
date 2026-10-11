# FPS budget benchmark — findings (2026-10-10)

Run of `docs/fps-budget-benchmark-plan.md`. Device: gorb-os at HEAD (art
`9d528ad`, code `a679463`), kit3, `/dev/ttyACM0`, probe on for every point.
Harness now in `tools/bench/`: `cap.py` (capture), `cadence.py` (parse with
`--csv`), `rectaudit.py` (pre-flash prediction), `mkvariant.py` (art variants),
`fit.py` (model + contrasts). Raw captures, the machine-readable table and the
driver log live under `/tmp/bench/` (`*.log`, `results.csv`, `points.log`).

## Headline

**Art lever A (fewer GIF rows) is worthless. Art lever B (a thinner carriage)
is the whole budget, and it clears 8 fps by every gate the plan set.** The
rows the panel pays for are not the GIF's 208-row band: the tourbillon
carriage's rotating diff box force-marks ~255 full-width rows every tick on
its own, so clamping the GIF bank from 208 rows to 32 changes nothing
measurable. Cutting the carriage's ink beyond r=150 drops 28k blends/tick and
14.9 ms off the mean flush — 138 ms to 123 ms — and is the only variant that
passes all four gates.

| gate | control (P2a_px0) | V-B (carriage r≤150) | V-C (V-A + V-B) |
|---|---|---|---|
| mean flush ≤ 125 ms | 138.1 **fail** | 123.3 **pass** | 122.6 **pass** |
| median fps ≥ 7 | 6.89 fail | 7.41 **pass** | 7.53 **pass** |
| ≥30 % of seconds read 8 fps | 5/27 = 19 % fail | 9/27 = 33 % **pass** | 12/27 = 44 % **pass** |
| gap p90 < 185 ms | 168 pass | 151 **pass** | 151 **pass** |

## Model

Joint least squares over all 19 points (`python3 tools/bench/fit.py
/tmp/bench/results.csv`):

```
flush_ms = -33.2 + 215.1 us/row + 648 ns/real-layer-blend + 95 ns/synthetic-blend
R^2 = 0.95
```

**The negative intercept is the finding, not the coefficients.** Every art
point sits at rows 250–270 (the carriage floor), and the only points far above
that — the row-spread synthetic points — lift the blend count with them. `b`
and `c` are collinear on this data. The numbers to use come from the
single-variable contrasts the sweeps were built to produce:

| contrast (one driver moves, the other held) | Δflush | per unit |
|---|---|---|
| synthetic alpha ink 0 → 400k px/tick, rows pinned | +74.1 ms | **185 ns/px** |
| synthetic alpha ink 0 → 128k px/tick, rows pinned | +29.8 ms | **232 ns/px** |
| same ink, rows 259 → 392 (spread leg) | +30.6 ms | **229 µs/row** |
| same ink, rows 255 → 417 (spread leg, top) | +35.3 ms | **218 µs/row** |
| opaque store instead of alpha blend, same 400k px | +30.2 ms | **76 ns/px** |
| V-B: carriage ink beyond r=150 removed (−28k blends/tick) | −14.9 ms | **530 ns/px** |
| V-C: carriage + GIF band (−27k blends/tick) | −15.5 ms | **574 ns/px** |

The usable model is therefore:

```
flush_ms ≈ 55 fixed (tick + restore + compare + submit)
         + 0.22 ms/row  x full-width rows the DMA carries
         + (per-layer px price) x blended px
```

`b = 220 µs/row` confirms the prior's ~300 µs/row to within the access-pattern
difference. `c` is **not one constant**: 76 ns for a store, 185–232 ns for a
linear alpha blend, 334–467 ns for the static dial/glass passes, 530–800 ns for
the rotated carriage/hand passes that gather scattered texels out of PSRAM.
Averaging them into one `c` is what makes a px curve look flat.

Per-layer blend price off the fps line's split (control point):

| layer | blends/tick | ms/frame | ns/blend |
|---|---|---|---|
| hour hand | 6,984 | 9.41 | 1,348 |
| minute hand | 6,845 | 8.30 | 1,213 |
| carriage (sec) | 62,531 | 40.80 | 652 |
| dial | 30,164 | 13.47 | 447 |
| glass | 71,991 | 24.06 | 334 |

The hand sweeps cost 1.2–1.35 µs per blend because they rotate about the dial
centre and sample across the whole sprite. The carriage is the same maths at
~650 ns because 85 % of its box is empty alpha. The static dial/glass passes
are 3× cheaper per px.

## Per-point table

Steady-state window (≥ 5 s after boot). `rows` = CAD `px` / 466 = full-width
rows the DMA carried. `blends/tick` = the real layers' blends from the fps
line. `ink/tick` = the synthetic `DISPLAY_BENCH` layer's blends (0 for every
art point).

| point | blocks | fps | flush avg | gap p50 | gap p90 | hook | idle | te | dma | rows | blends/tick | ink/tick |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| P1-base (probe, shipped art) | – | 6.50 | 134.4 | 151.6 | 168.3 | 108.7 | 24.9 | 8.9 | 14.0 | 265 | 177,129 | – |
| P2a_px0 (control, bench on) | 0 | 6.89 | 138.1 | 151.3 | 168.3 | 104.8 | 24.1 | 8.3 | 14.3 | 269 | 178,516 | 0 |
| P2a_px18 | 18 | 6.04 | 154.7 | 168.3 | 185.2 | 124.3 | 27.1 | 8.1 | 13.7 | 259 | 193,140 | 64,398 |
| P2a_px36 | 36 | 5.67 | 167.9 | 185.0 | 201.5 | 137.4 | 26.4 | 7.4 | 13.5 | 259 | 199,726 | 128,536 |
| P2a_px36b (repeat) | 36 | 5.59 | 169.7 | 185.0 | 201.9 | 140.9 | 23.9 | 8.3 | 14.4 | 272 | 200,212 | 128,718 |
| P2a_px72 | 72 | 5.02 | 188.2 | 202.3 | 218.7 | 161.6 | 24.6 | 8.9 | 13.9 | 266 | 208,923 | 257,221 |
| P2a_px112 | 112 | 4.47 | 212.3 | 235.4 | 236.9 | 183.5 | 29.8 | 7.9 | 13.5 | 255 | 209,632 | 399,754 |
| P2o_px112 (opaque control) | 112 | 5.46 | 168.4 | 185.1 | 201.9 | 140.2 | 30.2 | 8.2 | 13.2 | 253 | 204,996 | 400,457 |
| P3_r32 (GIF clamped to 32 rows) | – | 7.01 | 137.0 | 151.2 | 168.0 | 106.7 | 19.8 | 8.2 | 14.3 | **268** | 176,375 | – |
| P3_r64 | – | 6.70 | 138.2 | 167.3 | 168.1 | 108.9 | 24.1 | 9.6 | 13.5 | 252 | 175,573 | – |
| P3_r128 | – | 6.72 | 137.7 | 167.3 | 168.2 | 106.6 | 25.1 | 9.9 | 13.4 | 255 | 175,216 | – |
| P3_r128b (repeat) | – | 6.91 | 133.6 | 151.2 | 168.0 | 105.3 | 22.7 | 8.5 | 14.4 | 269 | 175,964 | – |
| P3_r208 (shipped control) | – | 6.83 | 134.2 | 151.3 | 168.1 | 106.3 | 23.5 | 9.3 | 14.0 | 267 | 177,543 | – |
| P3_r336 | – | 6.49 | 134.3 | 167.9 | 168.2 | 108.2 | 29.1 | 10.3 | 13.4 | 253 | 177,214 | – |
| P4_va (blanked [140..330] = null) | – | 6.52 | 135.9 | 167.9 | 168.3 | 109.3 | 28.1 | 9.9 | 13.3 | 251 | 175,608 | – |
| P4_vb (carriage r≤150) | – | 7.41 | **123.3** | 151.0 | 151.3 | 94.5 | 23.5 | 9.7 | 13.2 | 250 | **150,523** | – |
| P4_vc (V-A + V-B) | – | 7.53 | **122.6** | 134.6 | 151.3 | 93.7 | 22.2 | 9.3 | 13.2 | 252 | 151,565 | – |
| P5_sp36 (ink spread page-wide) | 36 | 5.05 | 198.6 | 217.6 | 219.0 | 153.3 | 24.7 | 8.7 | **20.6** | **392** | 213,589 | 127,606 |
| P5_sp112 | 112 | 4.08 | 247.6 | 252.4 | 271.6 | 201.5 | 24.3 | 7.6 | **21.9** | **417** | 230,790 | 399,467 |

Repeat spread, which the plan requires inside ±5 ms: `P2a_px36` vs
`P2a_px36b` = 1.8 ms; `P3_r128` vs `P3_r128b` = 4.1 ms. The control's own
spread across `P1-base` / `P2a_px0` / `P3_r208` is 134.2–138.1 ms, so a
single-point difference under ~4 ms is noise, not effect. Read every claim
against that floor.

## What each phase showed

**Phase 0/1 — the control reproduces the cadence-probe session.** Re-baking
kit3 with the documented recipe produced a bank **byte-identical** to
`assets/clock_assets.bin` (only the generated `.h`'s provenance comment
differs), so the recipe is reproducible and it is the baseline.
`rectaudit.py --check-committed` reports `8/8 transitions identical,
committed push 34304 px/tick MATCH` — the gate that makes a pre-flash
prediction worth a flash. The probe build's baseline (flush 134.4 ms, 6.50 fps,
hook 108.7, te 8.9, dma 14.0, bands 3.4) sits inside the plan's gate of
131–135 ms and 6–7 fps.

**Phase 2 — pixels are not free, but the first ladder said they were, and that
was a probe bug.** The plan specified "opaque ... source-over" blocks, which
`blend_px()` implements as a byte-swapped store (`a >= 255` shortcut): no
`mix_16_16` at all. That ladder read 19 ns/px and would have triggered the
plan's abort condition ("flat curve → stop before Phase 3"). Adding
`DISPLAY_BENCH_ALPHA` and re-running the whole ladder at a=128 — the real
blend maths the dial's layers pay — gave **185–232 ns/px**, the order of
magnitude the plan predicted (310 ns/px from the glass). The opaque ladder is
kept as a control point (`P2o_px112`): it prices pure pixel traffic
(76 ns/px) separately from blending (185 ns/px), which is the distinction the
earlier session's "pixels exonerate themselves" conclusion was missing.

**Phase 3 — the row lever does not exist.** Clamping the GIF frames to 32 rows
pushed 4 rects / 3.8 % of a page (predicted: 48-row union after the packer's
16-px tile closure) and the DMA still carried **268** full-width rows, against
267 for the shipped control. Flush across the whole sweep, R = 32/64/128/208/336
plus a repeat, spans 133.6–138.2 ms with no trend. The reason is in the
committed header: the carriage's per-frame diff rect `(97,60,58,57)` sits at
radius ≈130 from the pivot, so as the carriage rotates 45° per tick that box
alone marks rows 106..360 — **255 rows**. The GIF's 208-row band is a subset of
what the carriage forces regardless. Only moving the *synthetic* ink outside
the band (`P5_*`, rows 392/417) made the DMA row count and the `dma` phase move,
14 → 21 ms. That pair is what prices `b`, and it is why the row coefficient
cannot be measured from the art side at all.

**Phase 4 — the one lever that works, and one specced variant that is a null.**
V-B (alpha-zero the carriage beyond r=150) passes `tools/kit_check.py`: staff
jewel centroid still exactly (233,233) on all eight frames, bbox still odd,
bank registered. It removed 9,003 of the carriage's 62,350 ink px (14.4 %) and
28k blends/tick, moving hook 104.8 → 94.5 ms and flush 138.1 → 123.3 ms.
**V-A as the plan specced it — blank the base frames outside [140..330] — is a
verified null**: `rectaudit.py` reports the resulting bank rect-for-rect
identical to shipped, because 140..330 rounds out to tile rows 128..335
anyway. The packer's 16-px tile quantisation absorbs the change. It was flashed
anyway (`P4_va`, 135.9 ms = control) to prove the prediction instead of trusting
it, and the tile-aligned band [144..319] (176 rows) is covered by the P3 sweep,
which also bought nothing. V-C = V-B's gain and no more: lever A adds nothing
on top.

## Limitations

- `rows` is CAD `px` / 466, so it counts full-width rows the DMA carried, not
  the marked-row set. A row marked but dropped by the compare gate is free and
  correctly invisible to the model.
- `blends/tick` is the fps line's per-second `blend/s` divided by the reported
  integer fps: a per-second mean, not a per-frame distribution. It excludes the
  synthetic layer, which is booked in its own column.
- One 30 s capture per config (~150 frames) except the two required repeats.
  The control's own spread is 3.9 ms, so differences under ~4 ms are not
  effects. The V-B win (14.9 ms) is well outside that.
- `b` and `c` are collinear in the joint fit, because the art cannot move rows
  independently of the carriage's blends; the negative intercept is the
  evidence. Use the contrast table, or points like `P5_*` that move one driver
  alone.
- The synthetic blocks read a solid PSRAM plane linearly while real layers
  gather scattered rotated texels. That access-pattern gap, plus alpha, is why
  the synthetic slope (185 ns/px) sits below the real carriage/hand slope
  (530–800 ns/blend). The sweep prices "pixel traffic + blend maths", not "what
  this particular layer's fetch costs".
- The 8-fps-second gate passes at 33 % on V-B — barely over the 30 % bar on 27
  seconds of samples. Directional, not proven.
- Every point ran with the probe on. The plan's own off-probe check said the
  median fps matches, so the per-frame serial lines are honest load.
- The opaque point (`P2o_px112`) is excluded from the joint fit and reported as
  its own contrast, for the reason above.

## Art lever A/B decision

- **Art lever A: dead. Do not spend another bake on the GIF delta bank.** The
  carriage's rotating diff box forces ~255 rows no matter what the bank does.
  The trigger to revisit is a change that shrinks the carriage's diff rect
  itself, and `rectaudit.py` will show the row-union drop before any flash.
- **Art lever B: adopt it; it clears the plan's own 8 fps gates.** A carriage
  whose ink stops at r≈150 (kit3 reaches r=192.8) buys ~15 ms/frame: mean
  flush 138 → 123 ms, median 6.9 → 7.4 fps, 33 % of seconds at 8 fps, gap p90
  168 → 151 ms. The honest caveat: it removes 14 % of the carriage's ink — the
  tips of the outer arm — which is a visible design change, so it is the kit
  owner's call, not a silent re-bake. `tools/bench/mkvariant.py cart` builds the
  variant and `tools/kit_check.py` gates it.
- **The last ~15 ms is not art.** At V-B's 123 ms the remaining structure is
  ~55 ms fixed (tick work, band restore, compare, submit) plus the carriage's
  own 40.8 ms of rotated blends. Closing that is the internal-RAM texel staging
  experiment the axis spec already priced, not another art iteration.


## The frozen-rim finding (same day)

Reported symptom: at 4 fps the gear spokes crawl but the teeth look stationary.

`dial-renderer.js` turns every wheel exactly `K = 4` of its own teeth per loop,
and derives the spoke count as `s = N/K` (`chain()`), which is what makes the
loop seamless: one spoke pitch equals exactly four teeth. Frames equal fps, so
the loop plays once a second and the per-frame step is `K/F` teeth.

A feature repeats `F / gcd(K, F)` positions per bank. Teeth are identical, so
their period is one tooth:

| fps | step | teeth | spokes (period 4 teeth) |
|---|---|---|---|
| 4 | 1 tooth | **1** | 4 |
| 8 | 1/2 | 2 | 8 |
| 12 | 1/3 | 3 | 12 |
| 15 | 4/15 | 15 | 15 |

`gearSpec` shades with a world-fixed conic gradient, so landing exactly one
tooth later reproduces the same pixels: the rim is a fixed point of the frame
schedule while the spokes still resolve four positions. That is the report
exactly. It is geometry, not a row-cull or cache fault.

No frame schedule fixes it. Seam closure forces the total rotation to be a
multiple of the spoke pitch, so with `K = 4` every legal total is divisible by
4 and every 4-frame step is a whole tooth. Raising teeth-per-spoke to an odd
number does work, but only `T = 7` (or 11) is coprime with 4, 8, 12 and 15
together; `T = 5` fixes those three and collapses the 15 fps tier to 3
positions. `T = 7` also requires every wheel's tooth count to be a multiple of 7, which
re-proportions all 76 wheels across 11 trains and invalidates the measured caps
in `rows-capacity-2026-10-10.md`.

What shipped is `T = 7`, and the re-proportioning is the reason rather than the
obstacle: the teeth were too coarse to begin with, and finer teeth are only
available by buying more of them. Holding each pitch radius `rp = m * N / 2`
while stepping `N` to the nearest multiple of 7 lets the module drop roughly
half, so tooth height halves with it. Classic's front goes from `m = 4` / 9 px
teeth to `m = 2.3` / 5.2 px at `N = 32,24,20,16,24 -> 56,42,35,28,42`. Spoke and
pinion counts are unchanged, because `N / K` is preserved: 56/7 = 8 where 32/4
was 8. Every train is re-toothed the same way in `watch-engine.js` `TRAINS`,
with the fallback `FD`/`RD`/`FM`/`RM` in `dial-renderer.js` matching `classic`.

The method per side: the module is shared, because it sets tooth size for the
whole side and the meshing centre distance `dist = m(Na+Nb)/2` derives from it,
so it cannot be tuned per wheel. Pick the module for the tooth size wanted, then
snap each wheel to the multiple of 7 that best preserves its pitch radius. All
18 sides fit within 2.5 px, most within 0.5 px. The floor is `2.25m >= 3` px,
below which a tooth is sub-pixel decoration that cannot be seen to move.

Caps must be re-measured. Rows per tick come from the widest per-tick union of
changed rows across the delta bank, measured in-browser, and finer teeth change
fewer pixels per tick than coarse ones.

The index tooth (`MK_DEEP`, `MK_TIP`, `MK_BASE`) is retained but demoted to a
guard. Marking one tooth in `K` makes the rim's period `K` teeth, which fixes
the same aliasing without touching geometry — the right move if `K` must stay
even, and it is what shipped in the first pass. With `K = 7` it is redundant, so
`markPeriod()` now returns 1 (no mark, uniform rim) whenever `K` already shares
no factor with 4, 8, 12 or 15, and engages the mark only if a future `K`
reintroduces a common factor. The mark may only move **inward**: a meshing pair
has just `0.25 * m` of tip-to-root clearance, so the first attempt, a taller
marked tooth at `+0.495m`, drove the tooth into the wheel it meshes with.

`loopcheck.gear_marking_check()` rasterises every wheel and asserts the bank
resolves one distinct rim image per frame at all four tiers, in the feature's
own pitch units — `K/F` for teeth, `K * pin / N / F` for pinions. Three controls
stop it passing vacuously: a whole-tooth step on an unmarked rim must still
measure 0 pixels, proving the rasteriser can see a frozen rim at all; the
`K = 4` arithmetic must still resolve `[1, 2, 3, 15]`; and each tooth must still
change at least one pixel per tick, which catches a wheel shrunk to a blob.
Mutants confirm each bites: `K` reverted to 4, `K = 5`, one wheel moved off the
7 lattice, the mark grown outward, and the mark neutralised at `K = 4`
(reproducing the original fault) all fail.
