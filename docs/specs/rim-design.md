# Rim design: the ring sends only its own lanes, and it knows which way it runs

Slice **rim** of issue #11, `docs/app-platform-plan.md` §64.11 (rim lanes) together with the
ring's direction and start (§64.8, the deferred overlay-design amendment 1). Design only: this
file, `rim-1`..`rim-3` and `rim-chain.json`. No product code here; the three steps write it.

## 0. The rulings that bind this slice

- **§64.11: the owner found the slow breathe poor** — steppy at the 2 Hz grid, slow at the 4 s
  floor. "The cost was never the ring's 10k pixels but the rows they sit in." **Ruling: rim
  lanes** — a ring-only refresh sends the band itself as narrow windows (left and right slabs,
  short spans across the top and bottom), blended from the mirror, aligned to the CO5300's
  even-column rule, timed into a blanking window; breathe, flash and the draining arc then step
  at up to ~25 Hz. §64.1's "slow breathe only" is relaxed to a design number of this slice,
  measured on Bronze.
- **§64.7/§64.8: the ring takes a direction and a start**, and they ride the record's reserved
  words so the API stays one call. overlay-design's amendment 1 spells the fields and was not
  built; this slice builds it, in the same change as the lanes' new stamp geometry.
- **The ring is still the OS's and still the copy arc's twin** (§64.1, ring-design §0/§8):
  stamped into the DMA tiles as they leave, never into the mirror `s_fb`, so `/fb` cannot see it;
  the copy arc wins the band while a card copy runs; nothing on the untiled fallback;
  `gorb-os-nothing-off-the-glass` still holds (an app off the glass sets the ring and draws
  nothing itself).
- **Internal RAM is the cliff** (`gorb-os-internal-ram-under-clock`, `gorb-os-frozen-panel-witness`:
  ~23 KB free and a ~7.2-7.4 KB largest block under the clock, and a few hundred internal bytes
  once froze the panel because the tile ring needs a free 18,640 B block). This slice adds **zero
  new internal statics and zero new internal DMA allocations** (§7) — it reuses the tile ring it
  already shares with the flush.
- **The bus is one lock** (`main/display/display.c` `s_ring_mux`): every submitter takes it. The
  lane refresh takes the same lock, in the same commit, and never adds a second one.
- **The CO5300 clocks 2 px per cycle on both axes**: a window starts on an even column and spans
  an even number of columns, and `DB_EVEN_BANDS` does the same to rows. Every lane obeys both
  rules by construction, and the host suite proves it.
- **Host suites are the cheap signal** (`gorb-os-2-host-suites-are-cheap`): everything with a
  verdict is pure C compiled by `tools/host_ring_test.py` (and the pomodoro's by
  `tools/host_pomodoro_test.py`), with mutants that must bite — a guard no mutant breaks is a
  claim, not a test.
- **No device, no ESP-IDF in the sandbox.** The glue is thin, and every spec names the files only
  an `idf.py build` checks. The orchestrator builds, flashes and benches (§9); workers never touch
  a board or a serial line.

## 1. What the slice adds, mapped to §64.11

| the ruling | this slice | step |
|---|---|---|
| "the flush keeps one dirty span per row, so a ring step re-sent ~216k pixels" | `ring_lanes()`: the annulus as ≤64 even-aligned rectangles, 19,912 px | rim-2 |
| "a ring-only refresh sends the band itself as narrow windows … blended from the mirror" | `send_ring_lanes_locked()` in the commit, pixels out of `s_fb` + `orb_ring_disp_stamp()` | rim-3 |
| "timed into a blanking window", "never fights an app frame" | the lanes ride the flush's own commit, after its bands, under `s_ring_mux`, after the TE wait | rim-3 |
| "left and right slabs, short spans across the top and bottom" | `orb_ring_lane_t` kinds, §4 | rim-2 |
| "breathe, flash and the draining arc step at up to ~25 Hz" | `ORB_RING_STEP_MS` 500→40, the alpha gate, the batch governor | rim-3 |
| "the period floor becomes a design number, measured on Bronze" | floors 1500/400 ms, the A/B table in §4.4, the bench in §9 | rim-3 |
| the ring's direction and start (§64.8, overlay amendment 1) | `flags` (`ORB_RING_MATCH` 0x01, `ORB_RING_CCW` 0x02) and `start`, the span maths, the reference matrix | rim-1 |
| the pomodoro sets them (`ring_dir`, `ring_start`, "Match the app") | `pm_cfg_t.dir/.start/.match`, `pm_ring_t.dir/.start/.match`, `ring_send()` | rim-1 |

## 2. The record: `flags` and `start`

