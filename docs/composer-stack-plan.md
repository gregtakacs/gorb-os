# D43: the composer as a layer stack, with an honest panel preview

**Status: RULED (owner, 2026-11-01, §11). Nothing built.** This file is D42's slice
E2 (`docs/element-model-plan.md` §10) widened from "one list of rows" into the
composer's whole authoring model, plus an RGB565 panel preview, a bake-time dither,
the design carried inside the `.orbtheme`, and the retirement of the hub's
`/composer/` route.

**Precedence (owner, 2026-11-01).** For the theme editor and the design it authors,
this file **supersedes** D42: E2's row is replaced by this file's slices (§10), and
D42 §7's warnings are adopted here as R8. D42 still governs the wire, the firmware
and the reader (its §2–§6, §8, §9 and rulings 1, 2, 4, 5 and 6).

Anchors were read at `edabd47` (v1.1.44). Re-verify them before editing.

**The owner's brief (2026-11-01):**

1. Hands, complications and dials are handled the same way, and their depth order
   lives in one place. Use D42's generalization, not a parallel one.
2. The editor ships inside the `.orbapp` with a version, so apps can be built and
   delivered apart from the OS. The hub serves its library copy of the editor only
   when the installed app is the same release.
3. A watch is built from the bottom up: pick a movement, pick a dial style, add
   complications that match the dial, then move, sort and order them, then render.
   Anything that is customizable stays customizable. Think of a Photoshop document
   with layers.
4. The preview matters but is not the most important part. Keep a 1:1 preview. Add
   a preview of what the RGB565 OLED actually shows: gradients posterize on the
   glass and never in the preview. This probably needs a dither.
5. Layers are **not** freely movable. The structural constraints are enforced (§3).

## 1. What is there today

### 1.1 Three depth systems

| Where | What it orders | Anchor |
|---|---|---|
| The Layers panel | eight fixed groups: Clock insides 1, Geartrain 2, Discs C, Dial 3, Raised dials 4, Hands H, Glass 5, Case & outer shine 6. Visibility only, no reordering | `LAYERS`, `Watch Composer.dc.html:447` |
| The Complications tab | `design.comps[]` in list order ("listed bottom to top") with ↑/↓ buttons | the comps list, same file |
| The Hands tab | `design.order` among the centre hands only (`z 4 / z 3 / z 2`) | `watch-engine.js:2548`, set at `Watch Composer.dc.html:791` |

A sub-dial hand has no row in any of the three. It is implied by the sub-dial's
`scale` and carrier (`SUB_CARRIERS`, `watch-engine.js`). The bake then flattens all
of this into v6 rows through `v6Drive` (`orbtheme-baker.js:2950`). D42 §8.2 records
that "the composer's inputs are unchanged until E2".

### 1.2 The editor's delivery already does what point 2 asks

- The clock's editor region is derived from `tools/watch-composer/` by
  `tools/make_clock_region.py`, and `tools/make_factory_apps.py` puts it in the
  container. `tools/watch-composer/` stays the one source.
- `libraryEditorRegion` (`tools/orb-manager/editor.go:294`) serves the hub library's
  region only when the library container and the installed row carry the **same**
  version stamp. A missing or unparsable stamp falls back to the read from the card.

This file keeps that version gate. Two loose ends:

- The hub opens the editor with `?orb=<ip>&app=<slug>` (`web/app.js`,
  `openThemeEditor`), and the composer reads neither. It asks `/api/orb/known`
  instead.
- **The region leans on the hub's runtime, by design, but through the old route.**
  The region carries only the app's own files (`index.html`, `watch-engine.js`,
  `dial-renderer.js`, `sound-engine.js`, `editor.json`, the bridge shim).
  `tools/make_clock_region.py` points `support.js` (the dc-runtime, 69 KB) and
  `orbtheme-baker.js` (the theme writer, 236 KB) at the hub's `/composer/` route, and
  React, ReactDOM, Babel and the fonts come from `/composer/vendor/` (built by
  `cmd/orbvendor`). Sharing these is right (ruling 7): they are OS services. What is
  wrong is that they live under a route named after one app's editor, carry no
  version an app can ask for, and are built from the hub-embedded copy under
  `tools/orb-manager/web/composer/`. §9 moves them.

### 1.3 Why the glass posterizes and the preview does not

- **The bake truncates and never dithers.** `rgb565Plane`
  (`orbtheme-baker.js:409`), the frame-to-565 path (`:581`) and `q565` (`:870`) all
  use `(r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3`. `bake_clock.py`'s `rgb565_le`
  (`:104`) is the same. A smooth gradient therefore gets 32 red and blue levels and
  64 green levels, with hard steps between them. Truncating instead of rounding also
  darkens every channel by up to one step.
- **The device blends with 5-bit alpha.** `mix_16_16` (`main/gfx/orb_gfx.c:224`)
  uses `mix5 = (mix + 4) >> 3`, so there are 32 coverage levels. A blurred shadow or
  a glow edge drawn live gets at most 32 steps across its falloff, whatever its A8
  plane holds. The MULTIPLY and ADDITIVE modes (`blend_px`, `:238`) quantize the
  same way.
- **The preview composites in 8-bit canvas RGBA**, with smoothed sprite rotation, so
  it shows neither effect.

The baker already mirrors the device's NORMAL blend for folding (`mix565`,
`orbtheme-baker.js:871`, documented as `mix_16_16`). The panel preview in §5 starts
from that mirror.

## 2. The model: one stack, three bands

The composer's document is a **stack**. Everything visible on the watch is a layer in
it. Every layer that becomes a D42 row is edited through D42's five parts, and no
layer kind has its own editor.

| Band | Holds | Becomes on the wire | Movable |
|---|---|---|---|
| **Crystal** (pinned top) | glass reflections, the sub-opaque half of the case | the crystal overlay section | no |
| **Rows** (depth 0–63) | every hand (centre or sub-dial), every pointer, ring, disc, window plate, readout, still bridge or blocking dial, the carriage | AUX and BANK rows, D42 §8 | yes, within §3's rules |
| **Plate** (pinned bottom) | movement plate, geartrain (its keyframe bank), inner light, the opaque bezel, and every still layer the bake folds | the plate or its bank | no, but layers enter and leave it automatically (§3, R5) |

**Layer kinds as the panel shows them.** These are the D42 shapes with familiar names:

