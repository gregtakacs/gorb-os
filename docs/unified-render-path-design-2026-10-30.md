# The unified commit path — design, ruled 2026-10-30, not yet implemented

Owner-directed. This closes the question the Station ablation opened
(`docs/render-cost-findings-2026-10-30.md`): the still-plate dial renders at ~7 fps
while the scheduler seeds it a 15 fps floor, and the reason is a gate, not a limit.

Companion handoff prompt: `docs/unified-render-path-agent-prompt.md` — the same
ruling in execute-as-written form, five commits, with the two owner gates named.

**Status: design ruled, zero code changed.** HEAD at writing is `1366568` (v1.1.37).
Every line anchor below was read at that commit; re-verify before editing.

## 1. The ruling

One commit path for every dial. The plate is a restore source, and every moving
thing — hand, shadow, carriage, complication, pointer, glass — is a job layer with
a marked row extent. Carrier class picks the source pointer and nothing else.

The three buckets `sched_seed_floor()` seeds today (`orb_sched.c:565-569`) are an
artifact of build order, not a design. L4 was built to fix the GIF plate re-blit
(`orb_sched.h:42-47`: 250-300 cycles/px, 85 ms/frame; dropping the plate
invalidation took render~ 100->14 ms, fps 8->23). That fix needed a mutable PSRAM
canvas, because a GIF background mutates. `orb_sched_l4_active()` (`:202-205`)
gates on `orb_bg_gif_mode()`, whose comment says why: *"without it (non-GIF themes…)
the classic all-through-LVGL path stays in charge."* The predicate is a proxy for
"does a canvas exist." It was never a ruling that a still plate should pay LVGL.

Two facts make this cheap rather than a rewrite:

1. **`gfx_commit()` already takes the source as a parameter** (`orb_gfx.c:508`):
   `void gfx_commit(int nj, const gfx_paint_ctx_t *jc, const uint16_t *canvas)`.
   The commit pass never asks where restore pixels come from.
2. **Every dial mark site already branches on the gate.** Six sites, one shape:
   `clock_view.c:509` and `:534` (hands/shadows/carriage in `apply_angles`), `:768`
   (`tourbillon_advance`), `:1631` (aux pointers), `:1202` (complications), plus the
   chord gate at `:697`. Each reads `if (orb_sched_l4_active()) gfx_mark_rows(...); else
   lv_obj_invalidate_area(...);`. **No new marking machinery is needed, and none of the
   six needs an edit.** They take the L4 branch the moment the predicate answers true.

Two further readers of the predicate exist and must be read as intended rather than
assumed to "follow it too": `request_commit()` (`orb_sched.c:213`), which §4.2 covers,
and the bench-overlay walk at `orb_sched.c:844` under `#if DISPLAY_BENCH`. The second
starts running for still dials the moment the gate widens, walking blocks and marking
their rows every tick. That is correct on a bench build, but it is new cost on precisely
the arms this design measures, so a `DISPLAY_BENCH` figure is never comparable with a
non-bench one and slice 1 must state which it quoted.

## 2. Population: which dial is in which bucket, measured

