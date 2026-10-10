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

The fix was purely art-side: re-render the carriage shifted `(+0.5, +0.5)` texel
from the 2x source. Zero runtime instructions changed.

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
- **The 2-tap texel filter** (commit `8cc76f6`, `CLK_TOURB_FILTER`): reduced
  kit2's bob 1.70 → 1.60 px only, and cost 7 → 6 fps because the pair's second
  row is a second PSRAM line stream per pixel (687 ns → 1.02 µs per blend). It
  shipped compiled out at `0`, so its own commit message's fps claim was measured
  with the filter ON. On kit3's centred art it buys literally nothing: the bob is
  0.27 px with or without it. Reverted; the remaining runtime delta was four
  non-comment lines.
- **Rounding vs truncating** the 8.8 row origin: no meaningful change; the
  partition snap is a consequence of the off-axis pivot, not the cause.
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