Byte-1 `match` becomes `flags`, and the reserved `uint16_t _r1`/`_res1` becomes `start`. The
size, the offsets and the wire signature do not move, so `imp_ring_set`'s verbatim 16-byte
`memcpy` and `binds.json`'s `i(i)` are untouched (overlay-design §5's argument still holds).

```c
#define ORB_RING_MATCH      0x01u  /* arc/track take the live theme's overlay colour (§64.9) */
#define ORB_RING_CCW        0x02u  /* the arc runs anticlockwise from its start (§64.8)      */
#define ORB_RING_FLAGS_MASK 0x03u

typedef struct {
    uint16_t fraction;    /* 0..1000, from twelve in the record's direction              */
    uint8_t  effect;      /* ORB_RING_STEADY | _FLASH | _BREATHE; other => STEADY        */
    uint8_t  flags;       /* ORB_RING_MATCH | ORB_RING_CCW; ring_clamp masks to 0x03     */
    uint32_t arc;         /* 0xAARRGGBB                                                  */
    uint32_t track;       /* 0xAARRGGBB                                                  */
    uint16_t period_ms;   /* clamped by the OS; 0 => the OS default                      */
    uint16_t start;       /* degrees clockwise from twelve; >359 wraps, ring_clamp folds */
} orb_ring_rec;           /* 16 bytes, ILP32 LE */
```

Offsets (LE): fraction 0, effect 2, flags 3, arc 4, track 8, period_ms 12, start 14.
`apps/sdk/orb.h` names the same layout `orb_ring_t` (byte-1 `match` becomes `flags`, `uint16_t
_res1` becomes `start`), `orb_ring.h` as above, and `apps/README.md` restates both words — three
restatements of one record, each with a
`_Static_assert` still at 16 bytes, and `orbsdk.py`'s size gate keeps them honest. `ring_clamp`
masks `flags` to `ORB_RING_FLAGS_MASK` (an unknown bit is dropped, not honoured) and folds
`start` with `start %= 360`; both are idempotent, and an app that sets `start` or `ORB_RING_CCW`
on firmware before this slice keeps its own colours and a twelve-clockwise ring — the fields it
  wrote were reserved.

## 3. The span maths with a start and a direction

`ring_row_parts` grows two arguments; the record is not passed (the stamp keeps taking a plain
fraction plus what it resolved at the step, so a per-tile call still costs no more than it does
today):

```c
/* The pieces of row y for this fraction, starting angle and direction. start_deg is degrees
 * clockwise from twelve (any value; it wraps); ccw is the ORB_RING_CCW bit's 0/1. Same contract
 * as today: up to max pieces, in increasing x, covering the annulus exactly once. */
int ring_row_parts(int y, int fraction, int start_deg, int ccw,
                   orb_ring_part_t *parts, int max);
```

**The arc is a wedge, not a prefix.** CW it is `[start, start + frac·360°)`; CCW it is
`(start − frac·360°, start]`. Both are one cyclic interval, so `ring_row_parts` solves it as
*one interval* on a rotated angle: `A0 = start`, `A1 = start + s` with `s = frac·2π/1000` for CW,
and `A0 = start − s`, `A1 = start` for CCW, with the inside test keeping the direction it came
from. The annulus spans of a row come from the integer-radius bisectors that are already there
(`p_hit`/`e_first`/`e_last`: four-times-squared, exact, no floating point at the radius boundary),
and the wedge cuts them with **two ray crossings**, one per boundary angle, each from the same
half-plane algebra as today's single ray.

> **The monotone-angle lemma (why two rays are enough per row).** Along a glass row the clockwise
> -from-twelve angle is monotone in x within each side of the centre, with its only discontinuity
> at twelve itself: above the centre the left half runs 270°→360° and the right half 0°→90°, below
> the centre the whole row runs monotonically 270°→90°. So a cyclic interval cuts each annulus span
> in at most one interval, and a row never needs more pieces than today: `ORB_RING_PARTS_MAX`
> stays 4, and the suite asserts the maximum it sees, so relaxing that bound is a visible decision
> and not a silent edit.

Fraction 0 is no arc and 1000 is the whole annulus, whatever the start and the direction (the two
degenerate cases short-circuit before the trig). `start_deg` is read modulo 360; a ray that is
horizontal (`|cos| < ε`) crosses no row and is decided by the sign test the current code already
uses. All the trig — two `sinf`/`cosf` pairs — happens once per row call, off the pixel loop,
exactly as ring-design's amendment 1 requires.

