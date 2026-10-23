# Theme delta storage + mmap load — design

Owner ruling 2026-10-22: a reboot on theme load is accepted. Themes move to the
mmap mechanism. Two conditions come with it — the orb's face must show progress
while a theme loads, and boot must show status whenever it runs longer than a
few seconds. This document is the design for that change. It replaces the
FORMAT 3 whole-plane model, and it retires D30 for themes.

Measured reason this exists: `orbital-arrow.orbtheme` is 4,010,536 B of art and
runs the tourbillon at 4 fps. The same dial authored as a delta theme measures
**859,837 B** and runs at full tempo. The 4.7x is a storage-model cost, not the
art's.

## 1. The whole-plane tax, and why it is self-imposed

FORMAT 3 already validates multi-frame element rows frame by frame, requiring the
frames to be consecutive catalogue rows of the same box (`orb_clock3.c:446-466`).
So the format knows a row can animate. What it refuses is the delta bank: an
animated plate naming gif frames is rejected at `:399-402` ("a keyframe bank
carries whole planes and no delta rects"), multi-frame rows beyond the carriage
are refused, and the loader zeroes the rect table and fills the delay table with
one uniform figure (`:811-822`). The delta vocabulary the format's own row struct
carries — `diffx/diffy/diffw/diffh` in `orb_bundle_elem_t` — is read by nothing
in the compositor.

FORMAT 2 always had the delta bank. `bake_clock.py` folded the animation
keyframes out of `watch-engine.js`'s model — it has no hook into it — so every
delta was a diff against a flat static background. The escaping tourbillon
carriage forced `bg_mode=0`, which bakes the whole 466×466 plate per frame:
3.97 MB for `orbital-arrow` alone, against ~0.30 MB the delta model needs.
`docs/tourbillon-axis-spec.md` records the same finding.

The composer owns the whole model, so the fold can now happen inside the
pipeline that holds it. Three things unlock it:

1. a per-element delta bank (the FORMAT 2 `rects`/`gif_delays` encoding, reused
   unchanged),
2. a `parent` column, so a child inherits its carrier's transform,
3. an element z-order the bake stops discarding.

## 2. Why the escapement needs `parent`, not a bigger plate

The `esc` element is authored at `cx:233, cy:233` — the cage centre — with the
balance `orbit` at `r68` and the pallet fork at `r33`. It orbits because
`carriage` rotates and it rides inside it. `watch-engine.js` applies carriage
rotation to `bal`, `pal`, `lev` and `staff` by rotating each part's own centre
about the cage pivot; a part's own sprite never changes.

Neither current placement expresses that:

- **In `plate`** (what the live bake does): each frame must show the escapement
  at a different point of the cage circle, so the escapement is painted into the
  plate and the whole plane diffs along with it. The plate is the 3.97 MB.
- **In `carriage`**: the delta becomes the union of three parts scattered around
  a 200 px circle, and the carriage sprite grows to cover the whole cage plus
  every position its contents can reach.

As a child of `carriage`, each escapement frame's delta is only that part's own
movement: about 1,300 B/frame for the balance, 700 for the pallet fork, 550 for
the lever, against the plate delta's 15,641 B. Same ink, same geometry, roughly
3x smaller per frame.

The same `parent` field serves the moonphase's crater and moon carriage
(`watch-engine.js:1215-1226`), which is already two carriages in a plane. It is
a generic composition model, not a tourbillon special case.

## 3. Budget, per region

Measured with the real `gifCost`/`plateCost`/`regionCost` from
`orbtheme-baker.js` and the real per-region frontier from the engine. This
supersedes the `/tmp/why4fps.js` figure of 1,020,957 B: that script priced the
escapement frames as carriage deltas, but a carriage delta is a diff between
successive carriage frames, and the escapement moves inside the carriage. The
correct carrier is the escapement's own delta stream.

| region | frames | cost |
|---|---|---|
| plate static (full) | 1 | 194,950 |
| plate delta (hunting scene only) | 3 | 23,673 |
| geartrain delta | 15 | 95,499 |
| carriage delta | 30 | 313,530 |
| escapement delta (carriage-child) | 30 | 174,972 |
| hands + shadows + subdial overlays | — | 57,213 |
| | | **859,837 B total** |

That is 10.3% of the real 8 MB slot cap (`ORB_THEME_SLOT_CAP`, 8,323,072 B) and
20.5% of the stale 4 MB figure the tools still assert. Two consequences:

- It fits the MMU window. The `assets` partition is one 8 MB slot, so a full slot
  costs 127 pages of the 234-page art budget at 64 KB granularity. This theme
  needs 14.
- No region needs its own map or arena. The whole theme is one mapping, so the
  D13 `ART_RESERVE_FAILED` multi-mapping race cannot arise for a theme at all.


## 4. Wire format: geom ABI 1 → 2

Three additions, all inside the geometry region the slot already carries. The
region pointer the loader gets (`orb_clock3.c:278`) has no length, so every new
figure is bounded by a field the container already checks, never by a length the
loader invents.

### 4.1 `parent` on the element row

One new word at the end of the AUX/COMP/BANK record. `CLK_AUX_WORDS`,
`COMP_WORDS`, `BANK_WORDS` and `THEME_GEOM_ABI` all move together, and the
loader's existing row-size check does the version gate — a bake that emits ABI 2
rows into an ABI 1 loader fails on size, not on a torn paint.

```
parent: u16, 0xFFFF = none (attached to the dial centre)
        otherwise = the `tag` of the element this row rides in
```

`tag` is the join key because the row already carries it and the compositor
already resolves `host` by tag. Resolution happens at parse time into a row
index; a row naming a parent that does not exist is a refusal in the existing
`reject()` voice, not a clamp. A cycle is refused the same way: the loader walks
the chain from each row and refuses a walk longer than the table.

The transform inherits multiplicatively. A child's pivot is expressed in the
parent's box, and the child composites at `parent_angle + own_angle` about the
parent's pivot — exactly what `watch-engine.js`'s `applyAngles` already computes
for `bal`/`pal`/`lev`, so the bake and the device agree by construction rather
than by a second implementation.

Depth cap of 2 (dial → element → element). Nothing in the vocabulary needs three
levels, and a cap the loader can state is a cap the loader can check cheaply.

### 4.2 Delta frames: reuse FORMAT 2's own rect vocabulary

FORMAT 2 already stores an animated bank as rect records plus per-frame delays,
and the FORMAT 3 row struct already carries `diffx/diffy/diffw/diffh`
(`orb_bundle_elem_t`): the vocabulary is present and unread. The compositor
reads those fields nowhere (`grep diff_x main/clock/*.c` returns only the two
assignments), so this is a dead column being honoured, not a new one invented.

Per animated row:

- frame 0 is the **base plane** — full `w`×`h`, the row's rest appearance.
- frames 1..N are **delta planes**, each `diffw`×`diffh` at `diffx`/`diffy` in
  the row's own box. Frame *n*'s delta applies over the composited result of
  frame *n-1*, so a frame that moves a part 6 px costs the swept area, not the
  row's box.
- `n_frames > 1` with `diffw == 0` on any frame after the first is a refusal:
  the row claims to animate and carries no way to apply a frame.

Because the row already indexes consecutive catalogue rows, and the catalogue
check at `orb_clock3.c:425-438` already walks every row's `size`, a delta frame
needs one relaxation there: `size` for a delta row is `diffw × diffh × 3` rather
than `w × h × 3`. The contiguity and total-bytes checks are unchanged, so the
region's extent is still fully accounted for — the invariant that makes the
missing region length safe.

### 4.3 Element z-order

This is a bake-side gap, not a device one. `clock_layers_build()` already sorts
depth-first with table order breaking ties, by a stable insert sort over at most
37 entries (`clock_view.c:1775-1800`), and `clk_aux_t.order` reaches it from the
row. The device honours `drawOrder` today.

What is lost is the bake. The element record's order column is written from a
fixed value rather than the engine's `drawOrder`, so every row ties and table
order silently decides. The engine's own order is available at
`orbtheme-baker.js:1325` (`var order = M.drawOrder || []`) and is used to build
the plate fold; it just does not survive into the emitted row. Carry it through,
and add one rule the flat case never needed: a child's sort key places it
immediately after its parent, so a theme that orders an escaping escapement below
its carriage is refused at bake rather than drawn wrong. `clock_view.c:1789-1795`'s
twin rule — a shadow twin stepping its sort key down one rung — is the precedent
for exactly this kind of key adjustment.


## 5. Device changes

### 5.1 Loader (`orb_clock3.c`)

- Delete the refusals at `:348` (plate frames that "ride the gif table a FORMAT 3
  clock section does not carry") and `:400-402` (animated plate naming gif
  frames), and stop zeroing the rect table and stamping one uniform delay
  (`:811-822`): populate `rect_first`/`rect_count` from the row's own delta
  frames and `gif_delays[]` from the row's own tempo column.
- Relax the catalogue size check for delta rows (§4.2).
- Parse `parent` for every row; build a `parent` row-index array during the
  existing table walk; refuse a missing parent or a chain deeper than 2.
- Honour `order` in the emit order (§4.3), which means the row loop that fills
  `io->layers[]` walks the table in display order rather than catalogue order.
- Tempo is today one scalar for the whole theme: `tourbillon_fps`
  (`orb_clock3.c:292`, `clk_types.h:69`), consumed at `orb_sched.c:501` and
  `clock_view.c:677` where a zero defaults to 15 Hz. The per-frame delay table is
  derived uniform from it — `orb_clock3.c:811` records that "R6 retired the
  per-frame delay" and `:818-822` fills every slot with the same figure. Per-row
  animation therefore needs the delay column back, as one word per animated row
  in the ABI 2 record. Without it a theme cannot run the escapement at 8 Hz and
  the geartrain at 1 Hz, and the 15 Hz default becomes the answer again rather
  than the ceiling.

### 5.2 The compositor (`main/gfx/orb_layers.c` + `clock_layers_*`)

Since Migration 2 Commit 4 the clock has no private compositor. Its build pass
claims one row per element (`clock_layers_build`), its tick re-poses them
(`clock_layers_refresh`), and `orb_sched_commit_layers()` is the only L4 commit
path on the glass. The two changes therefore land in the layer table and the
clock's pose pass, not in a new renderer:

1. **Delta application.** A row with frames beyond the first composites frame 0
   into its scratch, then applies the next frame by blitting only its delta rect
   and restoring the rest from the background already painted. FORMAT 2's
   tourbillon path does this today in `clock_view.c` (`s_tourb_shown`,
   `tourbillon_advance`, `tourbillon_ink_mark`), including the mark-the-rotated-
   ink rule the tourbillon-axis spec and `.clinerules` record. That machinery
   generalises from "the carriage" to "any row with a delta stream"; the ink-chord
   stamping is what keeps the dirty mark tight, and it must carry over intact.
2. **Child transform.** A row with a parent composites into the parent's scratch
   at the parent's local coordinates, so the parent's rotation about its own
   pivot carries the child for free. This is what makes the escapement correct
   without the compositor knowing what a tourbillon is.

The dirty-mark rule from `.clinerules` carries over unchanged: a mover marks its
own swept box, and the flush gate drops identical rows. A delta carriage costs
about 1.4k px per tick against the plate's ~200k, which is the whole point.


### 5.3 mmap, and the erase hazard it creates

`ORB_ART_MECH_B` is one global switch (`art_bank.h:108`, currently 1) that
`art_bank_init()` obeys for every caller (`art_bank.c:343-347`). Flipping it to 0
would take app banks onto mmap too, which §9 rules out. The theme therefore needs
the choice at the call rather than at the define: `art_bank_init()`'s existing
"two mechanisms, chosen by the define" becomes chosen by the caller, keeping the
two entry points `art_bank_map()` (A) and `art_bank_fill()` (B) that already
exist separately (`art_bank.h:225` documents the A form as available "whatever
`ORB_ART_MECH_B` says"). The theme calls A. App banks keep obeying the define.
The define stays 1, so nothing else in the build changes.

Two rules come with the theme's move to A, and both are new:

**Implemented (C3).** The store runs verify, project, bill, park, unmap,
erase, write, reboot — in `orb_theme_install_bundle()`, in that order:
`art_bank_reserve_span()` bills the rounded art span before any destructive
step (a miss puts "this theme needs N MMU pages and this board has M free"
on the row and leaves the slot whole); `store_park_dial()` then drops the
tick's running gate and switches the glass to the home row from the worker
task; `art_bank_unmap()` releases the map with the compositor parked; the
copy writes; the reboot re-maps from flash. Nothing past the unmap resumes
the clock: `s_slot_torn` refuses every re-entry route, each answering
"storing, reboot to finish" rather than erasing over the copy in flight.

Boot gets simpler rather than harder: no arena malloc, no ~2 s copy, no
`ESP_ERR_NO_MEM` fallback, and the theme's pixels are current the instant the
map returns.

## 6. Progress on the face, and boot status

This is the owner's condition on accepting the reboot, so it is a requirement,
not a refinement. Two distinct slow moments need it, and they need different
machinery.

### 6.1 What is actually slow, measured

| moment | today | after mmap |
|---|---|---|
| boot map + parse | ~2 s arena copy of 4 MB | one map, ~50 ms for 14 pages |
| install copy (card → slot) | 3–6 s for ~1 MB, tens of s for a full slot | unchanged — this is a flash write |
| reboot + boot to dial | ~1.4 s bootloader + init | unchanged |

So the long-running event is the **install copy**, not the boot. The boot path
only needs status for the rare slow case (first boot, torn slot, a re-store the
boot has to finish); the install path needs real byte-level progress. The design
gives both from one renderer.

### 6.2 One boot overlay, drawn before and beneath LVGL

`display_init()` currently does panel bring-up and LVGL setup as one call
(`display.c:830-974`), and the theme resolves before it (`main.c:86`), which is
why a staged boot is dark: the panel is not yet up. D31's `fb_draw_*` primitives
do not exist yet — `grep fb_draw main/` is empty — so the overlay cannot depend
on them.

Add a small, theme-independent overlay in `main/display/`, deliberately built on
`esp_lcd_panel_draw_bitmap` rather than on LVGL:

```c
/* Bring up QSPI + panel + disp_on only. No LVGL, no draw buffers, no theme. */
esp_err_t display_panel_early(void);

/* Draw the boot/load overlay. Works with LVGL up or down; the caller parks
 * LVGL first when it is running, so the overlay owns the glass. */
void orb_boot_overlay(const char *stage, uint32_t done, uint32_t total, uint32_t secs);
```

Implementation constraints, all cheap: one row-band buffer (466 × 24 px,
internal DMA RAM, reused), RGB565 fills, and one 5×7 bitmap font compiled in so
the overlay needs no theme font and no filesystem. A ring arc for percentage,
the stage word, and `done/total` in KB. Total cost: one small buffer, no PSRAM,
and nothing on the paint path.

`orb_boot_overlay` is the only thing that can paint during a theme load, because
the theme may be the thing that failed. It draws the same frame at boot and
during install, which is why one function serves both.

### 6.3 Install: byte progress, live

The store already runs on a worker task (`bundle_job`, `orb_theme.c:1394`) and
already reports bytes and ms when it finishes (`:1040-1043`), but has no notion
of progress and no way to tell the glass anything. Add a progress callback
threaded into `copy_bundle3()`, which already loops in chunks:

1. On install start: dial shows the overlay with `stage = "loading"`,
   `total = h.art_size`.
2. Every chunk boundary: `done` advances. The overlay redraws at most 10 Hz so
   the copy is not rate-limited by the panel.
3. On success: `stage = "stored — rebooting"`, then the existing reboot. The
   overlay stays up through the reboot handoff rather than the dial flashing the
   old theme back.
4. On failure: the overlay shows the same sentence the row and the ledger get
   (`install_failed()` at `:1349` already owns that one-sentence rule), and the
   old theme resumes.

The clock must stop painting before the erase, for two independent reasons: the
erase takes the theme's pixels out from under the map (§5.3), and a painted row
would overwrite the overlay.

**Implemented (C3).** The store parks the dial task-safely, from the worker,
without touching LVGL: `store_park_dial()` sets `orb_sched_set_running(false)`
— the plain bool the tick and the commit hook already read — and hands the
screen to the home row with `input_router_request(orb_app_home())`; a hidden
screen is a screen LVGL never re-renders. It waits 120 ms (two router polls)
so the switch has landed and no tick was mid-frame, then paints the overlay;
the overlay paints last or the switch would paint the menu over it. If the
running gate came back up under the wait, the park refuses and restores the
owner's screen with everything whole. `copy_bundle3()` grew a progress
callback; `store_progress()` drives the overlay at chunk boundaries (its own
10 Hz throttle keeps the panel from rate-limiting the flash write), and the
erases lead the frames because they lead the point of no resume.

### 6.4 Boot: status whenever it runs long

Reorder `app_main()` so the panel comes up before the theme resolves:
`nvs_flash_init` → `display_panel_early()` → `orb_theme_boot()` →
`display_init()` → apps. The install-before-display rule at `main.c:73-85` was
about erasing while LVGL paints; a pre-LVGL overlay does not paint the dial, so
the ordering reason disappears while the safety it protected is kept.

Boot status rules:

- The overlay appears at once with `stage = "starting"`, and advances through
  `mapping` → `ready`. Because the map is ~50 ms, it is normally gone before
  anyone notices it.
- A **4 s boot watchdog** promotes the overlay from stage-name to
  stage-plus-elapsed-seconds, so a boot that hangs names where it hung. It is a
  display rule only: the hardware watchdog stays untouched, and the elapsed
  figure comes from `esp_timer`, not from a new timer task.
- A staged load at boot (`s_staged_id` non-empty, `:1321`) means the last store
  was torn. The dial shows `stage = "theme load failed"` with the id it was
  waiting for, then the base face. Today that condition is only on the serial
  log (`:1335-1342`), which nobody reads on a device hung on a wall.
- A refused reserve or a refused parse puts its own sentence on the overlay for
  10 s before the base face takes over, using the string `reject()` already
  built. Same rule as the install failure: the dial says what the log says.

### 6.5 Why the reboot is still the right trade

The reboot is what makes this cheap. There is no live remap, no double-buffered
slot, no lazy-flip, and no window in which two themes' pointers are both valid,
because the process that could observe that dies before the new map is created.
The overlay is what makes it tolerable: the dial is never dark and never silent
about what it is doing.

## 7. Bake pipeline: where the fold happens

The fold must run where the model is, which is the browser. `watch-engine.js`
holds the composition, the angles and the parts; `orbtheme-baker.js` already
calls it per pose (`:940`, `:1466`, `:1673-1676`). A Python re-fold of a JSON
summary can never produce correct per-element deltas, because the summary has
already discarded the structure the delta depends on — that is the failure that
made `bg_mode=0`.

### 7.1 `captureDeltaSet3` captures a record, not a PNG

Today it returns `{ png }` per frame (`orbtheme-baker.js:1573-1576`,
`:1689-1696`) and `gifCost3` charges the full plane (`:1174`), which is why every
frontier number the tool has ever printed for an animated tourbillon is the
whole-plane price. Change the captured record to carry what the encoder needs:

```
{ canvas, rect: {x,y,w,h}, parts: [{name, box, inkAlpha}] }
```

The rect is the union of the frame's changed part boxes, measured in canvas
space. The encoder diffs only inside it, so the fold cost is the model's, not a
guess from decoded pixels.

### 7.2 The encoder: model-guided bounds, pixel-exact diff

Use the model to bound, and the pixels to decide. Part boxes tell the encoder
which sub-box can differ; the actual dirty rect and the RLE come from comparing
the canvases, so a shadow, an edge blend or a glow the model underestimates
costs bytes rather than producing a wrong pixel. The encoder is the current
self-verifying one, retargeted:

1. Frame 0 encodes as the base plane.
2. For frame *n*: composite the running canvas from the decode of frame *n-1*
   (never from the capture — that would let an encoder bug hide inside itself),
   diff inside the candidate rect, and tighten the rect until the diff is exact.
3. Verify every frame decodes to within the existing tolerance, and verify the
   whole loop returns to frame 0.
4. Emit one catalogue row per frame with its `diff*` fields, and the per-frame
   delay from the row's own tempo.

### 7.3 `splitPolicy3` prices per region, and the guard moves earlier

`splitPolicy3` (`:1544`) currently decides on one aggregate `wantFps` and falls
back to whole-plane plate + frozen tourbillon whenever any part of the theme
wants more than one frame. Split the cost it optimises into the four real
regions: plate-static, plate-delta, gear-delta, carriage-delta and
escapement-delta, with the escapement priced as a carriage child (§3). It then
chooses frames per region, so a theme can run the escapement at 8 Hz, the
carriage at 4 Hz and the plate scene at 1 Hz, which is what the dials actually
want — a tourbillon at 4 fps looks wrong while a date window at 1 Hz is
invisible.

`exportTheme3`'s fps guard (`:1702-1707`) currently fires after baking and shows
"bake but do not download" plus a one-size-fits-all preset. Once the choice is
per region, the guard's real job is to price the theme before the bake, so the
region costs and the recommended frames move to the click handler's measure pass
and the guard keeps only the last-line refusal.

### 7.4 Tooling constants, and the tests that hid the bug

Four sites still assert a 4 MB slot against a device that reports 8 MB
(`orb_theme.h:284`, `orb_theme.c:97`):

| file | line | value |
|---|---|---|
| `main/art/orb_bundle.h` | 61 | `ORB_BUNDLE_ART_CAP 0x3F0000` |
| `tools/orbtheme.py` | 350 | `SLOT_CAP = 0x00400000` |
| `tools/watch-composer/orbtheme-baker.js` | 100 | `SLOT_CAP = 0x003f0000` |
| `tools/watch-composer/orbtheme-baker.js` | 1725 | `SLOT_CAP3 = 0x00400000` |

(`tools/orb-manager/web/composer/orbtheme-baker.js` is a vendor copy: same two
lines, rebuild it from source rather than hand-patching.)

The test bug is worse than the stale value, so it is the part worth keeping.
`headless-bundle3.test.js:144` asserts `B.ART_CAP === 4128768` (0x3F0000) under a
comment claiming "the caps are orb_theme.c's own numbers". It asserts a literal,
so it cannot notice the shipped number is wrong — that is why the discrepancy
survived. `:148-149` then probe the boundary at the same stale figure, so the
boundary tests inherit the error too.

`:353-354` asserts `bg_mode === 0 && gif_frames === 0` and names it "the scalars
cross-set: two extra, delta vocabularies zero". That assertion pins the exact
accident that caused the 4.7x. Replace it with the contract: the escapement is a
child of the carriage, it animates, and the theme carries a delta bank. `:144`
should assert an exported `ORB_THEME_SLOT_CAP` rather than a literal, once the
constant is exported instead of re-declared per tool.


## 8. Sequencing

Six commits, each independently shippable and each with a pass line that does not
depend on the next.

**C1 — constants and tests tell the truth.** Fix the four stale 4 MB literals
(§7.4) and export `ORB_THEME_SLOT_CAP` so the tools import it instead of
re-declaring it. Rewrite `headless-bundle3.test.js:144` and `:353-354` to assert the
contract rather than the current output, and expect them to fail: the failure is
the spec for C2–C5. Pass line: the suite is red on exactly the delta assertions,
green everywhere else, and no tool prints a cap the device does not report.

**C2 — boot overlay and reordered boot.** `display_panel_early()` +
`orb_boot_overlay()` + `app_main` reorder + the 4 s elapsed rule (§6.2, §6.4).
No format change, so this lands against today's 4 MB theme. Pass line: the
overlay paints, and it paints the stage it names.

**C3 — mmap for themes.** `ORB_ART_MECH_B` off for themes, unmap before the
erase, `orb_art_reserve()` at install (§5.3). This is the commit that pays the
PSRAM back and the one that must not regress the fps line. Pass line: the boot
log shows a mapped theme and no arena refill; the dial holds its current fps;
the store refuses a theme that would overflow the page budget, with the page
count in the sentence.

**C4 — wire format ABI 2.** `parent`, delta-frame rows, per-row delay, in
`theme_schema.py`, `orbtheme.py`, `orb_bundle.h`, `orb_clock3.c`. Refusals for a
missing parent, a cycle, a depth over 2, and a claimed delta with no rect. Pass
line: `host_clock3_test.py` carries mirrored vectors for all four, and a flat ABI
2 theme bakes and draws pixel-identically to the same theme at ABI 1.
**DONE.** The append rides geometry stamp 4: element rows widen to 25 words
(`parent` at word 23, `delay` at word 24), the catalogue to 6 words (`diffx`/`diffy`),
and the row-size gate at `orb_bundle.c` refuses a stride that disagrees with the
stamp it carries rather than guessing at the dialect (`orb_bundle_elem_stride`).
`c4_battery` in `host_clock3_test.py` mirrors it: an honest flat v4 table reads at
25/6 with the appended columns zeroed, a delta plate materializes one rect row per
extra plane at the plate origin plus each diff and honours the row's own tempo, a
comp resolves its `parent` to a row index, and the refusals fire in the projection's
own words for a plate claiming a parent, a `parent` of 0, `EF_INSTANCE` under a v3
stamp, a delta outside its box, a stride that lies about its stamp, and a tempo on a
one-frame row. The C4 vectors run under the shared harness (`--dump3f`/`--refuse`),
so the byte-level contract is the shipped reader's claim, not the host packer's.

**C5 — the fold in the browser.** `captureDeltaSet3` record, model-bounded
encoder, per-region `splitPolicy3`, guard moved to the measure pass (§7). Pass
line: the same kit's tourbillon banks a delta stream whose first frame is the
base plane and whose loop closes on frame 0; the price the tool shows is within
5% of the bytes it emits.
- **DONE (headless-verified; board checks in C6).** `orbtheme-baker.js` now
  carries the v4 dialect end to end: `ELEMENT_FIELDS` widened to 25 words
  (`parent`/`delay`) appended after the drive four exactly as `theme_schema`
  does, with `ELEMENT_WORDS_V3`, `GEOM_ABI4`, `PARENT_NONE`, `SPR_WORDS2` and the
  two stride families (`CLOCK_STRIDES_V3`/`_V4`) mirrored from the Python.
  `packElements`/`packBlob3` take the section stamp and pack 23 words + a 4-word
  catalogue at v3 (byte-identical to every file shipped before C4) and 25 words +
  a 6-word `off/size/w/h/diffx/diffy` catalogue at v4; `writeBundle3` stamps the
  container header at the max section stamp; `unpackBlob3`/`checkClockStamp` read
  and gate both families in the reader's refusal order, and the v4 reader
  materializes `rect_count`/`diffw`/`diffh` onto the row as `orb_clock3.c` does.
  `checkElement`/`checkTable` gained the parent/delay/instance rules and the
  parent graph (claim exists, not itself, ≤2 links, no cycle). `bakeClock3`
  encodes an animated plate as the delta stream: frame 0 the whole base plane,
  each later frame the union box of its changed texels over the running canvas
  (a full-panel or unchanged frame costs a keyframe, and a loop that returns to
  frame 0 ships `bg_loop=1`/`bg_frames=0`/`bg_mode=1`); it carries its own
  `delay` (row `bg_fps`, else the shipped 67 ms) and a `parent`. A plate riding
  under a carriage row stays the keyframe bank — the tourbillon owns the gif
  table (`orb_clock3.c:539` refuses both), so `deltaBank` is cleared when one is
  present. `selfVerify3` composites the baked deltas back and refuses a chain
  that drifts before the bake ships. The `headless-bundle3` delta-contract check
  (line 369, red by design through C1–C4) is now green; the whole suite is green
  (182 checks) and the shipped `orb_clock3.c` projects the new delta bake — the
  projection prints the loop frames at their keyframe offsets (§5.2), so the
  harness prints a delta plate's `io.bg_frames` under the same `bgframe` line a
  keyframe bank used. The board pass line (re-baked tourbillon on orb-B, the
  within-5% price, the loop-closes-on-frame-0 glass check) and
  `sim_tourb_mark.py` against a built `assets/clock_assets.bin` need the device
  build and are carried into C6.

**C6 — the tourbillon earns its keep.** Re-bake `orbital-arrow` at the corrected
budget, flash, measure. Pass line: art lands near 860 KB, the escapement and
carriage animate independently, the fps line rises against today's 4.7, and
`sim_tourb_mark.py` still exits 0 on the new geometry.
- **BLOCKED on the kit source, not the code.** C6 needs a re-baked tourbillon
  theme as its input, and that input is gone from this tree. `e3f73a1` deleted
  `assets/sources/orbital-arrow-3/` (its `clock_style.json` carried no tourbillon
  entry; the carriage frames lived only in the untracked composer kit); the only
  committed source kit is av-kit, and the shipped bank records `tourbillon_n = 0`
  (`tests/fixtures/avkit-pair.h:104`), so it has no carriage to price. The bake is
  the browser's (`Watch Composer.dc.html` over `watch-engine.js`): §7 rules out a
  Python re-fold, and this box has no canvas or headless browser (no `canvas`, no
  `puppeteer`), so no `.orbtheme` can be produced here to flash. The two gates C6
  does name are green on what is present: the device build of the C4/C5 reader
  completes (`BUILD_EXIT=0`, `gorb-os.bin` 0x1a0a20 B, 19% free), and the board
  orb-B is bound and idling (`fw=v1.1.0+8c1537d`, ARP proves
  `192.168.2.34` = `44:bd:8d:78:26:18`). `sim_tourb_mark.py` fails only at a missing
  `assets/clock_assets.bin` (`FileNotFoundError`, identical at HEAD~1) — an unbuilt
  artifact, so unrelated to C4/C5. To run C6: recover a tourbillon kit, bake the
  theme in the composer, flash orb-B, and read the fps line and per-region art
  sizes against §3.

C2 and C3 are the reboot-and-progress work the owner asked for. C1 is cheap and
unlocks honest numbers for the rest. C4 and C5 are the size and quality fix. C6
is the proof.

## 9. Out of scope

- **Reverse-D30 for app banks.** They stay on the arena mechanism. The D13
  multi-mapping race that ended the theme argument does not apply to them —
  each is one map and the count is small — and removing a bank at revoke stays
  possible with no MMU page cycling. The app tier keeps both mechanisms.
- **A theme bigger than the page budget.** It is refused at install with a page
  count, not squeezed. The budget is a shared resource, and a device cannot
  renegotiate it at boot.
- **Live remap without reboot.** Rejected by the owner's ruling. It buys nothing
  the overlay does not already cover.
- **Lazy-flip / page-fault demand paging.** Measured 1–4 ms per fault on a
  64 KB page, which lands on the paint path and destroys the tempo the delta
  bank exists to protect.
- **`watch-engine.js` model changes.** The composition, the parts and the angle
  math are correct as authored; the escape is that they were discarded before
  the bake. C5 stops discarding them.

## 10. Risks

- **Read-from-flash tempo.** The theme's pixels move from PSRAM to a mapped
  flash span, and the dcache is small. The M2/M3 probes
  (`art_bank_m2_probe`, `art_bank_m3_probe`, `ORB_ART_LAT_PROBE`) already exist
  to price exactly this, and C3 does not ship until one of them says the dial
  holds tempo with the theme mapped rather than copied. The delta model works in
  this favour: 14 pages of hot art instead of 64.
- **Erase under a live map.** The one genuinely new hazard mmap introduces
  (§5.3). The mitigation is ordering, not a check: unmap, then erase, and let
  the reboot re-establish the map. The bench case is a re-store of the theme
  already displayed.
- **Delta chain drift.** A single wrong delta byte corrupts every later frame,
  because each composites on its predecessor. The encoder verifies every frame
  against an independent composite and verifies the loop closes, and the loader
  refuses a chain it cannot complete. Frame 0 stays a full plane, so one reload
  resets the row.
- **Child-authoring error is now possible in a new way.** A wrong `parent` makes
  the escapement orbit the wrong point at the wrong radius, visible immediately.
  The invariant is already stated and measured for the carriage jewel
  (`tourbillon-axis-spec.md`), and the bake refuses a child whose pivot is not
  inside its parent's box.
- **ABI 2 against an ABI 1 device.** The row-size check refuses it as unloadable
  rather than drawing torn art, and the version rides in the container header the
  store already validates, so the store says no before anything is destroyed.

## 11. Handoff

`docs/theme-delta-mmap-agent-prompt.md` is the prompt for an implementing agent.
It carries the bench commands, the ground rules, the four corrections above, the
mmap erase hazard, the C1 trap, the stop conditions and the report format, so the
next agent does not re-derive them or re-introduce them.


---

## 12. §32 — the torn send of 2026-10-22, and what it took with it

orb-B ate a live repeat of the store-tear during a composer send of the
orbital-arrow dial (v3 flat bake, plate keyframe bank, 5,784,178 B art — priced
honestly by `splitPolicy3`: with a carriage present the plate stays keyframed
until §7.3 lands, and 69.5% of the slot is what that looks like). The upload
landed and filed on the card; the door queued the store and acked; the hub
released its cable claim AT THE ACK; the identity sweep re-claimed the port
during the still-running copy and pulsed DTR/RTS at a board mid-erase.
Evidence: status `rst:11` (ESP_RST_USB — the hub's own probe, not brownout:
brownout is 9), `art_ready:false`, boot log `the slot is empty — base digital
face`, the glass frozen at the copy's last chunk (47%, 2680/5652 KB) with the
NEXT boot's overlay lines painted half over it — the CO530 keeps GRAM across a
warm reset and the overlay paints only three text bands, never the panel. The
card row for the uploaded id did not survive either; a rescan listed eight rows
with no sign of `d77c92f62c25`, only the staged witness naming it.

Fixed the same day, four ways:

1. **The hub holds the claim through the copy.** `theme.go`'s new `waitForStore`
   watches the four `/api/v1/sd` witnesses (`job_busy`, the row's `stored`,
   `staged_id`, `try_id`/`try_why`) from the queue-ack to a verdict, on a
   context detached from the browser request so a closed tab cannot release a
   mid-erase board. The self-reboot excuse re-arms on each silence streak.
   Applies to the upload door and the Store button alike; both responses now
   carry `store` and `store_note` beside the device's own fields.
2. **Progress everywhere it was missing.** `?precheck=1` answers the duplicate
   question from the container header and one theme-list call — zero bytes
   dialed. `GET /api/theme/upload-status` reports the hub's phase and byte
   count while the POST blocks (the hub's upload client counts bytes as the
   transport writes them). The composer sends over XHR for the browser-side
   percent; the Manage card polls the same witness. The device's card-write
   count stays on the glass, where the boot overlay already draws it per chunk.
3. **The overwrite decision moved earlier and lost the dialog.** A duplicate —
   at precheck, or the rare late 409 behind it — arms an inline second press
   that re-sends the held bytes with `?overwrite=1`. No `window.confirm`, no
   re-bake, no second transfer of the wrong bytes.
4. **The boot overlay wipes the panel black on the first draw of every boot**,
   24-row strips from y=0 — `host_boot_overlay_test.py` pins the count and the
   coverage. No warm reset can leave last life's progress bar under this life's
   sentences again.
5. **§32.3 — the failure sentence fits the glass** (the operator's next attempt,
   same day: the boot line read `theme load failed` plus the full 32-hex id into
   a 48-byte buffer, so the token clipped mid-number, and because `band_text`
   CENTERS its line at 18 px a 47-char line started at x = −190 and the guards
   ate BOTH ends — a mangled middle of a sentence, over the dark-cyan fallback
   field, is what hung on the wall). The glass now carries the sentence and six
   hex digits (`main.c`: `"theme load failed %.6s"`, 24 chars = 432 px, centered
   honestly); the whole id rides a serial `ESP_LOGE` and the hub's card.
   `band_text` clamps an over-wide line to x = 0 — it can now only ever clip at
   the RIGHT edge. The mirror suite runs a 66-char step and checks the stamped
   pixel SET against the clamped painter (the first inked column at x = 0, every
   column the panel holds landed), and greps the caller so the whole id cannot
   creep back onto the glass. Flashed to orb-B as `v1.1.0+f011366-dirty` the same
   afternoon: the boot drew the wiped panel and the base face clean, and the
   staged witness stayed cleared.


Live proof on the bench the same afternoon: a re-store of d31-anim (2.5 MB)
through the rebuilt hub answered `store:"stored"`, held the port the whole copy
(the hub log says so), and the board rebooted onto the dial by itself with the
staged witness cleared. The release-at-ack window — the exact one that tore
orbital-arrow — no longer exists in the shipped hub. The C6 gate is unchanged:
the flash of `v1.1.0+4ef0960` (or newer) still has to land before a v4 bake
means anything, and orbital-arrow wants one press of Send to orb once it does.

