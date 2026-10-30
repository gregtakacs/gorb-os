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
2. **Every dial mark site already branches on the gate.** Four sites, one shape:
   `clock_view.c:509` (hands/shadows/carriage in `apply_angles`), `:769`
   (`tourbillon_advance`), `:1631` (aux pointers), `:1203` (complications). Each
   reads `if (orb_sched_l4_active()) gfx_mark_rows(...); else
   lv_obj_invalidate_area(...);`. **No new marking machinery is needed. Those four
   calls take the L4 branch the moment the predicate answers true for their dial.**

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
  advance invalidates all 466 rows" live, and where the 4→12 fps step is.

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
 * plane in place — read-only XIP, nothing to write, nothing to allocate. A keyframe
 * plate changes underneath the painter, so it has no source until slice 4 removes
 * the encoding; the classic path keeps those dials, unchanged. */
const uint16_t *orb_sched_restore_src(void)
{
    if (orb_bg_gif_mode())   return s_canvas;                        /* PSRAM canvas */
    if (clk_theme.bg_frames) return NULL;                             /* keyframe bank */
    return (const uint16_t *)(blob() + clk_theme.plate_off);          /* baked plate   */
}
```

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

### 4.3 The predicate must print what it decides

The Oct-30 trap list earned this rule: *"a probe that changes an observable must
print the change"* — the shipped `hand AA:` line is derived from motion words and
reported the shipped rule under the AA-off arm (`render-cost-findings §5`), and an
arm with no boot line is an arm that was never on the glass. So the predicate change
prints once at clock init:

```c
ESP_LOGI(TAG, "render path: l4=%s src=%s", orb_sched_l4_active() ? "yes" : "no",
         orb_bg_gif_mode() ? "psram" : (orb_sched_l4_active() ? "xip" : "lvgl"));
```

`render_arms.py --arm="render path: l4=yes src=xip"` then proves the arm from its own
capture, which is how every bench row in §10 gets accepted.

### 4.4 Nothing else in the dial changes

The four mark sites named in §1 keep their exact text. `apply_angles` already
computes the swept union `u` for both poses and both shifted positions; the
complication and aux passes already clip and mark; the carriage already chords.
They flip branch because the predicate flipped, not because they were edited.
`hand_aa_for()` is untouched (see §8 for why that matters).

### 4.5 Keyframe banks are the one bucket that needs a new source, and it is deferred

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

**Change:** a still dial with `second_sweep == 0` and `tick_rate >= 2` contributes a
`DISTINCT` ask of `tick_rate`. 4/8/12 are already rungs on the `{1,2,4,8,12}` ladder
(`orb_sched.h:159-171`), so they pick cleanly; the pick's existing LCM rule and its
fallback (LCM past the ladder → the set's own floor) need no edit. This is what the
owner's §40 ruling already specifies, applied to the one carrier class that was
missing from the ask set.

`ORB_SCHED_FPS_CEILING` is **15** (`orb_sched.h:189`) and `ORB_SCHED_PANEL_FLOOR` 4
(`:190`). The `CLK_BG_MAX` comment at `orb_sched.c:49-52` is the plate-bank bound,
not an fps bound — a doc trap corrected on 2026-10-30 (`render-cost-findings §4`).

`hand_aa_for()` (`clock_view.c:264-274`) gates on `bg_mode` and friends to decide AA
affordability. Under the unified path the right gate is a **cost** predicate (does
the estimate leave headroom for the extra taps?) rather than a carrier-class one.
That is slice 5; the rule keeps its current behavior until then.

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
This is not a blocker; it is the reason §6 point 4 replaces the carrier-class gate
with a cost predicate rather than leaving it as-is. Two coherent end states:

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
| **Bake geometry / ABI** | **Bumps, in slice 4 only.** Carriage-over-moving-plate dials change encoding, which is exactly what `GEOM_ABI4` guards (`orbtheme-baker.js:1772`, currently 3). Slice 4 bumps 3→4 and refuses the older reader loudly on the boot line. Slice 1 touches no encoding and strands no installed theme. |
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
| A caged dial whose **plate moves** (keyframe bank) | `bg_mode 0, bg_frames > 0` | 4 fps, 227 ms/flush (§33.3) | 12 fps target | **slice 4 only**; `geom=4` boot line; blob shrinks; a `geom=3` theme refuses loudly |
| A still dial with a comp (date/moon) | still plate + role 5/7 | classic | painter, marked boxes | comp still refreshes on rollover; `clock report` comp lines move |

Two failure modes to look for specifically, because the path is new for these dials:
**ghost** (a vacated hand box that never restores — a missing mark) and **flash**
(the plate repainting when it should not — a spurious mark). The chord-marked
carriage case is where a ghost is most likely and least visible.

## 11. Commit slices

Each is one coherent claim, benched, committed alone, and every one is revertable
alone. `CLK_BG_DIRECT_PUSH` stays the kill switch: each slice must leave a dial that
answers false on it rendering exactly as it did at `1366568`.

1. **`feat(gfx): unify the commit path on a restore source`** — §4.1 predicate +
   selector, §4.2 unchanged trigger, plus the painter's own per-role cost measured
   (§5) and the AA decision of §8 stated in the commit body. No bake change, no
   scheduler change. Still plates go L4 — including a carriage riding one (the `sprite`
   carrier), which needs no bake change to benefit. Keyframe-bank dials stay on the
   classic path.
2. **`feat(sched): a ticking still dial is an ask, not a pass`** — §6 `DISTINCT`.
   Hysteresis can now see and demote it.
3. **`feat(bake): price the commit at bake time`** — `est_fps` from §5, `sentence`
   carries it, Motion block shows it, `_devPace()` comment corrected and the carriage
   exemption deleted (§7 points 1-3). No firmware change.
4. **`feat(bake): carriage dials bake a delta plate`** — §3. Deletes the
   `o.carriage ? 0 : 1` fork and the `orb_clock3.c:513` refusal, adds the rect rows
   to the carriage case, bumps `GEOM_ABI4` 3→4, and re-checks D29 per dial. The
   largest fps win in the fleet, and the only slice that can strand an old theme, so
   it goes last and alone.
5. **`feat(clock): hand AA by cost, not by carrier class`** — §6 point 4, closing the
   §8 tension with the answer slice 1 chose.

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
`orb_sched.c:185/191/202/211/236/501/538/565/787` · `orb_sched.h:42-47/159-171/189/190`

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

· `orb_gfx.c:508/536-545/597` · `orb_gfx.h:36-43/100/104/139` ·
`clock_view.c:264-274/509/769/1203/1631/2190-2196` · `orb_clock3.c:513/808-827/1072` ·
`orbtheme-baker.js:1772/2734/2987/3324` · `Watch Composer.dc.html:471/503/505/527/1232/2541-2545`.