**How it is proved.** The suite's reference stays the copy arc's own per-pixel test (`atan2f`,
radius in and out, and now `in_wedge(a, A0, A1, ccw)`), and the matrix is `start ∈ {0, 90, 180,
270, 45, 359}` × `ccw ∈ {0,1}` × the existing fraction sweep (`{0,1,125,250,499,500,501,750,999,
1000}` plus 50 pseudo-random), for every one of the 466 rows, with today's rule unchanged:
coverage exact, no duplicates, nothing outside the glass, and at most 1 px of arc/track
disagreement and only on a ray boundary. `start = 0, ccw = 0` is the shipped case, so the old
expectations are the same numbers, and the mutants **extend** rather than replace: the CCW branch
inverted, the start offset dropped, `A1` taken as `A0 − s`, the modulo-360 fold deleted, and every
existing span/boundary mutant stays.

## 4. Rim lanes: the geometry, and what it costs

### 4.1 The set

`R_IN 224`, `R_OUT 231`, centre (233, 233) on the 466² glass: the annulus is **10,004 pixels**
over **462 rows** (rows 2..463; rows 2..8 and 457..463 are the top and bottom caps, where a row's
annulus is one span across the middle, and rows 9..456 are the sides, where it is a left slab and
a right slab). `ring_rows_for` answers `[2, 464]` and `mark_band()` calls `display_mark_rows(2,
464)` inclusive, so the whole-row re-send a ring step pays is **463 × 466 = 215,758 px** — one row
more than the annulus needs, because the answer is the band's rounding rather than its geometry.

A **lane** is one rectangle of that cover, and a cover is a partition of the rows into bands:

- a band whose rows are all side rows gives two lanes, the **left slab** and the **right slab**,
  each the union of its rows' spans;
- any other band — the caps, and the two mixed bands at the very top and bottom — gives one
  **span** lane, the union of all its rows' spans.

