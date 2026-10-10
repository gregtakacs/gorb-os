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

## Two different bobs, and why the hub metric cannot see one of them

The pivot error above is one of **two independent** mechanisms. Kit2 had both.

| mechanism | signature | cures | kit3 status |
|---|---|---|---|
| **geometric orbit** — pivot offset `d` from the true axis | jewel orbits a circle of radius `d`; amplitude scales with `d` | art: centre the jewel on the pivot | **fixed**, verified (0.0000 px) |
| **lattice re-partition** — the inverse map truncates the 8.8 texel row origin (`clock_view.c:1336`) | the whole carriage holds rigid, then **snaps 1 px as one unit**, ~17 times per revolution | the 2-tap texel filter (`8cc76f6`) | **unconfirmed on kit3** — see "On-device measurement attempts" |

The hub-jewel centroid is measured *at the rotation fixed point*, which makes it
structurally blind to the second mechanism. A body-wide snap shifts every texel in
the carriage, jewel included, but the jewel's alpha-weighted centroid stays pinned
wherever the stored pivot says it stays. So "0.27 px, one row" proved the art was
centred — it never proved the bob was gone. **A clean `sim_tourb_bob.py` is not a
clean carriage.** Any host metric that samples only the on-axis feature cannot see
a body artifact; measure an off-axis feature (the balance rim, the escapement
bridge) or read the panel mirror instead.

### On-device measurement attempts: all three failed, and what they proved

Three probes read `display_fb_mirror()` after each commit — the bytes actually
DMA'd, so a mistake in the painter replica cannot bias them. **None of them
isolated the hub jewel, so none of them confirms or refutes the bob.** Recorded in
full because each failure mode is a trap for the next person:

1. **Byte order.** The mirror holds **native** RGB565. Decoding with
   `__builtin_bswap16` makes red look blue, so a redness test returns *nothing at
   all* — which reads exactly like "the jewel is not there" and is not a finding.
   The canvas restore (`v>>8|v<<8` into the band) is what makes this counter-
   intuitive; `s_canvas` and `fb` are in the **same** order.
2. **Colour alone cannot isolate the carriage.** A `R - max(G,B)` test fires on the
   animated background's warm metal. Requiring the pixel to differ from `s_canvas`
   removes the background plate but not the dial/glass composite, which is painted
   over the carriage inside the same window.
3. **Window size vs radius.** Rotation preserves distance from the pivot, so the
   carriage's second red blob (41 px at r=100.6, canvas row 132) can only appear
   ~100 px from the centre. A window that reported a red peak at row 227 was
   measuring **that blob**, not the jewel. The wide probe's centroids (232.949 …
   239.667) and its red band (rows 203..263) are exactly what a r≈100 feature
   sweeping through the window looks like.
4. **A self-inflicted one.** Scaling an already-integer-divided centroid by 8 makes
   every sample divisible by 8. That produced a beautiful "whole-pixel steps only"
   histogram which was pure instrumentation artifact — a textbook example of why a
   metric must be able to produce a *wrong* answer. Keep the ratio unreduced
   (numerator/denominator) and divide only at print time.

**Consequence, stated plainly:** the re-partition mechanism is NOT confirmed on
kit3. The host whole-body test contradicts it — rendering the carriage ink through
the shipped nearest map tracks the exact forward-map centroid to **0.27 px** p2p
across the revolution, whereas a genuine 1 px whole-body snap would show ~1 px of
sawtooth there. `8cc76f6`'s 2.23 → 0.25 px figure was measured on **kit2**, whose
jewel sat 0.707 px off-axis: that residual was the geometric orbit, not the snap.

So the honest state is: kit3's art fix is verified, the two candidate explanations
for what you still see are both unsupported by measurement, and the filter is a
**cost with no demonstrated benefit on kit3** — 1 fps, plus ghosting (below).
The next step is the one measurement that cannot be confused: build the composite
twice per commit, once with the carriage ctx skipped, and difference the two. That
is the carriage's exact contribution with no colour test and no background
contamination.

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
- **The 2-tap texel filter** (commit `8cc76f6`, `CLK_TOURB_FILTER`): the only cure
  so far for the **lattice re-partition** bob. Its own commit message measured the
  residual falling 2.23 → 0.25 px p2p with the ~16 whole-body snaps per revolution
  eliminated. It cost 7 → 6 fps because the pair's second row is a second PSRAM
  line stream per pixel (687 ns → 1.02 µs per blend), so it shipped compiled out at
  `0`. Reverted on cost grounds, which is what left the re-partition bob visible:
  kit3's art fix removes the *orbit*, not the *snap*. Do not judge it with
  `sim_tourb_bob.py` — that metric cannot see a body artifact (see "Two different
  bobs" above).
- **Blocking defect in the filter, measured on kit3**: re-enabling
  `CLK_TOURB_FILTER` makes `tools/sim_tourb_mark.py` **exit 1**. The 2-tap paints
  ~209 extra px per commit (+0.34%), and at ang 310.8° and 314.6° the chord marks
  leave one of those pixels unmarked, so it is composited but never restored on the
  tick that moves away — a visible GHOST, not merely a blur. The filter's own commit
  message claimed "the chord marks still cover every pixel, so nothing ghosts", and
  that was true of kit2's crop; kit3's wider carriage (249x310, pivot (124,187))
  breaks it. **The filter must not ship until the chord stamps are widened to cover
  the lower tap's row** (`tourb_stamp_pose` marks the ink chords of the row it
  samples; it must mark row+1 too). Measured on kit3 with one rebuild per side:
  nearest 8 fps / carriage 564 ns per blend / flush 119 ms, filter 7 fps / 794 ns /
  flush 135 ms.
- **Internal-RAM staging of the carriage's texel rows** (`CLK_TOURB_STAGE`, the
  prerequisite the filter commit named): attempted and measured NEGATIVE. Copying
  each tile's texel bounding box into `.bss` and blending from there made the
  carriage *dearer*, not cheaper — 5 fps, carriage 1550 ns per blend, flush 182 ms,
  against the filter's own 794 ns unstaged. Two causes, both measured: the box for a
  32x8 device tile at ~45 degrees spans ~840 texels while only ~256 blends consume
  it (3.3x over-read), and the per-row `memcpy` pair cost ~19,000 calls per commit.
  A fused single-pass copy is the second attempt; if that also fails, the existing
  column tiling is already extracting the PSRAM cache win and staging is the wrong
  lever. The 32 KB cache is shared with the GIF blit and the canvas restore, so the
  carriage does not own it.
- **Rounding vs truncating** the 8.8 row origin: no meaningful change **in the hub
  centroid**, which is why it was first written off — but the write-off was wrong.
  The partition snap is NOT a consequence of the off-axis pivot: the probe sees it
  on kit3, whose pivot-to-axis distance is exactly 0.0000 px. Rounding moves where
  the boundary falls, not whether one exists; only filtering makes position
  continuous in the angle.
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
