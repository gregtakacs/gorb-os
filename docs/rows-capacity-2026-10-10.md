# Rows-capacity benchmark — what can animate at 4/8/12/15 fps

Date: 2026-10-10 (second sweep; companion to
`docs/fps-budget-findings-2026-10-10.md`). Harness: `tools/bench/pt.sh`
queue under `/tmp/bench2/`, parsed by `tools/bench/rowsfit.py`.

## Question

The first sweep priced the layers. This one answers the art question directly:
**how many rows of the background may change per tick** — and how big the
carriage may be — to hold a **sustained** 4, 8, 12, or 15 fps. The second hand
ticks or sweeps on its own beat (1/2/4/15 Hz); the whole clock ticks at the
background's tempo, so the tick budget is `1000/fps` and everything must fit
inside it.

## Method

Everything is real: real kit bakes, real flashed assets, cadence-probe
measurements on device. No synthetic ink layers this time.

* **Driver**: `mkvariant.py animband` paints a deterministic 8-cycle tint
  animation into a full-width row band of the base frames, so exactly R rows
  change every tick. `rectaudit.py` predicts pushed px / row union per variant
  before any flash; every point below was audited first.
* **Configs**: `A` no carriage, no second hand; `B` no carriage, simple needle
  second hand sweeping at the tick; `C` carriage clipped to r≤150 (V-B art);
  `D` shipped carriage. A/B are re-baked kits without a tourbillon bank
  (`tourbillon_n=0`), not runtime flags.
* **Tempo**: the ladder runs at the binding 15-fps tick (`--bg-fps 15
  --tick-rate 15`); achieved fps falls out of the cadence. Frontier bands are
  re-confirmed at their own tempo (batch 2).
* Kit3's own GIF animates only rows 128..335 (~14.7k changed px/tick inside a
  208-row union) — the `*_real` points measure that authored art.

## The flush model this sweep measured

flush(tick) ≈ fixed(theme) + 0.21–0.27 ms × (DMA rows) + small px terms,
where the rows come from the UNION of (animated band ∪ whatever layer marks
force rows). Two facts decide every number below:

1. A row only costs DMA if its re-composited bytes actually change — the
   commit hook's compare gate drops marked-but-identical rows. A moving GIF
   row is ~0.21–0.27 ms; a marked-but-static row costs only its re-composite
   paint (~0.35 µs/px of dial + glass).
2. Rows are forced by MOTION at geometry granularity: the animated band, and
   the swept box of anything that re-poses. Art decides the union.

## Measured capacity (15-fps ladder; achieved fps is the sustain verdict)

Config A — carriage-less, second-hand-less (the clean bg-row price):

| animated band rows | flush ms | sustained fps at 67 ms tick |
|---|---|---|
| 0 (static)         | 50.4     | 15.6 ✅ |
| 96                 | 59.1     | 15.5 ✅ |
| 144                | 71.0     | 12.4 ✅(12) ❌(15) |
| 192                | 83.4     | 10.4 |
| 240                | 97.4     | 8.9 ✅(8) |
| 304                | 108.3    | 7.8 ❌(8 by ~1 tick unit — native confirm pending) |
| 368                | 122.0    | 6.9 |
| 448                | 135.9    | 6.2 (✅ 4) |
| kit3's own GIF     | 73.8     | 12.4 ✅(12) — real art changes 14.7k px inside 208 rows |

Fit: flush = 42.1 + 209 µs/row over rows 8..448 (n=9).

Config C — carriage r≤150 (V-B art), tick 15:
static 117.3 ms (7.9 fps — clears native 8 pending), +full-page band 211 ms (4.3).

Config D — shipped carriage, tick 15:
static 136.2 ms (6.7 — does NOT clear native 8 even with zero bg animation),
+full-page band 217 ms (4.2 — clears 4).

Config B — simple NEEDLE second hand sweeping (the trap):
static background ALREADY costs 134.9 ms flush; every band size lands 140–160
ms (5.4–7.6 fps). The needle's per-tick swept box (~300 rows of restore +
dial/glass re-blend, ~85 ms/frame) dominates everything the art can do. Ticking
it at 1 Hz does NOT help (150.3 ms) — `mark_all_layers()` re-marks the current
box of every layer every tick for tourbillon-less themes, moved or not; only
the tourbillon's chord-mark path (and the overlay's "never re-dirty what did
not change" rule) avoid this. A production non-tourbillon theme needs the same
chord/skip treatment for simple hands before 12/15 fps is on the table.

## The firmware lever this sweep exposed: `mark_all_layers()`

Tourbillon themes skip `mark_all_layers()`; non-tourbillon themes pay it.
It re-marks the FULL swept box of every shown layer every tick, moved or
not, so a stationary hand still costs its box's canvas-restore + dial +
glass re-blend every tick (~0.35 µs/px × box px ≈ 85 ms/frame for a
full-length needle, ~40 ms for the static hour/minute pair). Evidence:

| point | hand motion | flush ms | blends/tick |
|---|---|---|---|
| B_static (sweep needle) | needle moves every tick | 134.9 | 210,235 |
| B_r192_t1 | needle moves once/s | 150.3 | 210,253 |
| A_static (no sec hand)  | hr/min never move     | 50.4  | 50,758  |