| Panel name | D42 shape and motion |
|---|---|
| Hand (hour, minute, seconds, GMT…) | arm, rotate or retrograde, about its pivot |
| Sub-dial | a still row or a fold, holding its scale; its hand is a child (§2.1) |
| Disc / ring (date, moon, rotating hours) | ring, driven by its source |
| Window | a still blocking row with an aperture over a ring (D42 §5's helper) |
| Slider / bar | arm with a lateral pose (D42 §3) |
| Readout | text, E4 |
| Flare / applied text / logo | still row or fold |
| Opening | a cut in the dial, not a layer: a property of the dial layer |
| Carriage | the one animated row |

### 2.1 Parenting

A layer may ride another layer's pose (D42 §5, `parent`). In the panel a child sits
indented under its host, like a Photoshop group:

```
▾ Sub-dial at 6 (small seconds)
    Seconds hand
    └ shadow
▾ Window at 3
    Date ring (1–31)
```

Moving a host moves its children in position and in depth. A child keeps its own
depth relative to the host, within §3.

### 2.2 Shadows are properties, not layers

A shadow is the AUX row directly after its hand (D42 §8, `EF_AUX_SHADOW`). In the
panel it is a line under its hand, toggled and tuned in the hand's Paint section. It
cannot be dragged on its own.

### 2.3 The dial style is inherited, not copied

The dial step defines a **style**: typeface, numeral colour, accent colour, metal,
finish, lume and weight. Every layer's matching property is **linked** to it by
default, so a complication added later already matches the dial. Changing a linked
property on a layer **unlinks** it (shown with a broken-link mark) and "re-link"
restores it. This generalizes today's per-field "Same as dial" options, which exist
only for typefaces.

## 3. The stacking rules (enforced)

A drag that would break a rule does not drop. The drop marker shows the nearest legal
slot, and a one-line reason names the rule. Rules R1–R7 are hard. The mechanical
checks in R8 stay D42 §7 warnings.

| # | Rule | Source |
|---|---|---|
| R1 | Nothing goes below the plate band or above the crystal band | D42 §5: "the plate is always below every row, the glass always above" |
| R2 | A shadow stays directly under its hand and moves with it | D42 §8, shadow twins |
| R3 | A child stays above its host's face: a sub-dial hand above its sub-dial, a readout above its background panel | D42 §5, parent |
| R4 | A ring or disc shown through a window stays below the blocking layer that carries the window, and the window stays above every ring it frames | D42 §5, "a window over a ring is three rows by depth" |
| R5 | Only a layer that never moves can fold into the plate, and it folds only when no moving layer is below it. The composer decides this automatically and shows it with a "baked in" mark. Putting a moving layer under a still one promotes the still one to a live row, and its cost appears (§6) | D42 §5, `fold` |
| R6 | At most 24 rows, depths 0–63, one animated row (the carriage) | D42 §11.1, §8 |
| R7 | The dial face folds into the plate unless something is under it. Placing a ring under the dial promotes the dial (with its apertures) to a still blocking row | R5 applied to the dial |
| R8 | Through an arbor, flyback through an arbor, coaxial order, a ring with nothing above it: **warnings** behind the Mechanical switch, never refusals | D42 §7 and ruling 3 |

R1–R7 are design-time mirrors of what the reader or the format already requires, or
of what the bake can express. Enforcing them in the panel turns a bake-time refusal
into a drag that never lands.

### 3.1 As built (P4, 2026-11-01)

- **The stack is a view of the design's own fields.** `apps/clock/editor/stack.js`
  (`OrbStack`) builds the banded stack from the face the stage draws: the crystal
  (case, glass), then the hands top first (centre hands and sub-dial pointers, one
  group, `design.order`), each sub-dial face with what is printed on it and the discs it
  frames, the dial with its flares and openings, the discs it frames, then the plate
  (geartrain, movement). Depth among sub-dials and among one host's discs is their
  order in `design.comps`. A move answers with the new value of those fields — the
  same ones the old ↑/↓ buttons wrote — so every preset's stack bakes exactly the
  preset, and hands the stage does not draw keep their slots in `design.order`.
- **The rules.** R1, R3, R4 and R7 refuse a move and name themselves; R2 holds by
  construction (a shadow is a hand's flag, never a row); R5 is each row's badge (live,
  value plate, baked in, printed on it, cut through it, crystal overlay); R6 counts the
  live rows against 24 in the panel header; R8 (coaxial order, a hand sweeping through
  an arbor stacked above it) is listed only with the design's `mechanical` field on and
  never blocks. Drag and drop walks step by step and stops at the rule that bites.
- **The panel** replaces the fixed Layers list: bands, indented children, live/baked
  badges, arrows pre-checked against the rules (a dim arrow names its rule on hover),
  drag and drop, the stage eye per group, the Mechanical switch, the R6 count. A row
  click selects the layer and opens its tab.
- **Style inheritance (§2.3).** Every select whose options include "Same as dial" /
  "Match dial" (or a hand's "Default …") shows whether the value follows that source
  (⛓) or is set here (↺ re-link, one click back). Built once, in the shared field
  builders.
- **Gates.** `headless-stack` (248 checks over all 17 presets and every rule, 7/7
  mutants), `headless-stack-ui` (21 checks through the real component). All 17 presets
  were re-baked with the current composer (dither included) and installed on orb-B
  (v1.1.45+12c1682) through the upload door, replacing the card's older bakes: each bake
  passed its own checks, the store rebooted onto it (`active` = its name, nothing staged),
  and the framebuffer drew it (ink 98-99 %). One upload (Perpetual calendar) was refused
  once by the card ("the card refused the write", HTTP 500) and stored cleanly on retry;
  it did not recur. orb-B was left on Skeleton.
- **P4b, as built (2026-11-01).** A sub-dial face can be *lifted* among the hands. The
  lowest hand stepping under a face lifts the face just above it; the top face on the
  dial stepping up lifts it over the lowest hand; a lifted face stepping down from the
  bottom of the live band settles back on the dial. `design.order` carries the live
  band, bottom first (hand ids, and `comp:<id>` for a lifted face); the engine's
  `liveSeq` reads it and `handOrder` ignores the comp entries, so a design with nothing
  lifted draws and bakes exactly as before. R3 holds across the band: a lifted face
  never rises over its own pointer. A lifted face carries what it prints and the discs it
  frames (R4), counts as one more live row (R6), and is badged "live · still row" (R5).
  - **Stage and Panel view:** `compose()` draws the hands in segments with each lifted
    face between them; `composeLayers` returns the live band as an ordered list (hand
    runs truncated, still rows dithered), which Panel view blends in order.
  - **Bake:** the plate holds lifted faces back with the hands. Each lifted face ships
    as a still AUX row (source none, no cadence or travel, dithered) rendered through
    `composeLayers` with only its static discs, cropped to its ink, at depth
    `comp:<id>` in `depthOrder`; a live window it frames rides just under it
    (`bank:<id>`). Bake renders that pass `compOnly` (the comp tiles, the pointer
    plates) never draw a lifted face, only the discs it frames.
  - **Gates:** `headless-still` (11 checks, 5/5 mutants) holds still rows to the
    baker's walk-back and to the shipped `orb_clock3.c` projection; `headless-stack`
    (263 checks, 10/10 mutants) and `headless-stack-ui` (25) cover lifting through the
    model and the real panel. On orb-B (v1.1.45), Perpetual calendar with its 6 o'clock
    moon face lifted over every hand baked (face at depth 9, its moon window at 8, the
    hands at 5–7, the other windows at 0) and the C projection accepted it; grabbed at
    :30, the seconds hand runs under the face on the glass and over it in the ordinary
    bake, with the moon window showing through the face's aperture in both.
  - **Cost:** the bake's own estimate put that dial's flush at ~122 ms against ~94 ms
    unlifted: a still row over moving hands is repainted when they pass under it. The
    stack badge names the promotion; pricing it in the composer's pace readout at the
    moment of the drop (§6) is owed to P5's Render-step budget.
  - **Not covered:** flares and openings stay part of their host (a flare printed on the
    dial cannot be lifted alone), and there are no ring rows yet, so R7's dial promotion
    waits for D42 E3's rings.

## 4. The build flow

A step bar across the top. Each step is a filtered view of the same stack, not a
separate document. Every step stays reachable after it is done, and the layer panel
is always there.

| Step | Shows | Adds to the stack |
|---|---|---|
| 0. Start | "Start from a finished watch" (today's presets as thumbnails) or "Build from scratch" | a preset fills every step |
| 1. Movement | movement cards (today's Movement tab), plate, wheels, train angle, beat | the plate band; the carriage row if the movement has one |
| 2. Dial | dial style cards, chapter ring, numerals, openwork, surface, art, wear, and the style tokens (§2.3) | the dial layer; openings as its cuts |
| 3. Complications & hands | an **Add** catalogue (hands, sub-dials, windows, discs, readouts, flares), each card rendered in the current style. Cards the current movement and dial cannot host are disabled with the reason: an opening needs a visible train, a carriage window needs a carriage | rows, placed at a legal default depth |
| 4. Arrange | the stack fills the side panel; drag to reorder (§3), drag on the dial to move, snap to hour positions | depth and placement |
| 5. Finish | light (moods first, sliders under "More"), case and glass, sound | crystal band, paint, sound slots |
| 6. Render | the budget (§6), pass/fail, **Send to orb** as the primary action; layer kit, frames, `.wckit` and Design ID under "More export options" | nothing; it bakes |

**Layout.** Stack panel left, preview centre, properties of the selected layer right.
Clicking a layer on the watch selects it in the stack, as the Complications tab does
today. Properties are grouped by D42's parts: **Shape** (style, size, colour, finish,
art), **Motion** (start and end pose, end rule), **Drive** (source, cadence, ease),
**Placement** (pivot, parent, depth, shown as read-only in the stack), **Paint**
(shadow, smoothing, dither, §5.3). Each group shows the common fields and folds the
rest under "More".

**Diagnostics drawer** (closed by default): the seam check, the frame stepper, fps
and bank size, the screen-ruler calibration, and the bake prose that the Export tab
shows inline today. Nothing in it is removed, only moved.

### 4.1 As built (P5, 2026-11-02)

- **Steps over the existing views.** The seven steps are a layer over the views every
  field builder already answers to: Start (its own view: the gallery and *Build from
  scratch*), Movement, Dial, Complications & hands (both views, as chips), Arrange (the
  stack, large, in the pane, plus the selected layer's fields), Finish (Light, Case &
  glass, Sound) and Render. The old `renderVals` is `renderVals0`, unchanged; the new
  one adds the step view and overrides only the flags it needs, so no builder was
  rewritten. A selection that lands on a view the current step does not show moves to
  the step that does; one that it does show keeps the step (selecting on the stage in
  Arrange stays in Arrange).
- **Build from scratch** loads a plain dial with hour, minute and seconds hands and no
  complications, and moves to Movement.
- **The Add catalogue** offers every complication type and a hand, each with a line on
  what it is; a window or moon that would pass the 24-row cap is refused with R6.
- **Grouped properties.** A selected complication's fields fall under Placement, Shape
  and Paint; each group shows five rows and folds the rest under *More (n)*. Hands keep
  their per-hand panels, which already group style, motion and art.
- **Light moods.** *As designed*, *Warm lamp*, *Daylight*, *Studio*, *Night* set the nine
  light values at once; the sliders sit under *More*.
- **Render** leads with a badge (fits / runs slower / too many live layers, with the
  pace model's fps and the live rows), the theme name, *Send to orb* (in the hub) and the
  `.orbtheme` download; the layer kit, frames, design file, Design ID and the bake prose
  sit under *More export options*.
- **Diagnostics drawer**, closed by default: the bank line, the frame stepper and
  slow-motion, the true-size ruler and its calibration, the seam check.
- **Gate.** `headless-flow` (30 checks) walks Start → Render from scratch and from a
  preset through the real component, and proves nothing was removed: every field every
  old tab offered is reachable from a step with the folds open, every complication
  type's fields included, and every binding the pre-P5 template used is still bound (two
  renamed on purpose: `tabs` → `stepOpts`, `isDial` → `isStart`). Checked in Chrome on
  Start, Complications & hands, Arrange (drawer open), Finish and Render.
- **Follow-ups (2026-11-02), both closed:**
  - *Lifted faces are priced.* The device-pace model adds one plain crossed-plate bill
    (K_PLAIN) on the coverage of each lifted face the seconds sweep crosses: the device
    paints the still row again on top on exactly those beats. Estimated, not fitted to a
    panel measurement yet. The Render badge, the pace explanation ("a lifted face over the
    sweep repainting on top on N % of them") and the lifted row's note ("repainted on ~N %
    of sweep ticks") all read it. On a centre-seconds dial with one 60 px sub-dial the
    estimate drops 12 → 10 fps when the face is lifted.
  - *Hand panels read like complications.* Each hand reads Shape (style, length, width),
    Paint (finish, shading, lume), Motion, and the artwork upload folds behind its own
    toggle (open when the hand already carries art).
- **The editor door after a hub update (2026-11-02).** An orb whose app is older than the
  hub's shelf used to meet a bare "editor and hub do not match". The refusal now names
  both versions and offers *Update <app> on this orb*, which asks the hub page to run the
  Apps face's own forced install and reopen the editor; and an install now drops the
  hub's cached editor region for that orb, so the reopened editor is the new one.

### 4.2 The studio layout (P5b, owner 2026-11-02)

The P5 composer lived in a 70vh frame inside the Themes card, inside a page capped at
900 px: about 800×630 px for a layout whose fixed columns needed 900, so it scrolled
both ways, carried two title bars (the hub's and its own), and spoke a warm gold palette
beside the hub's blue-grey. The owner's ruling on the mockup:

1. **The editor fills the hub window.** The hub draws no bar over it; the editor's own
   top bar is the only one (breadcrumb to the orb and app, the theme's name, the steps,
   the fit badge, Send to orb) and its back arrow closes it (`theme.close`).
2. **One palette: the hub's.** Selection and focus are the hub's blue. The tokens ship
   as the OS runtime's `ui@1` stylesheet, so any app's editor matches the hub.
3. **Three panes, no page scroll.** Layers (left, every step: the stack panel, where
   arranging happens), the stage (centre: Fit by default, 1:1, True size and 2× are
   zoom stops that scroll inside the stage and never step out of it), the inspector
   (right: the selection's properties, or the step's). Only the layer list, the
   inspector and a zoomed stage scroll.
4. **Six steps.** Start · Movement · Dial · Parts · Finish · Render. Arrange is the
   layers panel. Start is a gallery sheet over the stage.
5. **Text diet.** Explanations move to tooltips; the persistent UI carries labels.
6. **The hub page is fluid.** It resizes to its window instead of a fixed 900 px column.
7. **The floor is 1280×720.**

**As built (2026-11-02).** The studio is the composer page's template rewritten over the
same component: every binding the P5 page used is still bound or retired with its
replacement named (`headless-flow`: `compRows` → the layers panel, `selTitle` → the
inspector's title, the stage box → the pane), and the P5 checklist still finds every
field every old tab offered. New: `ui@1` in the runtime catalog and the region's
`editor.json` (its tokens held equal to `app.css` by `TestUISheetTokensAreTheHubsOwn`);
Fit as the default view, measured off the stage pane, with 1:1, True size and Magnify as
stops that scroll inside it (opening centred); the inspector names the selection, shows
a selected hand's panel alone and first, and carries Remove; the layers panel carries
the add list; a field's long note is two lines with the rest in its tooltip. The hub's
pane is a fixed sheet over the page with no bar (`#mn-theme-editor`); its status note
floats above the editor's toolbar and clears itself after eight seconds; the refusal
page wears the hub's colours and has its own Back. `.view` is fluid to 1600 px and
centres. The clock app keeps v1.1.45: `version.txt` is also the firmware's stamp, and
this branch has rebuilt the unreleased v1.1.45 region at every slice. Screenshots at
1280×720 and 1920×1080 were taken in Chrome with the page framed full-window, standalone
(`file://`); the hub-served frame awaits the owner's test.

### 4.3 Hands are complications (P5c, owner 2026-11-02)

The owner's test of P5b, ruled 2026-11-02:

1. **The plate rows open the Movement tab.** Picking *Geartrain* in Layers opens
   Movement at its top. *Clock insides* opens it at *Backdrop*, which is now the tab's
   last section.
2. **The plate's background art moves to Movement,** under Backdrop. The Dial tab keeps
   only the dial-surface art.
3. **A hand is a complication.** Brief point 1 ("hands, complications and dials are
   handled the same way") was met for depth only. The design still had three fixed
   centre hands, one optional "additional" hand, and one implied pointer per sub-dial.
   That was wrong:
   - **Every hand is arbitrary and every dial is arbitrary.** Ten hands on the main
     dial is a legal design, subject only to R6's 24 live rows.
   - **Add a part** (left) opens one catalogue on the right, of every complication type;
     a hand is one entry in it. Once placed, the author picks what it shows and its style.
   - **Picking a hand in Layers** shows that hand's properties alone, never every hand's.
   - **A complication nested on a sub-dial** is nesting for depth (R3) and placement;
     it says nothing about what the part is.

**The model (P5c).** `design.comps` gains `type: 'hand'`:
`{ id, host: 'dial' | <sub-dial id>, shows, ...look }`.
- `shows` is one of hour, minute, second, 24-hour, GMT, day of week, date or month. A
  GMT hand also carries its offset.
- The look is the per-hand fields `design.hands[<id>]` used to hold: style, finish,
  shading, lume, length, width, seconds style, artwork and motion.
- `design.centre`, `design.extra`, `design.hands` and `design.gmtOffset` are removed.
- A sub-dial's `scale` is only its print, and no longer implies a pointer. Adding a
  sub-dial adds one hand on it that reads its scale, and that hand can be removed or
  changed like any other.
- The outer ring (`perim`) becomes a Dial property in its own right, instead of being
  derived from the one additional hand.
- `design.order` still carries the live band, and a hand's entry is its comp id.
- No backward compatibility is needed (D43). The presets are rewritten to the new
  shape, and each must still render and bake pixel-identical: the regression suite is
  the gate.

**The wire is unchanged.** The firmware drives every pointer from a row that names its
source, pivot, motion and depth (D42). Only the composer's hand-off changes: the first
hour, minute and seconds hand on the dial's centre keep the baker's hand slots, and
every other hand ships as a pointer row.

**As built (P5c, 2026-11-02).**

*1 and 2: the plate rows and the backdrop.*
- The Geartrain and Clock insides rows carry a `move` selection, and the inspector names
  them.
- The plate's art block moved to Movement under Backdrop, the tab's last section. The Dial
  tab keeps the surface art.

*3: the model and the engine.*
- `buildHands` walks the hand comps. A hand is sized off its host (the dial's reach, or the
  sub-dial's radius) and turned by `shows`. A GMT hand carries its own `offset`, and a
  hand reading the printed ring takes the ring's `offTurn`.
- Hands are listed bottom to top in `design.comps` order, and the sub-dial's tilt and
  print direction still apply.
- The 17 presets were rewritten to the new shape. A geometry snapshot of all 22 designs
  matches the pre-change engine: the presets plus five legacy edge cases (24-hour with a
  turned ring, GMT +3, day, date, and five sub-dial scales with tilt, ccw print and
  motion). That is 76 hands, compared on position, angle at three instants, sprite box,
  pivot, cap and stacking height. The one difference: calendar hands now carry `len`.

*3: the editor.*
- Parts is one view. With nothing selected it shows the catalogue, whose first entry is
  Hand.
- A selected hand shows its own panel: Shows, On (the dial's centre or any sub-dial), the
  GMT offset, then Shape, Paint, Motion and Artwork. Remove takes it off like any part.
- The old on/off switch is gone, and canon drops a stray `on`.
- The Add a part button in Layers opens the catalogue on the right.
- Hands added on the dial fill hour, minute and seconds first, then hour. A new sub-dial
  brings one hand reading its scale. Changing a sub-dial's scale carries the hands that
  read the old scale. Removing a sub-dial removes its hands.
- The style every hand starts from, and the shared settings (tick, cast shadows, shading,
  lume), sit on the Dial step under Hand style.
- The outer ring is a Dial choice (None, 24 hours, Date, Day of week). Its note names
  the hands that read it.

*3: the bake.*
- `handSeats3` gives the first non-sliding hour, minute and seconds hand on the dial the
  baker's slots 0–2.
- `pointerRows3` makes every other hand a pointer row from its own source: `hour12`,
  `minute`, `second`, `hour24` (24-hour and GMT, whose offset rides the plate lifted at
  00:00), `weekday`, `date` (1..32, so a day lands where the preview draws it) and `month`.
  The device poses each at zero + span·(units − vmin)/(vmax − vmin).
- A sub-dial driven as plates keeps carrying, in its tiles, the hand that reads its own
  scale.
- baker@1: `compPlan3` skips hand comps, `pointerDefault3` no longer counts a sub-dial's
  own hand as "hosting another indication", and the kit reader seats only the first
  centre hand of each kind.
- The calendar and extra hands the old model never shipped (the kit marked them
  `deviceReady: false`) now go to the device as pointer rows.

*Owed.*
- A bench bake on Gold of a 24-hour, a date and a second seconds hand: no pointer row with
  a `weekday`, `date`, `month` or `hour24` source has been posed on glass yet.
- The pace model prices only the first centre seconds sweep.
- The month hand creeps through the month in the preview. The device moves it on its
  source's units.

*Tests.*
- The suites now speak the new model. The stub's `legacy3` converts old-shape fixtures
  (dial hands keep `centre-<shows>`, a sub-dial pointer becomes `<sub>-hand`), and
  `hands3` and `hand3` build and read hand comps.
- The old eye checks became Remove checks.
- The flow suite adds: a hand from the catalogue, its panel alone, Shows changing the
  hand, ten hands on the dial counted against R6, a sub-dial arriving with its hand, and
  Remove taking a sub-dial's hand with it.
- Results: flow 58, ui 279, component 59, engine 186, stack 263 (10/10 mutants), stack-ui
  33, motion 41, pace 14, regression 106, and the runtime suites green. The clock region
  suite has 0 failures, `go test` is green, and the clock container was rebuilt (807,106 B).

## 5. Preview: 1:1, and the panel

### 5.1 Views

Two independent switches over the one preview canvas:

- **Size:** True size 1:1 (kept, with its ruler calibration moved to the drawer),
  Full 466 px, Magnify.
- **Render:** *Design* (today's 8-bit canvas) or *Panel* (§5.2). A **Compare** wipe
  splits the dial between the two along a draggable line, so banding can be spotted
  without toggling back and forth.

### 5.2 The panel simulation

Panel view draws what the device would draw from the bundle the bake produces:

1. the plate (or the current keyframe), quantized exactly as the bake stores it,
   dither included;
2. each live row in depth order, sampled as the device samples it (nearest neighbour
   unless the row is `smooth`), blended with `mix_16_16` and its MULTIPLY and
   ADDITIVE modes at 5-bit coverage;
3. the crystal overlay, blended the same way.

The maths exists in two places today: `mix565`/`q565`/`unq565` in the baker, and
`blend_px` in `orb_gfx.c`. P1 adds the two missing blend modes to the JS mirror and
pins the mirror against the C source with vectors, the same way the host suites pin
the reader today.

Whether a complete JS painter for the live rows already exists is **not verified**.
D42 §8.2's pose check compares poses, not pixels. If no painter exists, P1 writes a
minimal one: per-row rotate and blend, with no scheduler and no dirty rects.

**The gate is the glass.** `GET /api/v1/fb` and `tools/fbgrab.py` already read the
framebuffer. Panel view of a still frame must equal an `fbgrab` of the same theme at
the same time, pixel for pixel on the plate, and within the hand antialiasing
tolerance `bench_preview_parity.py` already measures on the rows. A simulation that
has not been checked against the glass is not a preview.

### 5.3 Dither at bake time

**Where it lives (as built, P2).** In the baker, `rgb565Plane` and `alphaPlane` take an
optional panel origin; with one they dither, without one they truncate exactly as
before. `bank3` carries the origin (a delta crop adds its own offset), and
`bakeClock3` passes it for the still, screen-locked rows. The Python twin is
`tools/orbdither.py`, not `bake_clock.py`'s `rgb565_le`: that function is the FORMAT 2
packer the parity vectors pin byte for byte, and it stays truncating.
`_selftest/headless-dither.test.js` holds JS to Python on random planes at odd
origins. (Corrected 2026-11-01; the first draft named `rgb565_le` as the twin.)

- **Dither between the device's own levels.** (Corrected 2026-11-01, P0: the first
  draft's `floor(v·(L−1)/255 + t)` does not keep exact colours. Level 1 of a 5-bit
  channel decodes to 8, but 8·31/255 = 0.97, so a flat 8 would speckle about 3 % of
  its pixels.) Per channel, two 256-entry tables built from the device's decode
  (`(l << 3) | (l >> 2)` for 5 bits, `(l << 2) | (l >> 4)` for 6): `base[v]`, the
  highest level whose decode is ≤ `v`, and `frac[v]`, the position of `v` between
  that decode and the next, scaled to 0–255. The level is
  `base[v] + (frac[v] > T(x, y))`, with `T` the blue-noise threshold (0–255). Integer
  only, so JS, Python and C agree bit for bit. A value whose decode is exact has
  `frac` 0 and keeps its level at every pixel: flat 565-exact colours, text and
  UI-style art come through untouched, and only values between levels are spread.
  The expected decode equals `v`, so the dither also removes truncation's darkening.
- **Ordered, locked to the screen.** The threshold is a function of the absolute
  panel pixel (x, y) only. **No error diffusion** (Floyd–Steinberg and similar). With
  error diffusion, one changed input pixel changes the pixels after it, so a moving
  plate's frame-to-frame delta grows and the delta row band and keyframe bank cost
  rise. With a position-locked pattern, an unchanged input pixel produces an
  unchanged output pixel in every frame, so frame deltas only include pixels whose
  input changed.
- **Pattern: blue noise from the start** (ruling 2). One 64×64 u8 threshold table,
  generated once (void-and-cluster) by a committed script and stored as constant
  bytes in the JS baker and in Python. A test pins both copies to the same hash. The
  table tiles across the panel. Whether the three channels share one threshold (no
  colour noise) or use offset reads of the table (less visible pattern) is decided
  in P2 on the glass, not on a monitor.
- **Where it applies.** Per layer, as a Paint setting `dither: auto | on | off`:
  - `auto` is **on** for the plate, its keyframe bank and everything folded into it,
    where the gradients are (glow, vignette, enamel, light);
  - `auto` is **off** for arm rows that turn. A pattern baked into a rotating sprite
    turns with it, and with nearest-neighbour sampling it crawls. Hands are mostly
    flat colour anyway;
  - **ring and disc rows may be set `on`** (ruling 3): a large, slow disc (a moon, a
    rotating-hours ring, a date ring) carries real gradients, and at a jump or a
    slow sweep the crawl may not show. The composer offers it per layer, and Panel
    view shows the result.
- **Not addressed here:** the device's 5-bit coverage on live blends (§1.3). Fixing
  it means work in `blend_px`, the per-pixel hot path. Panel view will show it
  honestly, and P6 measures whether it matters before anything is proposed.

**Every baked byte changes** once rounding replaces truncation, so every preset and
bench theme is rebaked. D42 ruling 6 already requires a v6 rebuild of every theme, so
this costs one extra rebake, not a migration.

### 5.4 P0's record: the crystal overlay bands more than the plate (2026-11-01, host)

**Method.** Skeleton, Luna and Neon moon baked through the composer's real export
(`_selftest/bank3-drive.html?plate=1`, which also prints the 8-bit plate
`bakeClock3` received). For each, keyframe 0 of the stored plate and the crystal
overlay row were read back with `orbbundle.read_bundle` and composited four ways
against a float ideal (the 8-bit plate under the overlay at float alpha). The device
blend is a per-field model of `mix_16_16`, checked equal to the C on 200,000 random
inputs. Error is luma, low-passed at σ 1.5 px; **band** is that minus a σ 12 px blur,
so slow offsets drop out and only contour-sized error remains. "Contour" is the share
of the dial disc whose band error exceeds 1/255.

**Facts.** Every stored plate is byte-identical to the truncated 8-bit plate. All
three overlays cover 100 % of the disc at partial alpha.

| Glass variant (range over the three presets) | Band rms | Contour | Mean offset (luma) |
|---|---|---|---|
| Plate truncation alone, overlay at float alpha | 0.30–0.51 | 1.3–4.8 % | −0.4 to −1.2 |
| **What the glass gets today** | **0.78–0.95** | **19–29 %** | **−3.0 to −3.8** |
| Plate dithered (§5.3 as first written) | 0.70–0.78 | 15–18 % | −2.6 |
| Plate and overlay alpha dithered (bake only) | 0.44–0.47 | 2.6–4.5 % | −2.6 |
| Same, plus rounding in `mix_16_16` (firmware) | 0.38–0.40 | 1.1–1.3 % | +0.1 to +0.2 |

**What it means.**

- The overlay's live blend dominates. `mix_16_16` quantizes coverage to 32 steps
  (`(a + 4) >> 3`) and truncates each field (`>> 5` floors), so the soft reflections
  band, and the glass sits about 3 luma below the preview everywhere the overlay
  reaches.
- Dithering only the plate fixes a minority of what is seen. **P2 therefore dithers
  every still, screen-locked plane, colour and alpha: the plate, its bank, folded
  layers and the crystal overlay.** The overlay's alpha is dithered onto the
  device's own 32-step lattice (stored as `m5 · 8`, so `(a + 4) >> 3` returns the
  chosen step), using the table offset by (32, 32) so it does not correlate with
  the plate's dither.
- The darkening is the blend's truncation, not the bake's, and only the firmware can
  remove it. Adding `0x2008010` to the packed product before `>> 5` rounds every
  field (checked equal to a per-field round on 200,000 inputs, no field overflows).
  It is one add per blended pixel, and the baker's `mix565` fold mirror moves with
  it. **Ruled 2026-11-01 (ruling 9) and built:** `orb_gfx.c`'s `mix_16_16` rounds;
  LVGL's own RGB565 image blends (the first paint and the wedges it still draws)
  round too, through the custom-ASM hooks `main/gfx/orb_lv_blend.c` implements and
  `main/lvhook/orb_lv_blend.h` declares (`CONFIG_LV_DRAW_SW_ASM_CUSTOM`), so the two
  paint paths still agree texel for texel without patching the managed LVGL
  component. The mirrors moved with it: the baker's `mix1616` and `mix565`, and
  `bake_clock.py`'s `_mix565`. `headless-panel` holds the JS to the lifted C, holds
  `orb_lv_blend.c`'s mix token-identical to `orb_gfx.c`'s, and catches a blend that
  truncates again; `gfx_parity_test.py` declares the one term as its intended delta.

**The glass (orb-B, v1.1.44, 2026-11-01).** Luna was baked twice under test names
("P0 Luna trunc" with `dither: false`, "P2 Luna dither" with the P2 bake), each put on
orb-B through the upload door, and the framebuffer read twice per theme with
`tools/fbgrab.py`. The device composite was rebuilt from each bundle (stored plate,
overlay row, the `mix_16_16` model).

- **The model is the device.** Outside the plate's animated rects, the glass equals
  the model on 95.3–98.1 % of the disc, and every disagreement lies in 3 or 4 compact
  blobs: the hands, which the model does not draw.
- **Before and after**, the §5.4 metric on the glass (hand blobs filled from the model,
  animated rects excluded, 55 % of the disc):

  | Luna on orb-B | Band rms | Contour | Mean offset (luma) |
  |---|---|---|---|
  | Truncated bake | 0.73 | 17.9 % | −3.5 |
  | Dithered bake (P2) | 0.52 | 6.9 % | −2.6 |

  The remaining −2.6 is the blend's truncation, §12 Q1.

## 6. Cost stays visible

The stack shows, per layer: live or baked in (R5), and its share of the per-flush
estimate (`estimate3`, `docs/unified-render-path-design-2026-10-30.md` §5, which D42
§9 extends to every row). The Render step shows the total against the budget with a
pass/fail badge. A layer whose promotion to a live row (R5, R7) pushes the dial over
budget says so on the layer at the moment it is dropped.

## 7. Also in scope

- **Autosave** of the working design to browser storage (guarded, as the art store
  already is) and a **My designs** list with named entries and JSON import/export.
- **Undo/redo** (Ctrl+Z / Ctrl+Shift+Z). The design is already one serializable
  object, so the stack of snapshots is cheap.
- **Confirmation** for "Reset everything".
- **Read `?orb=` and `?app=`** from the hub instead of asking `/api/orb/known`.
- **Plain language.** The prose stays, moved to the drawer or to hover help. The main
  surface states what a control does in one line.

## 8. The design travels in the `.orbtheme` (ruling 5)

Today a `.orbtheme` carries the baked dial and nothing the composer can reopen. The
design lives only in the author's browser, a `.wckit` or a Design ID. A theme on a
shelf or a card therefore cannot be edited again.

**The rule.** Every `.orbtheme` the composer writes carries its **design document**:
the stack (§2) as JSON, the composer's version and a design-schema stamp, and the
source assets the design references (uploaded art, sprites, sounds), which is what a
`.wckit` holds today. Opening that theme in the editor restores the document, not a
reconstruction from pixels.

**Where it rides.** Not in the installed slot. The slot's caps are tight (geometry
under `GEOM_CAP` 60 KB, banks under the slot cap, `tools/orbbundle.py`), and the dial
never needs the design to draw. The design is a **trailing region** after the banks:
stored in the file on the card and on the hub's shelf, outside the install sums,
never written into the slot. It has its own length and CRC, so a damaged design
region is refused **as a design** ("this theme cannot be reopened") while the dial
still installs and draws.

**Owed before building (P7 opens with these; none is verified yet):**

- what the device reader, the hub's `themecontainer.go` and `orbbundle.py` do today
  with bytes after the last bank: refuse, ignore or hash them;
- what the header CRC (word 28) covers, and whether a trailing region changes it;
- whether the theme's identity (`sha256(name)[:12]`) or any duplicate check ("the card
  holds a different …") reads whole-file bytes and would treat the same dial with an
  edited design as a different theme;
- the upload cost: the region crosses Wi-Fi with every install. The Render step
  shows its size, and the source assets are included by default with an option to
  leave them out ("not reopenable with the original art").

**Opening a theme for editing.** The Themes pane offers **Edit** on any shelf or card
theme whose file carries a design region. The hub opens the app's editor (by the
version gate in §1.2) with `?theme=<id>`. The editor fetches the design region
through the hub, from the shelf copy when there is one and otherwise from the card.

**Versions.** The design stamp is the composer's to read. An editor refuses a design
stamped newer than it knows, and says which version is needed. A design stamped
older is either migrated by the composer or refused, decided with each stamp change
and recorded in this file. No dial-side rule changes: D42 ruling 6 governs the baked
dial, not the design.

## 9. The OS theme runtime, and retiring `/composer/` (rulings 4 and 7)

### 9.1 The boundary

An app's editor authors **content**. The OS **builds the theme file** from it. The
hub serves that capability to every app, so no app reimplements packing,
quantization or the format the firmware reads.

| Side | Owns | Files today |
|---|---|---|
| **OS** (hub code, served to any app) | the editor runtime (React, ReactDOM, Babel, the fonts, the dc-runtime); the theme builder: the container and sections, row and table checks, RGB565 quantization and the dither (§5.3), plate banks and deltas, folding, the budget and cost estimate (`estimate3`), the device blend mirror behind Panel view (§5.2), the design region (§8), and the upload door | `vendor/*`, `support.js`, `orbtheme-baker.js` (`GorbBake`) |
| **App** (in the `.orbapp`'s editor region) | its editor UI and the content it generates: the stack, the procedural art, the sounds, and the translation from its own vocabulary into the parts the OS accepts | `index.html`, `watch-engine.js`, `dial-renderer.js`, `sound-engine.js`, `editor.json`, the bridge shim |

**The hand-off.** An app gives the OS: rows (D42's five parts), planes and plate
frames, sprites, sound slots, scalars, the theme name, and its design document
(opaque to the OS). The OS returns the built bundle, its budget verdict and its
estimate, and sends it to an orb on request. The firmware reads what the OS
writes, so the format, its checks and its device mirrors live on the OS side of the
line, which is where a format change has to be made anyway.

**What crosses back to the app.** A few baker helpers translate the composer's own
vocabulary rather than the format: `v6Drive` (D34 motion names to rows),
`compPlan3`, `pointerPlan3`, and `bakeKit` (the FORMAT 2 layer kit). They belong to
the clock app. P4's stack emits rows directly, so `v6Drive` goes when P4 lands, and
the rest move into the app's region or are deleted with their callers.

**Versioning.** The runtime is served under a neutral path that names its version,
for example `/runtime/gorbbake@<major.minor>/`. An app's `editor.json` `runtime` list
(which exists already) names what it needs. The hub serves a matching version or
refuses the editor with a sentence naming the version it lacks. It does not silently
substitute the way the offline map does today. A breaking change to the builder's
hand-off is a new major version. The hub keeps serving the previous major for as
long as an app in its library or on a known orb still names it.

### 9.2 What goes

- The `/composer/` route and everything that serves it as a route:
  `tools/orb-manager/composer.go` (the repo-or-embedded source switch,
  `hubBackLink`, `hubDepWatch`, the URL rewrites), the composer branch in `main.go`,
  and the Makefile's `composer` target.
- The hub-embedded copy of the composer **document** and its app scripts under
  `tools/orb-manager/web/composer/`, and the tests that pin it
  (`TestEmbeddedComposerMatchesSource` and the parts of `composer_test.go` and
  `composer_offline_test.go` about the document). The offline and vendor checks move
  with the runtime.
- `docs/watch-composer-plan.md` is marked superseded by this file. It evaluated the
  standalone tool before D37.

### 9.3 Where things live after P3

- **The OS runtime's source** moves into the hub's tree (for example
  `tools/orb-manager/web/runtime/`): `support.js`, `orbtheme-baker.js` and the
  `cmd/orbvendor` output. That tree is the one source. The headless suites under
  `tools/watch-composer/_selftest/` that test the baker follow it.
- **The clock app's sources** move to an app source home of their own (for example
  `apps/clock/editor/`): the document, `watch-engine.js`, `dial-renderer.js`,
  `sound-engine.js`, the bridge shim and the app-side helpers from §9.1.
  `make_clock_region.py` reads from there and rewrites the runtime script sources to
  the versioned `/runtime/` paths. The exact path is settled in P3; no app source
  tree exists yet (the `.orbapp` files are built by `make_factory_apps.py`).
- **`tools/watch-composer/` is deleted** (ruling 8). There is no standalone
  `file://` path and no dev shim. Developers edit through the hub, which already
  serves the region from the library when the versions match (§1.2). The headless
  suites in `_selftest/` move with what they test: the baker's to the runtime tree,
  the UI's and the engine's to the app. `deploy_test_folder.py` and the `probe-*.html`
  pages go unless a suite still loads them.
- **Every reader of the old path is repointed before the delete.** `git grep` at P2's
  close (2026-11-01) for `watch-composer` and `/composer/`, outside `docs/`:
  - tools: `bake_clock.py`, `theme_schema.py`, `make_default_theme.py`,
    `make_bundle_fixture.py`, `make_clock_region.py`, `host_clock_region_test.py`,
    `bench/loopcheck.py`;
  - hub: `composer.go`, `composer_test.go`, `composer_offline_test.go`, `main.go`,
    `window.go`, `window_test.go`, `internal/orbnet/themeapps.go`,
    `themeapps_test.go`, `cmd/orbvendor/main.go`, the Makefile, `.gitignore`;
  - the selftests that name their own folder: `headless-bundle3`, `headless-estimate`,
    `headless-orbtheme`, `make_art_kit.py`, `orbtheme_vectors.py`,
    `pivot_passthrough_check.py`, `deploy_test_folder.py`;
  - `.clinerules` (the composer testing recipe).
  Re-grep when P3 starts.

### 9.4 As built (P3, 2026-11-01)

- **Homes.** The clock editor's sources are `apps/clock/editor/` (the document,
  `watch-engine.js`, `dial-renderer.js`, `sound-engine.js`, `deploy_test_folder.py`,
  and the selftests that exercise the app). The runtime is
  `tools/orb-manager/web/runtime/`: `support@1/support.js`, `baker@1/orbtheme-baker.js`,
  and `vendor/` (written by `cmd/orbvendor`, now `-editor` and `-support` inputs). The
  runtime-only suites (`headless-dither`, `headless-panel`, `headless-orbtheme` and its
  `orbtheme_vectors.py`) live in `web/runtime/_selftest/` with their own runner;
  `go:embed` skips `_`-prefixed directories, so they stay out of the binary. The app
  suites find the runtime through `_selftest/runtime-paths.js`; the browser pages keep
  loading `../orbtheme-baker.js`, because the deployer flattens the runtime's two files
  into the test folder beside the app's.
- **The route.** `runtime.go` serves `/runtime/<id>/` from a catalog: `support@1` and
  `baker@1` are their own directories (revalidated, no-cache), and `react@18.3.1`,
  `babel@7.29.0` and `fonts@1` map onto the vendor tree, each serving only its own
  files (immutable). Guarded by the launch token, like `/editor/`.
- **The check.** `serveEditor` reads the region's `editor.json` before handing out an
  entry document and refuses one naming an id the hub lacks, or an unversioned entry
  (an editor from before the runtime), with a page naming both sides. `make_clock_region.py`
  writes the versioned ids, points the two script tags at `/runtime/`, sets
  `window.__resources` ahead of `support.js`, and collapses the Google Fonts sheets onto
  `/runtime/fonts@1/fonts.css`.
- **What else moved.** The clone marker (`repoMarkerRel`) is `tools/orb-manager/go.mod`;
  `editorAvailability`'s fallback for dials that cannot answer no longer names
  `/composer/`, so such a dial offers no editor. `composer.go` and its two test files
  are deleted; `TestComposerHasNoUnknownExternalDependencies` survives as
  `TestRuntimeScriptsReachOnlyVendoredURLs`. `TestTheShippedClockEditorRunsAgainstThisHubsRuntime`
  serves the library's real `clock.orbapp` through `/editor/` and fetches every
  `/runtime/` URL its entry names. Chrome rendered that entry from a hub-shaped layout
  with no network.
- **Found while moving, not fixed here.** `gfx_parity_test.py` fails on `tick_timer_cb`
  and `bench/loopcheck.py` fails four source checks (`_budget`'s flag, the seal mode);
  both fail identically at `edabd47`, before D43.

## 10. Slices

| Slice | Content | Gate |
|---|---|---|
| **P0** ✔ | Measure the banding: bake presets with gradients, `fbgrab` them, and record where the glass bands (plate vs live rows) | **done 2026-11-01 (§5.4)**: host and glass agree; the overlay's live blend dominates |
| **P1** ◐ | Panel view and Compare (§5.1–5.2): the JS mirror of `blend_px` (all three modes), the device quantization of each stage layer, crystal overlay | **built 2026-11-01**: `GorbBake.mix1616/blendPx/panelPlate/panelLayer/panelRGBA`, `WatchEngine.composeLayers`, the Render row (Design · Panel · Compare with a wipe). `headless-panel.test.js`: `blendPx` equals the C lifted from `orb_gfx.c` on 300,000 random texels, the panel plate and overlay equal the bake's stored codes, 8/8 mutants caught. Glass: the device model equals `fbgrab` outside the hands (§5.4). **Owed:** hands are rotated by the canvas (smoothed), the device samples nearest, so a hand's edge can differ by a texel — not yet measured against `bench_preview_parity.py`'s tolerance; sub-dial pointers are drawn in the under-hands layer, so Panel dithers them while the device ships them truncated |
| **P2** ◐ | Bake-time blue-noise dither (§5.3, §5.4) of every still, screen-locked plane, colour and alpha: the plate, its bank, folded layers, the crystal overlay (alpha on the 32-step lattice). The table and its generator (`tools/make_bluenoise.py`), JS and Python twins, the per-layer `dither` setting with `on` allowed for rings and discs, every preset rebaked | both table copies pinned to one hash; vectors equal in both twins, with mutants caught (table transposed, frac comparison flipped, truncation restored); exact 565 colours unchanged at every pixel; delta row band and bank bytes for every animated preset recorded before and after; §5.4's band metric re-run on the new bakes; the glass no longer bands where P0 recorded it — **built 2026-11-01**: `tools/make_bluenoise.py`, `tools/orbdither.py`, the baker's still-row dither; `headless-dither.test.js` 24 checks, 11/11 mutants; bank bytes and delta texels unchanged on Skeleton, Luna and Neon moon; glass contour 17.9 % → 6.9 % on orb-B (§5.4). the hub default (`tools/orb-manager/themes/skeleton.orbtheme`) rebaked and installed through `make_default_theme.py`. **Owed:** the composer's per-layer `dither` setting, which needs the stack (P4) |
| **P3** ✔ | The OS theme runtime and the end of `/composer/` (§9): the runtime's source moved into the hub tree and served under versioned `/runtime/` paths; `editor.json`'s `runtime` list validated, with a refusal that names the missing version; the clock app's sources moved to their own home and the region built from there, pointed at `/runtime/`; every reader of `tools/watch-composer/` repointed; then the route, `composer.go`, the embedded document copy, their tests and `tools/watch-composer/` deleted | the clock editor loads in the Themes pane with no `/composer/` route present; an editor naming a runtime version the hub lacks is refused with its reason; the baker's headless suites pass from their new home; `make_factory_apps.py --check` and `go test ./...` green; no reference to `/composer/` or `watch-composer` left in `tools/` (grep) — **done 2026-11-01** (§9.4) |
| **P4** ✔ | The stack model and panel (§2, §3), D42 E2's composer half: one list, parenting, shadows as properties, the three bands, R1–R7 enforced on drop, R8 warnings behind the Mechanical switch, style inheritance (§2.3). Every preset expressed as a stack | headless tests per rule (each illegal drop refused with its reason, each R8 warning raised only with the switch on); each preset's stack bakes to the same v6 rows `v6Drive` produces today, or the difference is explained; D42 E2's own gate: preview-versus-reader pose check, each preset installed on orb-B — **done 2026-11-01** (§3.1); still-row promotion split out as **P4b** |
| **P4b** ✔ | Still-row promotion (§3 R5, R7): a still layer placed above a moving one ships as a live still AUX row, `compose()` draws the row band in stack order, and the dial is promoted when a ring sits under it | the panel moves that P4 refuses with "P4b" land, bake to still rows the C reader accepts, and draw on orb-B where the preview does — **done 2026-11-01** (§3.1) |
| **P5** ✔ | The build flow and layout (§4): step bar, Add catalogue with compatibility, properties grouped by D42 parts, diagnostics drawer, light moods | headless UI test walks Start → Render on a scratch build and on a preset; nothing removed from today's controls (a checklist of every current field and where it now lives) — **done 2026-11-02** (§4.1) |
| **P5b** ✔ | The studio layout (§4.2): the editor fills the hub window as one three-pane studio — layers left on every step, the stage centre, the inspector right — in the hub's palette, with one title bar; Arrange folds into the layers panel; the hub page itself goes fluid | nothing removed (the P5 checklist, re-pointed at the new panes); the stage zoom stays inside its pane at every stop; no page scroll at 1280×720 and 1920×1080 (Chrome screenshots of both, inside the hub); `go test` and every app suite green — **built 2026-11-02** (§4.2); the owner's test on the hub is owed |
| **P5c** ✔ | Hands are complications (§4.3): one Add catalogue, hands placed anywhere in any number, a selected hand's panel alone; the plate rows open Movement, and its backdrop and the plate art sit at the tab's foot | the presets render and bake pixel-identical in the new shape; ten hands on the dial within R6; every suite green — **built 2026-11-02** (§4.3 as built); the presets' hand geometry is identical across 22 designs; a bench bake of the new pointer sources and the owner's test are owed |
| **P6** | §7: autosave, My designs, undo, reset confirmation, `?orb=`/`?app=`. Then the live-blend coverage measurement (§5.3, last bullet) | headless tests: reload restores, undo restores the previous bake input exactly; coverage figures recorded here |
| **P7** | The design region (§8): the owed checks first, then the writer in JS and Python, the readers' tolerance, the hub's **Edit** door and `?theme=` | a theme baked, installed, then reopened from the card and from the shelf restores a design equal to the one exported; a design region with a broken CRC is refused as a design while the dial installs and draws; a design stamped newer is refused with the version it needs |

P0 comes first because it is cheap and decides how much of P2 and P6 is needed. P1
and P2 are independent of the rest, so the posterization fix does not wait for the
composer rework. P1 and P2 are OS work (§9.1), so every app that bakes a theme gets
the panel mirror and the dither, not just the clock. P3 comes before P4 so that the
rework is built against the versioned runtime from its first commit. P4 must land before P5 (the flow is a
view of the stack) and before P7 (the design region carries the stack).

Every slice that touches the editor ends by regenerating its region
(`make_clock_region.py` through `make_factory_apps.py --check`), so the `.orbapp` and
its version carry what was built.

## 11. Rulings (owner, 2026-11-01)

1. **Enforcement.** R1–R7 are enforced: an illegal drop does not land. R8 is a set of
   warnings, raised only when the design's Mechanical switch is on.
2. **Dither pattern.** Blue noise from the start. No Bayer stage.
3. **Rotating discs.** Dither may be turned on for ring and disc rows. Arm rows stay
   off under `auto`.
4. **The old route.** `/composer/` is retired completely and the hub-embedded composer
   document is deprecated and removed (§9.2). The shared files it served are not
   removed: they become the OS runtime (ruling 7).
5. **Reopening themes.** The design travels inside the `.orbtheme` so a theme can be
   edited again. In scope for this plan (§8, P7).
6. **Precedence.** For the theme editor and the design, this file supersedes D42. D42
   carries a pointer here.
7. **The OS builds the theme (2026-11-01).** React, Babel, the fonts, the dc-runtime
   (`support.js`) and the theme builder (`orbtheme-baker.js`) are hub-level support
   files, part of what the OS offers every app. An app hands the OS the parts of a
   theme and the OS builds the file. Everything else stays with the hub code (§9.1).
8. **No standalone composer (2026-11-01).** `tools/watch-composer/` is deleted in P3
   once its contents have moved (§9.3). The `file://` path is not kept.
9. **The blend rounds (2026-11-01).** `mix_16_16` and LVGL's RGB565 image blends round
   each field instead of truncating (§5.4). The clock app's version moves with the
   editor changes (v1.1.45), so a version stamp still names one set of bytes.

## 12. Open questions (raised during execution)

None open. Q1 (rounding in `mix_16_16`) became ruling 9.

## 13. The one sentence to remember

A watch is one stack of D42 rows between a pinned plate and a pinned crystal, built
bottom up, reordered only where the format allows, previewed as the panel will
actually show it, and shipped with its design so it can be opened again.