Every lane is aligned before it is billed: `x0 &= ~1` and the width rounded up to even (the
CO5300's window rule), and the band's row count is even with the start row even
(`DB_EVEN_BANDS`' rule, so a lane's window is legal to the panel driver verbatim).

### 4.2 The algorithm (pure, and the only thing that decides the numbers)

Greedy, top to bottom, no table committed anywhere — the same code produces the cover on the build
host, in the firmware at boot, and in the mutation bench:

```
y = 2
while y <= 463:
    take the tallest band [y, y+B-1] with B even, B <= ORB_RING_LANE_ROWS_MAX (64), y+B-1 <= 463,
      whose aligned cover's area minus the annulus pixels it must carry is <=
      ORB_RING_LANE_EXTRA_MAX (384); a band of 2 rows that still overruns is taken anyway,
      because it is the smallest legal band
    emit its one or two lanes; y = y + B
```

```c
typedef struct { int16_t x0, y0, x1, y1; } orb_ring_lane_t;   /* closed, even-aligned */
#define ORB_RING_LANES_MAX      64    /* size the caller's array this                  */
#define ORB_RING_LANE_ROWS_MAX  64    /* the greedy's band-height bound                */
#define ORB_RING_LANE_EXTRA_MAX 384   /* the greedy's per-band slack bound (A/B, §4.4) */
#define ORB_RING_LANE_MAX_PX    1024  /* every single lane is at most this many pixels */

/* The band's cover: the lane rectangles, in increasing row, into the caller's array; the count
 * returned, 0 for bad arguments or when max is too small. Deterministic and pure: the same
 * constants give the same cover on the host, in the simulator and on the orb. */
int ring_lanes(orb_ring_lane_t *lanes, int max);
```

### 4.3 What it comes to, at the shipped constants

Exactly, and pinned by the suite from the shipped source rather than from a committed table:

| | shipped |
|---|---|
| lanes | **60** (bound 64) |
| pixels sent | **19,912** (budget **≤ 20,000**) |
| pixels that are annulus | 10,004 |
| extra pixels sent (slack + alignment) | **9,908** (bound **≤ 10,000**) |
| the widest single lane | 944 px (bound 1,024, so a lane is 1,888 B on the wire) |
| first / last lane | rows 2..9 and rows 456..463, both `x 174..291` |
| a 41-row slice, rows 100..140 | 6 lanes, 1,856 px (the worst 41-row window anywhere is 13 lanes, 3,400 px) |

Against the whole-row re-send it replaces: **9.2 % of the pixels**, and the extra over the ring's
own 10,004 pixels is under a pixel per sent pixel. The suite asserts each of these numbers, that
the union covers every annulus pixel, that it covers nothing outside the glass, that every lane
is even in x, width, row and row count, that the lanes are in increasing row order and that no
two overlap the same annulus pixel twice. Its mutants bite on each of those claims (§9, rim-2).

### 4.4 The A/B knobs, and the coarse settings the bench may ask for

The two `#define`s are the whole tuning surface; the suite re-derives the bounds for whatever
value is shipped (the bounds are stated as invariants of the algorithm: sent ≤ annulus +
lanes × bound, and each lane ≤ `ORB_RING_LANE_MAX_PX`). Computed from the same greedy:

| `LANE_EXTRA_MAX` | lanes | px sent | extra px |
|---|---|---|---|
| 200 | 86 | 16,960 | 6,956 |
| **384** | **60** | **19,912** | **9,908** |
| 600 | 47 | 22,540 | 12,536 |
| 1024 | 35 | 26,612 | 16,608 |
| 2048 | 24 | 33,104 | 23,100 |

384 is chosen because the owner's currency is pixels and 19,912 is inside the ruling's 10-20k. If
Bronze says the per-window cost is what hurts (the `esp_lcd` command/CS setup per transaction,
which the repo has measured but not published in µs), the bench raises the bound to 1024 — 35
windows for 26.6k px — with no other change, and §6's governor keeps the bus share the same. That
decision is a bench decision and it is §9's to make.

### 4.5 A partial refresh, for the draining arc

A steady fraction move changes the arc/track boundary and nothing else, so it owes only the lanes
whose rows the swept wedge touches — the same sweep arithmetic `mark_span()` does today (in the
IDF glue, fed by `ring_rows_for()`), applied as a filter:

```c
/* The lanes intersecting rows [y0,y1], in order, into the caller's array; the count returned.
 * Pure, so the filter a steady drain runs through has a test: full range answers the full cover. */
int ring_lanes_in(const orb_ring_lane_t *lanes, int n, int y0, int y1,
                  orb_ring_lane_t *out, int max);
```

A row range that hits nothing answers 0 and the step sends nothing at all (a fraction move under
`ORB_RING_STEP_PCT` never reaches here, and a wedge that lands in the gap between two bands is
worth no pixels). Measured over the shipped cover, a sweep of 2 to 6 rows costs **1 to 5 lanes and
384 to 1,808 px sent**, and the pomodoro's drain — 4 ‰ a minute, so a degree of ray a minute — lives
in that range, against the tens of whole rows it costs today.

### 4.6 What lanes do *not* change

The lane cover is a property of the band only: it does not depend on the fraction, the start, the
direction, the colours or the alpha, because the track paints the rest of the annulus and an
alpha-0 lane still has to restore the glass under it. The cover is computed once, at
`orb_ring_disp_init()`, into the PSRAM static §7 names.

## 5. The refresh: how the lanes reach the glass

### 5.1 The one decision the design had to make

The lanes are sent **inside the flush's own commit**, not by a second path onto the bus. The
reason is the ruling's own sentence — the refresh must not fight an app frame, and it must land
in a blanking window: the commit is the one place that has already waited for the edge
(`present_wait()` / `wait_for_te()`), already holds `s_ring_mux` (`dma_dirty_bands()`) and already
has the tile ring's write-guard in hand. Anything else is a second submitter with its own TE race,
and `band_commit_here` shows what paying for one transaction at a time costs.

So the change is one call in `dma_dirty_bands_locked()`, after the band loop and inside the
tile-mode branch, plus the same call in `fx_frame()` where the ring is already stamped:

```c
/* display.c, LVGL task, s_ring_mux held, the bus quiet, the blanking window open. The ring's
 * lanes, in row order: the mirror's pixels with the ring blended into them, one CO5300 window
 * each. Lanes whose rows this commit already sent whole are skipped — the stamp sites put the
 * ring into those tiles on their way out. Returns the number of windows submitted, 0 when the
 * ring owns nothing. Never writes s_fb. */
static int send_ring_lanes_locked(const orb_ring_lane_t *lanes, int n,
                                  const int32_t *band_r0, const int32_t *band_r1, int bands);
```

and two thin entry points on the ring's side of the seam, in `display.h`:

```c
bool display_ring_lanes_ready(void);                           /* tiles funded, mirror up        */
int  display_ring_lanes(const orb_ring_lane_t *lanes, int n);  /* LVGL task; takes the mux itself */
```

`orb_ring_disp_service()` asks `display_ring_lanes_ready()` before it chooses its shape for the
step: lanes when they are ready, `display_mark_rows()` (today's whole-band mark) when they are
not. That is the degradation path — a parked or one-buffer ring, or a mirror-less build, gets
today's behaviour, byte for byte, and logs once.

### 5.2 The per-step data flow

1. `orb_ring_disp_service()` (LVGL task, every pass) decides at the grid whether the ring owes the
   glass a frame: an effect step, a colour/owner/theme change, a fraction move past
   `ORB_RING_STEP_PCT`, or a ring that went away. It fixes `s_alpha` for the step, as today, and
   it never reads the clock inside the stamp.
2. If the copy arc owns the rim (`orb_edge_arc_shown()`), the ring sends nothing and its state
   simply does not advance; the arc's own show/hide already re-sends the whole page, and the ring
   returns on the first step after the copy with no work for the app (ring-design §8, unchanged).
3. Otherwise the glue fills a pending set — the whole cover for an effect or a colour step,
   `ring_lanes_in(…, y0, y1)` for a fraction sweep — into a PSRAM index (a first/last lane pair;
   the cover itself is the source of truth), sets `s_lanes_due`, and invalidates the same
   off-glass corner pixel the arc uses, so a still page carries the frame.
4. The commit runs: the app's bands go as they always did, then `send_ring_lanes_locked()` walks
   the due lanes **in row order** (the panel scans top to bottom; submitting in scan order is what
   keeps the tear line honest when a batch spans an edge) and, per lane: `memcpy`s the mirror's
   rows into a tile (stride = the lane's width, which is what `orb_ring_disp_stamp`'s pointer
   arithmetic already assumes), calls the existing `orb_ring_disp_stamp(row, rows, tile, x0, w)`
   so the blend is the shipped `ring_blend565` on byte-swapped RGB565 and nothing else, bumps
   `s_bench_pix` / `s_bench_chunks` so `/health` counts the ring's wire the way it counts
   everything else, and submits `esp_lcd_panel_draw_bitmap(x0, y0, x0+w, y1+1, tile)` with the
   same `s_band_inflight` / `s_tile_done` bookkeeping the tile loop uses (a lane is one tile: one
   transaction, one trans-done). `s_last_pending` is set while windows are outstanding, so
   `lv_display_flush_ready()` still comes from `on_color_trans_done()`, and
   `dma_dirty_bands_locked()` returns the lane count when it had no bands of its own so the
   caller's zero-band shortcut cannot fire while the ring's windows are on the wire.
5. The batch's wall time is measured by the caller of `display_ring_lanes()` and fed to the
   governor (§6.3).

Nothing writes `s_fb`: the lanes are read out of it. `/fb`, `display_capture_*` and pixdiff keep
reading the mirror, so the ring stays invisible to the bench exactly as the copy arc is
(`gorb-os-edge-arc`, `gorb-os-frozen-panel-witness`).

### 5.3 The merge with an app's own frame

If this commit already sends a band covering a lane's rows, that lane is **dropped** — the
existing rim dispatch stamped the ring into those tiles on their way out at this step's `s_alpha`,
and the mirror is complete by commit time. The rule is "covered means sent", it is one comparison
per lane, and it is why the ring costs nothing extra over a dial that repaints the rim anyway.
`fx_frame()` (the carousel) is the same story from the other side: it stamps the ring into every
slab it composes, so its frames need no lanes at all, and the lane call there exists only for the
case where the carousel's own frame does not touch a ring row — where it returns 0 windows.

### 5.4 Buffering, and the cliff

The lanes are copied into **the flush's own ping-pong tiles** (`s_tile[2]`,
`MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA`, 18,640 B each), under `s_ring_mux`, with the write-guard
that already proves a buffer's previous transaction retired. This is the answer to "size it small,
reuse a tile, or justify PSRAM-DMA":

- a dedicated buffer would be 2 KB (every lane is ≤ `ORB_RING_LANE_MAX_PX` = 1,024 px) of internal
  DMA RAM that the tile ring does not have when the card has lent it, and the panel freezes when
  the ring cannot be re-funded at 18,640 B;
- PSRAM-direct is not available at all: this IO was created at `trans_queue_depth 2`, and a PSRAM
  submit at depth > 1 piles bounce copies into exactly the pool the cliff is made of;
- reusing the tile buys the guard, the FIFO retirement proof, the bench counters and the
  degradation semantics for free, and it works for any lane width;
- the fall-back when `s_tiles_n < 2`, `s_ring_parked` or `s_fb == NULL` is the current whole-band
  mark, which needs nothing new.

A lane is one transaction, and every lane of one step rides the same two buffers, so the batch's
memory shape is fixed by the ring, not by the geometry.

## 6. The effect clock: 25 Hz, the floors, and the governor

### 6.1 The constants (`orb_ring.h`; rim-3)

| constant | today | rim | why |
|---|---|---|---|
| `ORB_RING_STEP_MS` | 500 | **40** | the grid the ruling asks for: 25 Hz |
| `ORB_RING_BREATHE_MIN_MS` | 4000 | **1500** | a 1.5 s period at 25 Hz is 37 steps: 0..255 in 8-unit jumps with no visible step, so "slow only" is no longer what keeps breathe cheap; the pomodoro still asks 4000 |
| `ORB_RING_FLASH_MIN_MS` | 1000 | **400** | flash is two transitions, so the floor is 5 transitions a second; the 8-unit gate keeps it to ≤10 sends a second |
| `ORB_RING_DEFAULT_PERIOD_MS` | 4000 | 4000 | unchanged: an app that names no period gets a slow breath |
| `ORB_RING_STEP_PCT` | 5 | **2** | the lanes make a small steady move cheap; 2 ‰ is the drain's own granularity, and the pomodoro keeps its 4 |
| `ORB_RING_STEP_ALPHA` | — | **8** | new: a breathe/flash step sends only when the resolved effect alpha moved ≥8 from the last one *sent*, or when it hit 0 — the gate that keeps a 25 Hz grid from re-sending a pixel that did not change |
| `ORB_RING_STEP_MAX_MS` | — | **400** | new: the governor's ceiling (§6.3); it is today's grid, so a starved ring is never worse than the slice's base |

`ring_clamp` honours the floors exactly as it does now (a record can only be slowed down), and
`ring_effect_alpha` is unchanged in shape; the gate is new and pure, so it has a test:

```c
/* 1 when an effect step at `now_alpha` owes the glass a refresh after one at `was_alpha` (§6.1). */
int ring_alpha_moved(int was_alpha, int now_alpha);
```

### 6.2 The cost arithmetic, in the repo's own currency

80 MHz QSPI, quad mode: ~40 MB/s, and the transition bench's wire-bound figure of 24.7 ms for a
434,792-byte page (§48/§49.2) — the pixels are ~1.9 ns each, and a transaction's fixed
command/CS phase is the other term, which is why §4.4 exists.

| | a breathe step, paused pomodoro (4 s period) | the same ring sustained |
|---|---|---|
| today, 2 Hz grid, whole rows | 215,758 px (463 rows × 466), ~24 tiles | 431,516 px/s of wire, plus the mirror copy the ring never needed |
| rim, 25 Hz grid, lanes, gate 8 | 19,912 px (60 windows), ~0.94 ms of wire | the grid caps it at 25 sends/s: ~500,000 px/s of *ring* pixels — and **0 px/s of the app's rows** |

The pixel rate at the top of a fast breathe is of the same size as today's, and the whole
difference is what those pixels are: today's 2 Hz step re-sends the app's own rows and re-draws
215k px the ring did not change, which is exactly what §64.11 says the owner rejected. With the
gate and the governor:

- the pomodoro's 4 s breathe resolves to ~16-20 sends/s, so ~320-400 kpx/s of ring pixels;
- a 2 s breathe at the floor resolves to ~25 sends/s (the grid caps it), ~500 kpx/s;
- a steady drain sends only when the fraction moved ≥2 ‰, and only the lanes in the swept rows:
  1-5 lanes, 384-1,808 px;
- a flash at its 400 ms floor is 10 sends/s of the full cover, ~200 kpx/s.

### 6.3 The governor

One pure function, so the rule has a test and the glue is three lines:

```c
/* The next step's grid in ms, given how long the last lane batch took in microseconds (§6.3):
 * never below ORB_RING_STEP_MS, never above ORB_RING_STEP_MAX_MS, and never less than three
 * times the batch's own length. */
int ring_next_step_ms(int batch_us);
```

A batch of ≤13 ms keeps the full 25 Hz; 30 ms drops the ring to ~11 Hz; a board where a batch
costs 130 ms lands on the 400 ms ceiling, which is today's behaviour with the lane pixels. The
3× factor is the ring's promise that it never occupies more than a third of its own grid, so a
live app keeps two thirds of the LVGL task's commit window whatever the ring decides to do. The
glue logs the first batch and every grid change (one `ESP_LOGI` line naming the lane count, the
batch's µs and the new grid), which is how the orchestrator reads the real number without a
serial protocol change.

## 7. RAM accounting

- Lane cover: 60 × 8 B = **480 B**, `EXT_RAM_BSS_ATTR`, PSRAM, built once at
  `orb_ring_disp_init()` — an app that never sets a ring never builds it.
- Pending-set index (two int16 row bounds), the last alpha sent, the step clock, the governor's
  grid and a couple of counters: ~24 B, PSRAM statics beside the ownership table already there.
- **New internal .bss: 0 B. New internal DMA allocations: 0 B** (§5.4 reuses the tile ring). The
  stack usage of `send_ring_lanes_locked()` is a `memcpy` and a call the stamp already makes;
  nothing new runs on a new task or stack.
- Wire: a ring step is 19,912 px in 60 windows (~40 KB) instead of 215,758 px in ~24 windows
  (~430 KB); the 18,640 B block the panel depends on is untouched by this slice.

## 8. The pomodoro

`ring_dir` (1 = anticlockwise) sets `ORB_RING_CCW`, `ring_start` (0..3 → 0/90/180/270) sets
`start`, and `ring_col_away == -2` ("Match the app") while the app is off the glass sets
`ORB_RING_MATCH`; the two settings already exist in `app.json`, so the app's face, its editor and
its 14 visible settings do not move. The mapping lives in `apps/pomodoro/main.c`'s `ring_send()`
(next to the `ORB_RING_BREATHE` mapping already there); the asks come across the pure boundary as
plain `pm_cfg_t.dir`, `.start`, `.match` ints and `pm_ring_t.dir`, `.start`, `.match` ints,
because `pomodoro.h` must not name an `orb_*` symbol and `pomodoro.c` must not float (the rule
`host_pomodoro_test.py::shape` enforces). `ring_moved()` compares the two new words, so a change
of direction or start re-sends the ring. The comment near `ring_send()` naming amendment J goes
away with the code that needed it. `version.txt` stays **v1.7.0** — the branch has not bumped it
since origin and §64.8's settings shipped with it.

## 9. The gate

Sandbox (every step's verify runs its own half; `set -o pipefail` wherever a verify pipes):

1. `python3 tools/host_ring_test.py` → `RESULT: pass`, and `--mutations` → `mutants caught N/N`
   with no `MISSED` line, by **exit status** (ring amendment 3).
2. `python3 tools/host_pomodoro_test.py --mutations` → `mutants caught 54+/54+`, exit 0.
3. `gcc -std=c11 -Wall -Wextra -Werror -I main/display -c main/display/orb_ring.c -o /dev/null` →
   exit 0: the core is still IDF-free.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → green (the SDK's 16-byte record gate,
   the factory apps, the default theme, the shelf).
5. `ORB_CLANG=/usr/lib/llvm-21/bin/clang python3 apps/sdk/orbsdk.py build apps/pomodoro` → exit 0
   (the app's C really compiles against the new header, which `check-apps` does not cover: the
   pomodoro is not one of the five factory containers).
6. New symbols are grepped for, and a verify fails when a name is absent — each step's own list.
7. `make check-host` is **excluded** in this sandbox and no step runs it:
   `host_boot_overlay_test.py` (its shim's `ld` fails), `host_geo_test.py` and
   `host_tzrule_test.py` (need `tzdata==2026.5`; the bake names 2026e), `host_idle_dim_test.py`
   (needs `build/config/sdkconfig.h` from an IDF build), `host_json_test.py` (its driver binary
   times out on this glibc) and `host_layout_carry_test.py` (needs `IDF_PATH`'s
   `nvs_partition_gen.py`) are environment failures on the base commit, not this slice's. The
   orchestrator runs the full gate on the host.
8. No step edits a test outside the slice to make a gate pass, and no step touches the hub.

Bench, on Bronze, by the orchestrator (4.3, 6.2 and the §4.4 choice are the things being checked):

- `idf.py build` clean; the boot log's tile-ring line unchanged (2 × 18,640 B), `glass.tiles` 2
  through a radar card fill and a 1.5 MB write, internal `min_free`/`dma_min_free` low water no
  worse than the base build's.
- fps with the pomodoro paused (a 4 s breathe) over radar and over flights, same window, before
  and after; the ring's real grid and batch µs come from the log line §6.3 promises.
- the draining arc over a live dial: `/health` px/s, and no whole rows attributed to the ring.
- no tearing by eye: a breathing ring over the clock's second hand, the owner's verdict;
  `display_bench_scan`'s `lag_max_us`/`late_tiles` for the witness.
- `/fb` unchanged under a ring (the mirror cannot see it), a copy arc still wins the rim mid-copy
  and the ring returns after, and OTA/park behaviour is untouched.
- the pomodoro's `ring_dir`/`ring_start` on the glass, all four starts, both directions.
- §4.4's A/B: the same bench at 384 and at 1024, and whichever the numbers prefer ships.

## 10. Slices → steps

- **rim-1-flags-start** — the record's `flags` and `start` everywhere they are named, the span
  maths rotated to a wedge with the reference matrix, `ring_clamp`, the SDK, `apps/README.md`, and
  the pomodoro's mapping (`pm_cfg_t`/`pm_ring_t`/`ring_send`, amendment J's comment gone). Files:
  `main/display/orb_ring.{c,h}`, `apps/sdk/orb.h`, `apps/README.md`, `apps/pomodoro/{main.c,
  pomodoro.{c,h}}`, `tools/host_ring_test.py`, `tools/host_pomodoro_test.py`. Host-checkable in
  full; `idf.py build` still owns the glue's compile.
- **rim-2-lane-geometry** — `ring_lanes()` / `ring_lanes_in()` in the pure core plus their host
  suite family (coverage, alignment, budget, order, no double cover, the exact
  60/19,912/9,908/944 numbers, mutants). Nothing else: no display.c, no clock, no constants
  beyond §4.2's four `#define`s.
- **rim-3-ring-refresh** — the 25 Hz clock, the floors, the alpha gate, the governor, the pending
  set in the ring's IDF glue, `display_ring_lanes*()` and `send_ring_lanes_locked()` in
  display.c/display.h, the `fx_frame()` site, the bench counters, the one log line, and the
  firmware-only list. Only an IDF build compiles this; every decision in it is host-tested first.

No new `.c` file is added anywhere in this slice — the geometry goes into `orb_ring.c`, which
`main/CMakeLists.txt` already lists (the four `display/` sources are listed by hand and a missed
one links nowhere). If a later round adds one, the same commit adds it to `main/CMakeLists.txt`.

## 11. What the rim shows (the arbitration table, restated)

| condition | the rim shows | the bus pays |
|---|---|---|
| a card copy is running (`orb_edge_arc_shown()`) | the copy arc; the ring's state is not advanced | the arc's own rows (unchanged) |
| an owned record, its effect on | the ring, blended over the mirror, at the step's `s_alpha`, in lanes | ≤ 19,912 px, 60 windows |
| flash off-phase, or alpha 0 on both words | the app, untouched — the lanes go and answer the mirror exactly | same windows, no blend |
| a steady record, fraction moved < `ORB_RING_STEP_PCT` | nothing changes | 0 |
| a steady record, fraction moved | the swept lanes only | 384-1,808 px |
| tiles parked / `s_tiles_n < 2` / no mirror | the ring, the old way | `display_mark_rows()`, and one log line |
| carousel holds the glass (`fx_frame`) | the ring, stamped into each composed slab | 0 extra windows |
| untiled PSRAM-direct | nothing (as the copy arc: no rim there) | 0 |

## Owner questions

- **Which way does a lane batch go when an app frame already covers its rows?** *Chosen: skip the
  lane — the tile stamp already carried the ring at this step's alpha. Re-sending the same pixels
  narrow would double the windows to buy nothing.*
- **Does a partial (drain) step send the swept lanes, or the whole cover anyway for simplicity?**
  *Chosen: the swept lanes, through the pure `ring_lanes_in()`; a drain that re-sent the whole
  cover would be the second-most expensive thing about this slice.*
- **A dedicated 2 KB internal DMA buffer, or reuse the flush's tile ring?** *Chosen: reuse the
  tile ring — zero new internal RAM, and the park/short-ring case already has a degradation path.*
- **The breathe floor: 1500 ms (fastest smooth breath) or 2000 ms (one breath a period, half the
  sends)?** *Chosen: 1500 ms, because it is a floor and not a default — nothing asks for less than
  2 s today, and the bench can raise one number if it dislikes the cost.*
- **What may the ring spend of its own grid?** *Chosen: a third (the 3× governor), ceiling 400 ms,
  i.e. never worse than the slice's base behaviour, and no fixed playback rate
  (`gorb-os-live-layer-engine`: slower hard blips over faster partial mess).*
- **Does `/status` grow a ring block (`lanes`, `batch_us`, `grid_ms`)?** *Chosen: no — the hub's
  Go parser is outside this slice and §6.3's log line carries the same numbers to the bench. The
  orchestrator can ask for the counters in a later slice.*


## Orchestrator amendments (2026-11-11) — these override the sections above

1. **Every step's verify ends with `make check-sandbox`** (BUILDING.md; this branch now has it).
   The worker image builds the firmware, so the IDF-only glue in `display.c` and `orb_ring.c`
   compiles and links in the sandbox, and a step must leave it building. The suites the gate
   skips are named there with their reasons. Never edit a suite outside this slice to get a
   gate green; report the failure.
2. **The pomodoro's paused breath is 2500 ms, not 4000** (the owner found the slow breathe poor;
   §64.11). rim-1 changes it in `pm_ring_for`, and the pomodoro suite's period check follows.
3. **The pomodoro sends the ring when the fraction moved ≥ 2 ‰**, matching the new
   `ORB_RING_STEP_PCT`, so the drain moves as finely as the OS draws it (rim-3).
4. Owner questions: all accepted as chosen.