Ticking the needle 1–4 Hz does not recover the cost — the box is re-marked
regardless. The event-mark path (`apply_angles` wedges + `gif_apply_frame`
pushes + the hook's all-layer re-composite on marked rows) makes the static
re-mark provably redundant, exactly as it did for the glass overlay
("never re-dirty what did not change"). `CLOCK_L4_MARK_STATIC_BOXES`
(clock_view.c, default 1 = shipped) disables it for the batch-6 re-runs
below; if the rows track the band with no frozen hands, the fix should ship
and every 12/15-fps capacity number rises.

## The decision table (kit-owner answer)

Native-tempo confirmations (queue5) — a band "holds" only if the cadence held
the target tick for the whole 30 s capture:

| target | carriage-less (no second hand) | carriage r≤150 | shipped carriage |
|---|---|---|---|
| 4  | ✅ full page (A_r448f4 4.18) | ✅ (C_r448 4.28) | ✅ (D_r448f4 4.04) |
| 8  | ✅ 304 rows (A_r304f8 7.8); ❌ 368 (A_r368f8 7.0) | static bg only ✅ (7.97 @ 121 ms); ❌ even a 144-row band (C_r144f8 6.8) | ❌ — carriage alone busts 8 fps with ZERO animated bg (6.9 @ 134 ms) |
| 12 | ✅ 160 rows (A_r160f12 12.42); ❌ 192 (A_r192f12 10.2) | ❌ (carriage alone 117 ms + row union) | ❌ |
| 15 | ✅ 96 rows (A_r96 15.5); ❌ 112 (A_r112 12.4) | ❌ | ❌ |

Carriage-less + simple sweeping needle (config B): nothing above 4 fps —
140–160 ms flush at every band size, at any hand rate, because the hand's
swept box is re-marked every tick (see the mark_all_layers section above).


## Limitations of this sweep

1. Bands are synthetic (tint-filled full-width motion), deliberately the
   worst case per row: every pixel of every band row changes. Real art can be
   cheaper at the same row count — kit3's own GIF changes 14.7k px inside its
   208-row union and costs what 63 full rows cost (A_real, 73.8 ms).
2. Bands are contiguous and centered; the packer caps (rect budget / page
   collapse) were raised for the ≥304-row rungs via --bg-max-rects/--bg-max-flush,
   so very wide art animation also needs those bake flags in production
   bakes or it silently degrades to full-page rects.
3. The needle in config B is authored test art (2,050 ink px, full length);
   a kit-owner needle differs in detail but the box-mark cost is geometric,
   not ink-count dependent.
4. One 30 s capture per point; capture-to-capture flush noise is ±5 ms (the
   A_r48/r96 inversion above is that noise plus band-position effects).
   Frontier claims use the last rung that HELD, so the table is conservative.
5. fps here means sustained tick rate from the cadence probe — achieved fps
   is 1000/(flush + ~12–17 ms idle/TE/submit), which is why flush rungs and
   fps rungs interleave; the native-tempo confirmations in the next table are
   the authoritative pass/fail, not the 15-fps-ladder interpolations.

## Second-hand modes measured (sweep vs 1/2/4 Hz tick, bg at 15 fps)

| point | sec-hand behavior | flush ms | verdict |
|---|---|---|---|
| B_r192 (sweep)  | moves every tick | 150.0 | 6.4 fps |
| B_r192_t1/t2/t4 | moves 1/2/4 per s | 150.3 / 145.9 / 150.3 | unchanged |
| B_r304_t1/t2/t4 | " | 147.8 / 148.2 / 150.9 | unchanged |
| A_r192          | no second hand at all | 83.4 | 10.4 fps |

The needle's own blending amortizes to zero when ticking (sec blends drop to
~2k/tick), yet the flush does not move: the cost is the per-tick re-mark of
the hand's swept box (`mark_all_layers`), not the hand. Batch 6 (`F_*`
points) re-measures the ladder with that re-mark disabled.

## Batch 6 — with the static-box re-mark disabled (`CLOCK_L4_MARK_STATIC_BOXES=0`)

Same bakes, same tick ladders, one compile-time change:

| point                    | shipped flag flush / fps | flag-0 flush / fps |
|---|---|---|
| F_A_r96  (tick 15)       | 59.1 / 15.5              | **50.0 / 15.6** |
| F_A_r144 (tick 15)       | 71.0 / 12.4              | 70.3 / 12.4 |
| F_A_r192 (tick 15)       | 83.4 / 10.4              | 78.9 / 11.1 |
| F_A_r240 (tick 15)       | 97.4 / 8.9               | 84.6 / 10.1 |
| F_A_r304f8 (tick 8)      | 109.6 / 7.8 MISS         | **97.7 / 8.4 HOLD** |
| F_B_static (tick 15)     | 134.9 / 7.6              | **54.5 / 15.5** |
| F_B_r96  (tick 15)       | 140.6 / 6.9              | **72.8 / 13.1** |
| F_B_r144 (tick 15)       | 150.0 / 6.4              | 87.6 / 10.4 |
| F_B_r192 (tick 15)       | 150.0 / 6.4              | 99.9 / 9.3  |
| F_B_r240 (tick 15)       | 154.8 / 6.2              | 116.6 / 7.8 |

What this says:

* The non-tourbillon clock with a SIMPLE SWEEPING second hand sustains
  **15.5 fps** on a static background (F_B_static) — impossible under the
  shipped marking (7.6). Sweeping is affordable once the box re-mark is gone;
  ticking it is NOT required.
* The needle adds ~125 DMA rows of its own per tick while sweeping (its box
  minus the rows the compare gate drops) — the user's 12-o'clock worry is
  real under box marking and near-free under event marking + compare gate.
* 8-fps frontier without carriage widens from ~304-borderline to ~304-clean
  (97.7 ms at the 125 ms tick); 368 rows becomes plausible but is untested
  under the flag.
* 15-fps frontier (band rows, needle sweeping) sits at 96 (72.8 ms holds 12,
  misses 15 at band 96+needle; ~64-row bands are the untested 15-fps frontier
  with a sweeping needle). Without a second hand: 96 held (50.0 ms).