Read off the shipped reader (`host_clock3_test.py`'s own `build()` + `--dump3f`) and
the card (`GET /api/v1/sd`), not inferred from presets:

| dial | scalars | bucket today | L4 today | seed |
|---|---|---|---|---|
| **Skeleton** (factory default, D37) | `bg_mode 1, gif_frames 8, bg_fps 4, tick_rate 4, sweep 1, tourbillon_n 0` | delta bank | **yes** | 12 |
| Sector | delta bank (animated plate, no carriage) | delta bank | yes | 12 |
| Station (**active on orb-A**) | `bg_mode 0, bg_frames 0, gif_frames 0, tourbillon_n 0`, sweeping | still plate | **no** | 15 |
| Neon moon, Perpetual calendar | still plate | still plate | no | 15 |
| Orbital / kite / flying / skeleton-tourbillon presets | `tourbillon_n > 0`, plate forced to keyframe bank | keyframe bank | **no** | 4 |

The name "GIF mode" is lineage. `bake_gif.py`'s header records the origin: GIF
accumulation refused an on-device decoder, and 20 full frames (8.3 MB) refused
storage, so frames became a flash-resident delta bank. `bake_clock.py:2742` still
writes `bg_mode = 1 if gif else 0`. The input no longer has to be a GIF, and the
editor's main bake path emits it whenever `splitPolicy3` returns
`carrier === 'bank'` (`Watch Composer.dc.html:2541-2545` →
`orbtheme-baker.js:2987`).

## 3. The carriage is not special, and the rule that says it is cites a dead format

The owner's read: a carriage is an oversized circular seconds hand whose frames
animate at the plate tempo, over a geartrain that is the same animated plate a
Skeleton has. Confirmed in the shipped code, three ways:

- **It is already a job layer.** `orb_gfx.h:36-40` defines paint role 2 as
  "seconds/**carriage**". `orb_sched.c:686-692` attributes blend cost under the
  same name and states the purpose: "decides the next optimization by measurement".
- **It already marks precisely.** `tourbillon_advance()` (`clock_view.c:734+`)
  unions the outgoing and incoming swept boxes; `tourb_chords()` derives per-row ink
  chords from the alpha plane so the mark is the ink, not the bbox; and under ABI4
  each frame carries its own diff rect (`orb_clock3.c:816-826`) — "the carriage
  costs its swept arc, not the whole bbox."
- **Its frames never touched the gif table.** They ride the carriage element row's
  own sprite window (`orb_clock3.c:808-827`: `orb_bundle_spr_at(sec, e.frame0 + f)`
  into `t->tourbillon[f]`). The delta *plate* is what uses `clk_bg_rects[]`
  (`orb_sched.c:236-244`). Two different tables.

**The constraint on file is stale.** `orbtheme-baker.js:2734`:

```js
deltaBank = o.carriage ? 0 : 1;
// "A carriage row owns the gif table for the tourbillon (§5.2, one gif_io at a
//  time), so a plate riding under a carriage stays the keyframe bank it always was."
```

§5.2 is a FORMAT-2-era note, and FORMAT 3 / ABI4 dissolved the contention it
asserts. §31's contract rule 2 (`app-platform-plan.md:4605-4606`) refused to settle
it either way: *"a delta bank is still unruled, not merely unimplemented."* Nobody
revisited the carriage case after ABI4 shipped, so the fork stayed by silence.

`orb_clock3.c:513-516` carries the live refusal: *"N distinct plate frames: those
planes ride the gif table a FORMAT 3 clock section does not carry."* That sentence
is the thing this design rules closed.

**Consequence, stated precisely.** The fork at `:2734` sits in the *keyframe-bank*
branch (`sc.bg_mode = 0; sc.gif_frames = 0;` at `:2730`) and fires only when the plate
actually moves. `splitPolicy3` already supports **carriage + still plate** as a
first-class carrier: `out('sprite', …)` at `:3368` and `:3378` — "plate rides the
still". What §5.2 blocks is a *delta* plate under a carriage. So the unified path wins
for caged dials twice, at different costs:

- **Slice 1 alone** captures every `sprite` dial (carriage over a still plate): no bake
  change, no ABI bump, straight to the painter at the plate's own rate.
- **Slice 4** is for caged dials whose plate genuinely moves, to stop forcing them onto
  a keyframe bank. That is where §33.3's 227 ms / 500%-page flush and §31's "every
  advance invalidates all 466 rows" live, and where the 4→12 fps step is. It is a
  **baker-only** change (§11): the v4 delta dialect the reader loads already exists.

The keyframe-bank plate encoding retires by becoming unreachable rather than by
deletion: once a moving plate under a carriage bakes as a delta bank, nothing emits
`bg_mode 0, bg_frames > 0` any more. The reader keeps accepting it for installed themes.

## 4. The mechanism

Four edits. All of them narrow; none of them adds a render path.

### 4.1 A restore-source selector replaces the canvas-only gate

New in `orb_sched.c`, beside `orb_bg_canvas()`:

```c
/* The plane the commit pass restores marked rows from, per carrier class. A delta
 * bank restores from its own mutable canvas; a still plate restores from the baked
 * plane in place — read-only XIP, nothing to write, nothing to allocate. */
const uint16_t *orb_sched_restore_src(void)
{
    if (orb_bg_gif_mode())   return s_canvas;                    /* PSRAM canvas */
    if (clk_theme.bg_mode == 1) return NULL;                     /* canvas wanted, absent */
    if (clk_theme.bg_frames) return NULL;                        /* keyframe bank */
    return (const uint16_t *)(blob() + clk_theme.plate_off);     /* baked plate   */
}
```

The `bg_mode == 1` line is not redundant with `orb_bg_gif_mode()`. That predicate
requires `gif_frames >= 2 && <= CLK_GIF_MAX && s_canvas != NULL` (`orb_sched.c:186-189`),
so a `bg_mode 1` dial fails it three ways: canvas alloc failed, `gif_frames` out of
range, or fewer than two frames. Falling through to the XIP plate in those states would
restore a `plate_off` that a delta bake carries only as frame 0 — and §12 promises the
out-of-memory dial stays on the classic path, which the guard makes true rather than
aspirational. A `bg_mode 1` dial on the classic path is the documented degradation
(`orb_sched.c:345-350`, "background held on keyframe").

**Slice 1 must check `plate_off` against the art-bank bound before trusting it** —
`orb_clock3.c:1072` already validates `plate_off % 2 == 0` and
`plate_off + PLATE_BYTES_T(scr_w, scr_h) <= art`, so the reader's guarantee is the
guard; reuse it rather than re-deriving. A dial that failed that check never had a
plate to draw and is already refused at load.

`orb_sched_l4_active()` then answers the real question — is there a source — and
stops naming a storage format:

```c
bool orb_sched_l4_active(void)
{
    return CLK_BG_DIRECT_PUSH && orb_sched_restore_src() != NULL;
}
```

`orb_sched.c:787` passes `orb_sched_restore_src()` where it passes `s_canvas`, and
the guard two lines above it (`orb_sched_commit_layers()`, `:784`: `if (!s_canvas)
return;`, commented "no canvas, nothing to restore a marked row FROM … as it owns
them for a non-GIF dial today") tests the same selector. **That comment is the last
sentence this design deletes.** `sched_flush_commit()` (`:806`) gates on
`orb_sched_l4_active()` and needs no edit — the predicate carries it.
`gfx_commit()` does not change shape: its restore loop indexes
`canvas[y * CLK_SCR_W + xa]` and byte-swaps into mirror order (`orb_gfx.c:536-545`),
which is exactly what a flash-resident RGB565 plate row needs, and the plate is
already even-offset (`orb_clock3.c:1072` refuses `plate_off % 2`). The mirror compare
that gates DMA (`:597+`) is unchanged and source-blind.

`blob() + clk_theme.plate_off` is the pointer LVGL already samples for a still
plate (`orb_sched.c:144`, `fill_plate_dsc`). **The unified path reads the same bytes
LVGL reads today, once per marked pixel, and skips the transform for pixels that are
plate rather than ink.** XIP read volume does not grow; it shrinks.

### 4.2 The still bucket needs a commit trigger, and already has one

`request_commit()` (`orb_sched.c:211-217`) exists because L4 leaves LVGL with
nothing of its own to render, so a tick that only painted must give it a reason:
a 1 px plate nudge whose `is_last` chunk drives the flush the hook hangs on. It is
gated on `orb_sched_l4_active()` and therefore starts firing for the still bucket
with the predicate change. No new code.

### 4.3 The predicate must also require a fully claimed layer table

Marking rows instead of invalidating LVGL is only safe while something welds those rows.
`orb_sched_commit_layers()` returns early on `orb_layers_count(t) == 0` (`orb_sched.c:780`)
and again on the source guard below it. A still dial whose table build failed, refused
every row, or never ran therefore gets **no weld and no LVGL**: frozen or vanished hands,
not a degraded dial. The GIF bucket already carries this exposure on a handful of dials;
the predicate change would extend it to every still dial in the fleet, which is the wrong
direction for a fallback.

**A partial claim is as bad as an empty one, so the gate is `refused == 0`, not
`rows > 0`.** A table that claimed most of its rows still goes L4 under a rows test, and
each refused element then fails silently in the worst direction: its widget is not
invalidated, so when it moves the commit restores the plate across the rows it just
vacated and **no job layer repaints it** — a hand that vanishes inside its own box and
stays gone. `clock_layers_build()` already counts exactly that condition (`refused++` at
`:1948` and `:1955`, for a full table, a malformed row, or a failed heap allocation) and
already prints it (`layer table: … %d refused`, `:1979`). Refuse the predicate on a
non-zero count: the dial loses speed and keeps every hand.

**One owner for the state, pushed from the app. Do not read across the seam.**
`orb_sched.c` is the OS and cannot dereference a `static` that belongs to the dial, and
the broker's table is wiped while the dial is behind the glass, so it disagrees with
`s_lay_n` in precisely the window the gate governs. The dial therefore tells the
scheduler what it claimed, at build time, and the predicate reads its own copy:

```c
/* orb_sched.c — the claim the dial last reported, and the gate that consumes it. */
static int s_lay_claim_rows, s_lay_claim_refused;
void orb_sched_set_layer_claim(int rows, int refused)   /* called by clock_layers_build */
{   s_lay_claim_rows = rows;   s_lay_claim_refused = refused;   }
static bool orb_sched_layer_claimed(void)
{   return s_lay_claim_refused == 0 && s_lay_claim_rows > 0;   }

bool orb_sched_l4_active(void)   /* a source to restore FROM, and rows to weld with */
{
    return CLK_BG_DIRECT_PUSH && orb_sched_restore_src() != NULL
           && orb_sched_layer_claimed();
}
```

`clock_layers_build()` calls the setter immediately before its `layer table:` line, so
one print carries rows, refusals, and the verdict that consumes both, and the re-apply
path (`clock_view.c:2471-2479`) re-pushes through the same call. The gate must not read
`clock_layers_refresh()`'s own test (`:2009`, `s_lay_n == 0 || !orb_sched_l4_active()`) —
that is void and reads the predicate, so a gate depending on it must not depend on it back.

**The consequence to write down while doing it:** §12's promise that a failed table is
"never a dial that fails to boot" (`clock_view.c:2305-2310`) survives this change *only
because* of the gate. Without it, that comment becomes false on its first clause for
every still dial, and the sentence "a dial without a canvas still draws through the
widgets above" has to be read as a bug report. Keep the widgets live (they are the
fallback, §8 and the bounce path both depend on it); just refuse the predicate when the
dial cannot paint itself completely.

### 4.4 The predicate must print what it decides, at both build points

The Oct-30 trap list earned this rule: *"a probe that changes an observable must
print the change"* — the shipped `hand AA:` line is derived from motion words and
reported the shipped rule under the AA-off arm (`render-cost-findings §5`), and an
arm with no boot line is an arm that was never on the glass. So the decision prints:

```c
ESP_LOGI(TAG, "render path: l4=%s src=%s rows=%d refused=%d", orb_sched_l4_active() ? "yes" : "no",
         !orb_sched_l4_active() ? "lvgl" : (orb_bg_gif_mode() ? "psram" : "xip"),
         s_lay_n, refused);
```

Two placement requirements, both from the reader's own findings:

- **Print after the claim is pushed, never at scheduler start.** The gate reads the
  claim, which `clock_layers_build()` pushes at `clock_view.c:2312` — after the canvas
  decision at `:2190-2196` and after `orb_sched_start()`. A print placed at init would
  report `rows=0 l4=no` for a dial about to go L4. Co-locate it with the existing
  `layer table: %d rows … %d refused` line (`:1979`), so one line names the claim and
  the verdict that consumes it.
- **Print on re-apply, not only first boot.** The re-activation rebuild path
  (`clock_view.c:2471-2479`: `clock_layers_build` then `clock_layers_refresh`) is where
  a theme switch lands. A bench run that swaps dial mid-capture is otherwise
  indistinguishable from the previous image still running — which is exactly the
  failure `render_arms.py` exits non-zero on. Printing at the end of the build pass
  covers both cases with one call site, because re-apply calls the same build.

`render_arms.py --arm="render path: l4=yes src=xip"` then proves the arm from its own
capture, which is how every bench row in §10 gets accepted.

### 4.5 Nothing in the dial's mark sites changes; AA does

The six mark sites named in §1 keep their exact text. `apply_angles` already
computes the swept union `u` for both poses and both shifted positions; the
complication and aux passes already clip and mark; the carriage already chords.
They flip branch because the predicate flipped, not because they were edited.

`hand_aa_for()` is **not** untouched, and §8 states why it cannot be: the LVGL hand
widgets stay live under L4 as the fallback, so any repaint LVGL starts itself re-draws
them with whatever filter the v1.1.37 grant gave them.
### 4.6 Keyframe banks are the one bucket that needs a new source, and it is deferred

A keyframe plate mutates on advance, so its restore source must be the *shown*
keyframe. The clean route is to stop shipping keyframe plates at all (§3): a
carriage dial bakes a delta plate plus a carriage row, and `s_canvas` covers it.
**While that bake change is unbuilt, keyframe-bank dials keep today's path.** The two
caged classes separate cleanly, and the distinction matters for what slice 1 buys:

| caged class | scalars | `sched_seed_floor()` | L4 after slice 1 |
|---|---|---|---|
| `sprite` — carriage over a still plate | `bg_mode 0, bg_frames 0, tourbillon_n > 0` | `ORB_SCHED_FPS_CEILING` = 15 | **yes** — it is the still bucket, and slice 1 promotes it for free |
| keyframe bank — carriage over a moving plate | `bg_mode 0, bg_frames > 0` | `ORB_SCHED_PANEL_FLOOR` = 4 | no, until slice 4 |


## 5. The two-term cost model, and why it makes the editor number honest

Measured on orb-A, Station, device-time windows (`render-cost-findings §3`):

| arm | fps | render ms/s | µs/kpx |
|---|---|---|---|
| shipped | 7.0 | 870 | 1,863 |
| AA off | 7.5 | 848 | 1,724 |
| ink hidden (plate only) | **15.0** | 640 | **555** |

Plate share is 30%, ink 70% — the inverse of the intuition that a hand fills 4% of
its box, which counts pixels and not cost. The unified path pays a restore at the
plate rate on marked rows and a blend at the ink rate on ink pixels only, which is
why it wins: LVGL pays the transform rate on every pixel in the box.

```
flush_us ≈ box_kpx × RESTORE_US_KPX + ink_kpx × INK_US_KPX
```

Both inputs exist at bake time: box from sprite bbox, pivot and angle sweep plus
`SWEPT_PAD 3` (`orb_gfx.h:100`); ink coverage from the A8 plane the matte pass already
walks (`DELTA_TOL 16`, `orbtheme-baker.js:1773`; `inkBoundsOf` / `inkCentroidOf` in the
composer). So `bakeClock3` can price an estimate and `splitPolicy3` can answer against
it, with no new measurement anywhere in the toolchain.

**All three terms are already measured, and the LVGL 1,300 is the wrong one to use.**

| term | rate | source |
|---|---|---|
| LVGL box composite (paid today) | 1,863 µs/kpx | Station shipped arm, Oct-30 |
| LVGL plate-only share | 555 µs/kpx | hidden-ink arm, Oct-30 |
| LVGL ink transform share | ~1,300 µs/kpx | difference of the two above |
| **L4 painter ink (paid after)** | **185-232 µs/kpx** | `render-cost-findings §3`, internal RAM |
| **restore copy (paid after)** | **~22 µs/kpx** | `display.c` mirror memcpy rate |

The substitution is 1,300 → ~200 on ink pixels only, and 555 → ~22 on the box. That is
why the hidden-ink arm reads as an achievable target rather than a horizon. Two numbers
need re-proving on the glass: that the painter's rate holds at a carriage's orbit angle
(the per-role blend/s line at `orb_sched.c:693` already shows rotated carriage sampling
swinging 111→317 cycles/blend with angle), and that an **XIP** restore — the still
bucket reads flash, not the PSRAM mirror — lands near the memcpy rate instead of at a
cache-miss rate. Slice 1 measures both before any claim is made, and the estimator's
label reads "estimated" until it does.

## 6. The scheduler: one beat model, and the deadbeat tick joins it

`sched_fill_asks()` (`orb_sched.c:501-531`) registers four asks: the GIF bank tempo
(`:504`), the plate-bank `bg_fps` (`:509`), and the carriage `tourbillon_fps` (`:514`)
as `DISTINCT`, and a sweep as `FLUID` capped by `tick_rate` (`:520`). The gap is
narrower than "carriages and stills are unscheduled": **a caged dial is already
scheduled**, because `tourbillon_n` registers its own ask — that is why an Orbital
demotes honestly instead of overrunning. The unscheduled case is a dial with
`second_sweep == 0`, `tourbillon_n == 0`, and `bg_frames == 0` — a ticking still. It
registers nothing, so `:525-529` files it as one `FREE` ask, and
`sched_tick_period_ms()` (`:538-547`) routes it to the raw hand beat
(`orb_sched_tick_base_ms()`), which the pick does not schedule. The dial beats at its
named rate — the Oct-30 cadence fix honors a named beat for tickers too — but
hysteresis never sees it, so a dial that cannot hold its named rate just overruns
instead of demoting. Station is the counter-example on the bench right now: it is a
*sweeping* still, so it already asks `FLUID`, and its 7 fps came from the renderer and
not from the scheduler.

**Change, and the second half is not optional:** a still dial with `second_sweep == 0`
and `tick_rate >= 2` contributes a `DISTINCT` ask of `tick_rate` — **and**
`sched_tick_period_ms()`'s `anim` test (`orb_sched.c:540-544`) gains
`|| (!clk_theme.second_sweep && clk_theme.tick_rate >= 2)`. The ask alone is a **no-op**:
`:544` returns `orb_sched_tick_base_ms()` while `anim` is false, and it does so **before**
`sched_fill_asks()` is consulted at `:545`. A ticking still dial has `anim == false` by
construction — none of the four terms matches it — so the new ask would sit unread and
the dial would keep riding the unscheduled hand beat, with the commit green and nothing
changed. Adding the ask without adding the `anim` term is precisely the false pass the
`.clinerules` `CONFIG_FOO` note describes, so `host_sched_rung_test.py` must carry a
vector that **fails when the `anim` term is omitted**, not merely one that passes when
the ask is present.

**The pick does not honour the ask, and this is where slice 2 is still open.** The claim
that "4/8/12 are already rungs so they pick cleanly" is **false**, and running
`orb_sched_pick_rung()` (`orb_sched.c:426-473`) by hand shows why: the cap starts at the
*seeded floor*, not at the ask (`:431`), and the ladder is searched for the highest rung
the ask **divides into** (`:463-469`) — not the rung equal to it.

Picked rates, computed by reimplementing `pick_rung` line-for-line (`RUNG_LADDER =
{12,8,4,2,1}`, `orb_sched.c:414`; floor seeded at 15 as `sched_seed_floor()` does for a
still):

| tick_rate | single `DISTINCT` ask | `DISTINCT` + `FLUID` pair | today's tick |
|---|---|---|---|
| 1 | **12** | 4 | 1 Hz |
| 2 | **12** | 4 (panel floor) | 2 Hz |
| 3 | **12** | 4 (panel floor) | 3 Hz |
| 4 | **12** | **4** ✓ | 4 Hz |
| 5 | 5 | **5** ✓ | 5 Hz |
| 6 | **12** | **6** ✓ | 6 Hz |
| 8 | 8 ✓ | **8** ✓ | 8 Hz |
| 12 | 12 ✓ | 12 ✓ | 12 Hz |

Every rate at or below 6 goes to 12 under the single ask. `:467` skips 15 because
`15 % 4 != 0`, and 12 is the first rung that survives.

So a 4 Hz dial goes from four ticks a second to twelve, and a 2 Hz dial goes from two to
twelve — three times the work for a dial that asked for less. The angle-change gate hides most of the redraw, but each tick still runs
`clock_layers_mark()` and the commit. Worse, it breaks the evenness the cadence fix
bought: `hand_angles()` cuts the wall fraction into beats (`clock_view.c:365-367`,
`secF = sec + trunc(wall_frac * tick_rate)/tick_rate`), so a 4 Hz beat sampled on a
tick grid that is not the beat lands 250 ± 83 ms apart — visually a hand that
stutters-then-jumps. That is the exact unevenness `hand_beat_ticks()` (`:309-316`) was
written to remove in GIF mode, newly introduced into a dial that beats evenly today.

**The demotion this was supposed to buy mostly does not exist either.** `rung_clamp()`
(`:419-424`) floors at 4, so a 4 Hz ticker has nothing below its own rate to demote to.
An 8 Hz ticker at a demoted floor of 4 finds no rung under 4 that 8 divides into, falls
through to `rung_clamp(slowest)` = 8 (`:473`), and does not demote either. The honest
statement is: **a ticking still gains hysteresis visibility and a 12 → 4 demotion path
only at tick_rate ≥ 12; at 2 and 4 the pick makes it strictly worse.**

**Likely fix, and it must be pinned before any firmware edit:** a ticking still asks
`DISTINCT tick_rate` **and** `FLUID tick_rate`, the same pair a sweep already files
(`:515-520`). The `FLUID` term drags the cap to `rung_clamp(tick_rate)` at `:442-445`
before the ladder runs, so the pick lands on the highest rung ≤ the beat that the beat
divides into — 4 Hz for the 2 Hz case, 4 Hz for the 4 Hz case, 8 Hz for 8 Hz. Two things the table
makes concrete. Rates at or above the panel floor that miss the ladder entirely (5, 6, 7,
9, 10, 11) land on **their own rate** via the `rung_clamp(slowest)` fallback at `:473` —
off-ladder but exact, and the tick counter re-anchors the quantiser at `:365-367` every
wall second, so the visible error stays one beat. Only sub-floor rates (1, 2, 3) clamp up
to 4, which is faster than asked and is the panel's own floor rule, not this change's.
And the pair keeps the beat grid and the tick grid identical for every rate ≥ 4, which is
what removes the jitter.

What slice 2 then actually buys is **hysteresis visibility**, plus 12 → 4 for the
sub-4 Hz cases and the removal of the 12 Hz overrun for 1-4 Hz dials. It buys **no
demotion** for a 4 or 8 Hz dial, and its commit body must say so rather than claiming the
governor win generally.

`host_sched_rung_test.py` should pin the picked rung for tick_rate 2, 4 and 8 with the
pair filed, and the table must read **before** the firmware changes: if the first run
shows 12 for a 4 Hz dial, the `FLUID` term is missing or in the wrong branch, and that
failure is the point of the vector. **Slice 2 is not ready to hand off until this table
exists** — see §11.

`ORB_SCHED_FPS_CEILING` is **15** (`orb_sched.h:189`) and `ORB_SCHED_PANEL_FLOOR` 4
(`:190`). The `CLK_BG_MAX` comment at `orb_sched.c:49-52` is the plate-bank bound,
not an fps bound — a doc trap corrected on 2026-10-30 (`render-cost-findings §4`).

The AA work splits, and the split is forced by the fallback rather than chosen:

- **Slice 1 must deny the LVGL grant whenever the painter owns the dial** —
  `hand_aa_for()` returns false when `orb_sched_restore_src() != NULL`, and the
  `host_hand_aa_test.py` vectors and mutants move with it. This is not cosmetic: the
  widgets stay live (§4.5), and the painter does not touch a row the moment a bounce,
  an overlay close, or any other LVGL-initiated repaint redraws them. A granted still
  dial would then show bilinear hands until each one next moved — minutes for an hour
  hand, and longer on a 4 fps ticked dial. Mixing edge quality inside one dial is the
  one outcome both AA options agree is wrong. The placement works because the source is
  decided at `clock_view.c:2190-2196`, before the hands are built at `:2281`.
- **Slice 5 replaces the carrier-class test with a cost test** (does the estimate leave
  headroom for the extra taps?), restoring a grant where the arithmetic now affords it.
  Slice 5 is where option (b) lives, and it stays additive.

## 7. Editor surfaces: revise two, re-ground one, delete one exemption

The composer already models delivered fps, and the model is good. It prices
`rowPrice()` box rows (`Watch Composer.dc.html:471`, `:1232`), an `occ` occlusion
factor, a CAPS ladder (`:505`), and `_devPace()`'s panel-calibrated slopes — `K_TWIN
2.31` / `K_PLAIN 1.16`, re-fit from two glass points (`:517-521`: aviator at cov
0.00 held 15 sustained; perpetual at cov 0.32 clamped to 4 at tier 15, commit
`07d3411`). `devFps()` (`:503`) is the one readout. **This is the surface to revise,
not a new one to invent.**

1. **`_devPace()`'s comment must stop describing the classic path.** It says the
   commit "restores dirty rows from canvas then re-blits every layer over them"
   (`:509-510`). That becomes the *whole* truth rather than the L4-only truth, and
   the twin-plate tax it prices stops being an L4 special case.
2. **Delete the carriage exemption.** `:526` closes with "A caged dial is exempt:
   its carriage marks by chord, unmeasured." The unified path makes it measured;
   its chord marks give it the *cheapest* box on the glass. It gets a number.
3. **Add the estimated-fps line.** `bakeClock3` returns `{ bytes, report, scalars }`
   already (`orbtheme-baker.js:1628`); `splitPolicy3`'s answer is
   `{ carrier, keyframes, fps, decim, bbox, carriage, sentence }` (`:3333-3336`). Add
   `est_fps` to the policy answer from §5's model, and let `sentence` carry it, so the
   Motion block can show carrier class, estimate, and the rung the pick will land on.
   The author then sees *why* a rate is refused before pressing Bake — the same
   name-the-refusal-before-the-press pattern the bake already uses at `:3354-3359`.
4. **Preview tempo does not move.** `devFps()` (`:503`) drives the preview and the
   readout, and the preview keeps playing at the honest number. The Oct-30 baker lesson
   (`render-cost-findings §4`: the pane drew 15 while the board stepped 2/s) argues
   against ever bending a preview down to match a stutter. Fix the device, then re-fit
   the slopes.


## 8. Fidelity: the AA grant changes meaning, and that is a real consequence

v1.1.37 (`1366568`) grants bilinear to still-plate dials on the reasoning that a
static plate absorbs a beat's edge shimmer. That grant is applied to **LVGL image
objects** (`lv_image_set_antialias` at `clock_view.c:304, 312, 322, 326`). Under the
unified path those same hands are re-drawn by the L4 painter, which uses **LVGL's own
nearest-neighbour rotation maths** (`orb_sched.c:229`) and takes no AA setting.

**So generalizing the gate silently reverses the AA ruling the same day it ships.**
And it does it *partially*, which is worse than reversing it. The LVGL widgets are not
hidden under L4 — only the marked rows stop invalidating — so any repaint LVGL starts on
its own (the activation bounce at `clock_view.c:2470-2479`, an overlay closing, a
carousel return) re-draws the hands bilinear from the standing grant, and they stay that
way until each one next moves. On a ticked dial an hour hand moves once an hour. The
dial would therefore run two edge qualities at once, switching per layer on how recently
that layer happened to move, and the bench would compare a mixed image against a clean
one.

**This forces one thing into slice 1 regardless of which end state the owner picks: the
LVGL grant must go false for any dial the painter owns** —
`hand_aa_for() && orb_sched_restore_src() == NULL` — because an untouched grant is not a
neutral choice, it is an active defect. `hand_aa_for()` is decided at hand-creation time
(`clock_view.c:2281`), which sits after the plate-source decision (`:2190-2196`), so the
test is available where it is needed and needs no new plumbing. Two coherent end states
then remain for slice 5:

- **(a) the painter stays nearest**, and the AA grant moves to a cost predicate that
  also tests L4 activity — a dial on the painter gets nearest hands and its saved
  AA cost buys box headroom. `host_hand_aa_test.py` inverts the still-dial vector.
- **(b) the painter gains bilinear** for granted classes — ~4 taps/pixel at the
  painter's rate, affordable for the ~42% of box pixels that are ink per the Oct-22
  chord measurement (`app-platform-plan.md:5898`). Bigger change, better ink, and it
  makes the grant keep its meaning.

**Slice 1 must decide this and say which.** §10 treats (a) as the default because it
keeps the painter's cost model at two terms; (b) is a follow-up with its own bench.
The owner's ruling of 2026-10-30 was about tempo and carrier class, not about edge
sampling, so this design does not claim to reverse it — it reports that the two
cannot both hold and asks for a call at the §"Owner prompts" gate.

## 9. Risks, priced

| risk | verdict |
|---|---|
| **Memory** | **Funded.** The still bucket needs **no new allocation** — its source is `blob() + plate_off`, already resident and already read by LVGL. `orb_bg_gif_canvas_init()` (`:329-350`) still allocates its one 434,312 B canvas for the bank bucket and nothing adds a second. Free PSRAM at the Oct-30 boot: 7,744 kB (largest block 7,680 kB). |
| **XIP restore stalls** | **No new class.** The commit pass already restores from PSRAM during flush; LVGL already samples the XIP plate every frame. A concurrent flash write suspends the cache and stalls both alike — already documented (`app-platform-plan.md:392`, the reason an install runs before `display_init()`), and the install-before-display rule holds. |
| **D29 art cap** | **Not threatened by the unified path itself.** Still plates add 0 bytes. The carriage slice *replaces* keyframe planes (434,312 B each) with a delta bank plus rect rows, so it shrinks the blob. The cap is 4,128,768 B (`ORB_THEME_SLOT_CAP`, 0x3F0000, plan §0 D29); re-check per dial in that slice anyway. |
| **Bake geometry / ABI** | **No bump.** `GEOM_ABI4` is already **4** (`orbtheme-baker.js:1772`, "C4's delta dialect, one stamp up from what shipped before"), and the delta branch already stamps `abi_min/abi_max = GEOM_ABI4` (`:3276`). A carriage dial's delta plate is therefore the existing dialect the reader accepts today via `plate_delta` (`orb_clock3.c:568`). Slice 4 changes which shape the *baker* emits, not the container. It strands no installed theme, because every theme already on a card either boots as-is or is re-baked. |
| **Frame counts** | Two tables, still separate: a carriage row holds **16** frames (`bake_clock.py:2337` refuses past 16 — the "32" figure is `TOURB_MAX`'s sprite ceiling at `orbtheme-baker.js:3351`, not the carriage's own bank limit); the delta plate rides the bg table (`BG3_FRAMES_MAX 16`, `GIFS_MAX 32`). A 48 s orbit at 4 fps is 192 frames, so the plate bank samples and the carriage keeps its own 8-12. |
| **Skeleton regression** | **The one arm that must not move.** Skeleton is the factory default (D37): a regression strands every freshly-provisioned orb on a worse first dial. Gate in §10. |
| **Painter cost mis-scaled** | Handled by measuring the painter's own per-role rate in slice 1 rather than reusing LVGL's, and by labelling the estimator "estimated" until that measurement lands. |

## 10. The bench matrix

The recipe, proven the hard way on 2026-10-30 (`render-cost-findings §5`) — an arm
that cannot be proven from its own capture is not an arm:

1. `git commit`, then `. /home/greg/esp/esp-idf/export.sh && idf.py build` (there is
   **no `idf6.sh`** in this workspace), then `tools/serflash.sh` detached, polling
   `/tmp/serflash-<ORB_LABEL>.log` for `Hash of data verified` + `EXIT=0`.
2. **Put the dial on the glass before capturing**: a reflash boots to the menu, the
   menu being foreground stops the clock tick, and no `clock … fps` line is ever
   printed. `curl -sf -m8 http://192.168.2.32/api/v1/screen?to=clock`.
3. `python3 tools/capf.py 25 /tmp/cap.txt` (**secs first**, then path; the port open
   resets the board, which is what makes same-window comparison possible), then
   `python3 tools/bench/render_arms.py shipped=/tmp/cap.txt --window 8:20`. It exits
   non-zero on an arm it cannot prove. Report lines do not exist before t≈10 s.
4. A probe that changes an observable must print the change: the shipped `hand AA:`
   line is derived from motion words and will cheerfully lie under an ablation arm.

| dial | carrier class | before | after | what proves it |
|---|---|---|---|---|
| **Station** (active on orb-A) | still plate | 7.0 fps, 1,863 µs/kpx | hold 15 | boot line `render path: l4=yes src=xip`, `render ms/s` down, `clock report` fps 15 |
| **Sector** | delta bank, no carriage | 12 fps seed | **unchanged** | the no-regression gate: identical boot line (`src=psram`) and fps |
| **Skeleton** (factory default) | delta bank, comp overlay | 12 fps seed | **unchanged** | same, plus `gfx_parity_test.py` identical against its pin |
| A **`sprite`** caged dial (carriage over a still plate) | `bg_frames 0, tourbillon_n > 0` | 4-7 fps via LVGL | 12 fps target | **slice 1** — boot line `src=xip`, no ghost on the vacated arc, no bake change |
| A caged dial whose **plate moves** (keyframe bank) | `bg_mode 0, bg_frames > 0` | 4 fps, 227 ms/flush (§33.3) | 12 fps target | **slice 4** — baker-only change, blob shrinks, boot line `src=psram`, and the delta tick + `tourbillon_advance` combination runs on glass for the first time (§11) |
| A still dial with a comp (date/moon) | still plate + role 5/7 | classic | painter, marked boxes | comp still refreshes on rollover; `clock report` comp lines move |

**On the pixel-parity check.** `fbgrab --diff` against a classic-path capture is not a
same-instant comparison: the classic arm needs its own `CLK_BG_DIRECT_PUSH=0` flash, and
the dial has moved between the two captures. Pin the time before capturing either arm
(push a fixed time over the existing debug route, or capture both arms within the same
second-hand beat on a ticked dial and diff at `tick_rate 1`). If neither is available,
**downgrade the check to the ghost/flash inspection named below and say so** — a diff
against a moving dial proves nothing and must not be quoted as parity evidence.

Two failure modes to look for specifically, because the path is new for these dials:
**ghost** (a vacated hand box that never restores — a missing mark) and **flash**
(the plate repainting when it should not — a spurious mark). The chord-marked
carriage case is where a ghost is most likely and least visible.

## 11. Commit slices

Each is one coherent claim, benched, committed alone, and every one is revertable
alone. `CLK_BG_DIRECT_PUSH` stays the kill switch: each slice must leave a dial that
answers false on it rendering exactly as it did at `1366568`.

1. **`feat(gfx): unify the commit path on a restore source`** — §4.1 predicate +
   selector (both `bg_mode 1` and keyframe guards), §4.3 table gate + setter, §4.4
   print at both build points, §4.2 unchanged trigger, plus the painter's own per-role
   cost measured (§5) and the **AA denial of §8 in force** — the grant cannot be left to
   slice 5 without shipping mixed edge quality. No bake change, no scheduler change.
   Still plates go L4 — including a carriage riding one (the `sprite` carrier), which
   needs no bake change to benefit. Keyframe-bank dials stay on the classic path.
   The gate is `refused == 0 && rows > 0`, never `rows > 0` alone — a partly claimed table
   makes a refused hand vanish inside its own box, because the commit restores its rows
   and no layer repaints them. The dial pushes the claim through the new
   `orb_sched_set_layer_claim(rows, refused)`; `orb_sched.c` must not reach into a
   `clock_view.c` static. One consequence belongs in the commit body: the AA denial tests
   `orb_sched_restore_src()`, not `orb_sched_l4_active()`, so a dial the table gate refused
   loses AA as well. That is harmless for a painter-owned dial that paints none, and it is
   the conservative reading, but it is not "L4 dials only" and must not be reported so.
2. **`feat(sched): a ticking still dial is an ask, not a pass`** — §6. **NOT READY TO
   HAND OFF.** The `DISTINCT` ask, the `FLUID` cap, **and the `anim` term**, or the commit
   changes nothing — and the single-`DISTINCT` form as first drafted sends a 4 Hz dial to
   12 Hz and jitters its beat (§6 has the measured table). Pin the picked rung on the host
   for tick_rate 2/4/8 before touching firmware; do not implement from the earlier draft.
3. **`feat(bake): price the commit at bake time`** — `est_fps` from §5, `sentence`
   carries it, Motion block shows it, `_devPace()` comment corrected and the carriage
   exemption deleted (§7 points 1-3). No firmware change.
4. **`feat(bake): a carriage dial's moving plate bakes as a delta bank`** — §3, and
   **baker-only**. Drop the `o.carriage ? 0 : 1` fork at `orbtheme-baker.js:2734` in
   **both** byte-identical copies (`tools/watch-composer/` and
   `tools/orb-manager/web/composer/`), and bench the result. It needs no reader change:
   the v4 delta dialect already ships at `GEOM_ABI4 = 4` (`:1772`, stamped at `:3276`),
   the reader already admits it (`plate_delta`, `orb_clock3.c:568`), and the `:513`
   refusal the first draft told you to delete fires on `bg_frames && bg_mode == 1` while
   the delta branch writes `bg_frames = 0` (`:2987`) — so it is unreachable for this
   shape, and deleting it would only re-admit a genuinely malformed value pair. Keep it.
   **Why this is still the riskiest slice:** §2's population table shows the delta tick
   (`:1772`, stamped at `:3276`), and `orb_clock3.c:513` never sees a delta theme, so it stays a
   writer. The refusal is **not** dead code to delete: it tests `bg_frames`, which the delta
   re-admits a shape the reader cannot validate. Owner gate 2 is **withdrawn**; if a later reader
5. **`feat(clock): hand AA by cost, not by carrier class`** — §6 and §8's option (b) if
   the owner chose it, restoring a grant the §4.5/slice-1 denial removed for dials whose
   estimate affords it. Additive by construction, because slice 1 already left every
   painter-owned dial nearest.

## 12. Declined and out of scope

- **The classic LVGL dial path stays** as the `CLK_BG_DIRECT_PUSH == 0` and
  out-of-memory fallback (`orb_sched.c:195-198`, `:345-350`). The unified path
  replaces it for every dial, not its existence.
- **Sub-pixel (0.1°) hand angles** — separate lever, costs bake size rather than
  runtime, and would cut carriage box area ~4×. Not part of this.
- **X-clipping the commit band** — implemented, measured neutral, and declined for
  tempo (`render-cost-findings §2`). Revisit only if the band fills the flush
  deadline after slices 1-4.
- **Bigger glass or more scanlines** — out.

## 13. Re-verify before editing

Anchors move. Confirm each still says what §1-§7 claims, and stop if one does not:
`orb_sched.c:144/186-189/202/211-217/261/504/509/514/520/525-529/540-545/565/780/787/806/844` ·
`orb_sched.h:42-47/63/117/159-171/189/190` · `orb_gfx.c:508/531-545/597` ·
`orb_gfx.h:36-43/100/104/139` ·
`clock_view.c:269/288/509/534/697/768/1202/1631/1784/1979/2007-2011/2190-2196/2281/2305-2310/2471-2479` ·
`orb_clock3.c:513/568/808-827/848/1072` ·
`orbtheme-baker.js:1772/2730-2734/2987/3276/3324-3336/3368/3378` ·
`Watch Composer.dc.html:471/503/505/527/1232/2541-2545`.
## 14. What this design does not decide

- **Whether the v1.1.37 AA grant survives on a dial the painter owns.** §8 lays out the
  two coherent end states; slice 1 picks one and the owner confirms it at the gate. The
  design does not silently reverse a ruling made the same day.
- **Whether to delete the keyframe-bank reader.** No. Installed themes carry it, and the
  reader refusing a theme it cannot restore is worse than the reader painting one badly.
  It becomes unreachable for new bakes and stays correct for old ones.
- **Whether a moving plate can be restored without the PSRAM canvas.** No, and the
  canvas stays exactly one allocation. A mutating background needs a mutable plane;
  that is the whole reason L4 was built, and it is still true.
- **Bilinear in the painter.** Option (b) of §8, its own bench, its own commit. Not
  folded into slice 1.
- **Whether other apps (non-dial screens) move to this path.** The commit weld
  (`sched_flush_commit()`, `:806`) already composites an app's layer table over marked
  rows, so the machinery is app-agnostic. Nothing here changes app behaviour, and moving
  an app's rendering onto it is a separate ruling with its own gate.

## 15. Corrections applied after the first draft, and one gate withdrawn

A reader checked the first draft against the tree and found six defects. All six are
fixed above; recorded here so nobody re-introduces them from the earlier wording.

1. **Slice 4 was baker-only, not an ABI event.** `GEOM_ABI4` is already 4; the delta
   branch already stamps it; the reader's `plate_delta` (`orb_clock3.c:568`) already
   admits the shape. The `:513` refusal is `bg_frames && bg_mode == 1` and the delta
   branch writes `bg_frames = 0`, so it is unreachable for carriage+delta and **must be
   kept** — deleting it re-admits a malformed pair. **Owner gate 2 is therefore
   withdrawn:** nothing in this design strands an installed theme.
2. **Slice 2 was a no-op without the `anim` term** (`orb_sched.c:544` returns the raw
   hand beat before `sched_fill_asks()` is ever reached). §6 now names both halves and
   demands a mutant that catches the omission.
3. **Slice 1 and the AA vectors contradicted §4.5.** Resolved in favour of the change:
   slice 1 denies the LVGL grant on painter-owned dials, because the live widgets plus a
   self-initiated LVGL repaint otherwise produce two edge qualities in one dial.
4. **The empty-table fallback hole widened.** `orb_sched_commit_layers()` returns early
   on zero rows, so the predicate now needs the table gate (§4.3), or every still dial
   inherits a failure mode only the GIF bucket carried before.
5. **Out-of-memory fell through to the XIP plate**, contradicting §12. Fixed by the
   explicit `bg_mode == 1` guard in §4.1.
6. **Six mark sites, not four** (`:534` and the `:697` chord gate were missed), and the
   `DISPLAY_BENCH` walk at `orb_sched.c:844` is a seventh reader of the gate that starts
   running for still dials — so bench and non-bench figures are not comparable.
7. **Slice 2's rung claim was false.** `orb_sched_pick_rung()` seeds its cap from the floor
   rather than the ask, so one `DISTINCT` ask of 4 picks 12 — three times the ticks, plus
   beat jitter through `clock_view.c:365-367` that `hand_beat_ticks()` exists to prevent.
   The demotion it promised does not exist at tick_rate ≤ 8 either, because `rung_clamp()`
   floors at 4. §6 carries the measured table and the `FLUID`-pair fix; the slice is
   withdrawn from the first handoff pending a host-pinned rung table.

Tool-path and method corrections: the headless suites live under
`tools/watch-composer/_selftest/`, not `tools/watch-composer/`; the `fbgrab --diff`
parity check needs the clock pinned or must be downgraded to ghost/flash inspection; and
the `render path:` line must print at every build pass, not once at init, because a
mid-bench theme switch is otherwise indistinguishable from a refused flash.

