# P5e-2: a face lifts only when something below it passes under it

## Goal

Slice P5e of D43 (`docs/specs/p5e-design.md`: read ruling 2, amendment 2 and the refinement
to it). Today a sub-dial face is lifted exactly when `design.order` names `comp:<id>`, and it
then ships as a still row whatever lies below it. From now on the engine decides: a face
lifts (ships as a still row at its depth) only when a moving layer lower in the stack reaches
under its ink; otherwise it folds into the plate, wherever the author ordered it. The
verdict lives in one place, `liveSeq` in `apps/clock/editor/watch-engine.js`, and every
consumer (the preview's compose, the Panel's composeLayers, the Layers panel and its row
count) reads it from there.

## Read first

- `docs/specs/p5e-design.md`.
- `apps/clock/editor/watch-engine.js`:
  - `liveSeq(lay, design)` and `handOrder` (near "D43 P5d: the draw order, bottom first");
  - `handSlots` just above it, and `buildHands` (the hand objects: `x`, `y` = the hand's
    own arbour from `lay.G[id]`, `len`, `sp` = `{ c: canvas, px, py }` sprite and pivot,
    `host`, `sub`, motion fields);
  - the sub-dial layout in `geom`/the layout builder near `subs.push(s)`: a face is
    `{ id, x, y, r, form, slide: { hl, hh, ... } }`; a slider face's extent is its plate
    along the track (`hl` half length, `hh` half height);
  - the wheel layout `lay.wheels`: `{ id, x, y, r, exposed, holes: [...] }` (each wheel's
    cutout holes; `holes` entries are the hole shapes `inside(h, x, y, m)` and `_holePts(h)`
    understand: `circle` with `r`/`ry`, `poly`, `moon`, rect `w`/`h`, all with `x`, `y`,
    `rot`);
  - where `face()` builds `liveSeq: liveSeq(lay, design)` (near `handOrder: handOrder(lay,
    design)`), `compose`'s live band ("D43 P4b: the live band in stack order",
    `const lifted = {}; (F.liveSeq || [])...`) and `composeLayers`.
- `apps/clock/editor/stack.js` (the header's R-rules, `build`'s `subBlock` badge,
  `liveKeys`, `rowCount`), and `apps/clock/editor/clock-logic.js` `stackInput()`.
- `apps/clock/editor/clock-logic.js`: grep `liveSeq` and `kind === 'sub'` for every other
  consumer (the Panel still rows near "still rows (dithered, as the bake ships them)", the
  bake's `seq3` stills block and its depth list in `t3bake`).
- `apps/clock/editor/_selftest/headless-stack.test.js` (how stack tests build input) and
  `headless-subdial.test.js` (created by step 1; add to it).

## Contract

1. `liveSeq(lay, design)` keeps its signature and its order exactly (same entries, same
   positions), with one change: an entry `{ kind: 'sub', id }` whose face does NOT lift is
   emitted as `{ kind: 'fold', id }` instead. A face that the author did not order among
   the hands, but that a lower wheel reaches (below), IS lifted: it is emitted as
   `{ kind: 'sub', id }` at the bottom of the live band, i.e. before the first dial hand in
   the sequence, followed by its own hands (exactly where the sequence already puts a
   lifted face's own hands: right after the face). Its own hands then leave the "faces on
   the dial" group at the start.
2. Export `faceReach(lay, design, faceId, below)` from WatchEngine (add to the big export
   object at the end of the file) returning `{ lifts: boolean, by: <id or null> }`, the one
   predicate `liveSeq` uses. `below` is the list of hand ids lower in the sequence than the
   face. Rules:
   - **The face's ink**: a circle at `(s.x, s.y)` with radius `s.r + 4` for a round face; for
     a slider, `Math.hypot(s.slide.hl, s.slide.hh) + 4` about `(s.x, s.y)`. (The 4 px covers
     the face's rim and drop.) All in layout space; the lens scales everything equally.
   - **A hand's reach**: its swept disc about its own arbour `(h.x, h.y)`, radius = the
     largest distance from the pivot `(sp.px, sp.py)` to the four corners of its sprite box
     `(0..sp.c.width, 0..sp.c.height)`; if the hand has no sprite, `h.len`. A hand that slides
     (`motion.path` `slide`, `slide-x`, `slide-y`) adds its track length (from
     `slideGeom(motion)`'s span, absolute value, px) to that radius. A hand hosted on the
     face itself never counts. It lifts the face when
     `Math.hypot(h.x - s.x, h.y - s.y) < reach + faceRadius`.
   - **A wheel's reach** is its VISIBLE disc: for an exposed wheel (`exposed === true`) its
     whole disc `(w.x, w.y, w.r)`; otherwise only its cutout holes `w.holes`. Only wheels
     hosted on the dial count (`host` dial or absent): a wheel hosted on this face sits under
     it and is covered by it; a wheel on another face is not below this one. A covered
     wheel lifts the face when any hole overlaps the face's ink circle: some point of
     `_holePts(hole)` lies inside the circle, or the face centre is `inside(hole, ...)`. An
     exposed wheel lifts it when the two discs overlap. Wheels are always below every face
     in the stack (they sit just above the plate), so every dial wheel is "below" every face.
   - `by` is the id of the first layer found that lifts it (hands first in sequence order,
     then wheels in `lay.wheels` order), for the Layers badge.
3. Every consumer treats `fold` as "on the plate":
   - `compose` (the preview): the `lifted` set is built from `kind === 'sub'` only (already
     the case); in the live band loop a `fold` entry is skipped (no flush, no face drawn
     there: it was drawn with the plate faces); the single-call path ("no sub entry at
     all") applies when no entry is `sub`.
   - `composeLayers`: a `fold` entry is skipped (no still layer, no flush).
   - `clock-logic.js`: every `kind === 'sub'` test stays as it is, and every loop over
     `liveSeq` that does `else` on non-hand entries must skip `fold` (check the Panel still
     rows block, the `seq3` stills block and the depth list in `t3bake`). `stackInput()`
     maps `fold` to `'comp:' + id` in `order`, exactly like `sub`, so a drag in Layers keeps
     the author's position, and adds `folded: [ids of fold entries]` and `liftedBy:
     { faceId: byId }` to the input.
   - `stack.js`: `build` keeps the face's position in the live group when it is ordered
     there; when its id is in `input.folded` its badge reads `baked in · nothing passes
     under it` instead of `above the hands · drawn once`, and its `lifted` flag stays as
     today (it describes position, not shipping). A face lifted by a wheel only appears in
     `order` because `stackInput` maps its `sub` entry (it then sits at the bottom of the live
     band in Layers, which is where it ships). `rowCount` counts a `comp:` entry as a still
     row only when its id is not in `input.folded`. Update the
     R5 line of the header comment: "R5 only still layers fold into the plate, and a face
     folds unless something below it passes under it (P5e)".
4. Nothing about hand order changes: `handOrder` (the sequence's hands) is identical before
   and after for every design.

## Behaviour and edge cases (each is a check in `headless-subdial.test.js`)

Build faces with the suite's `face(comps)` helper (as in headless-wheels: `E.face({ design:
E.canonDesign({ print: 'roman', comps, order }) , ...O })`; put `order` inside the design).

- A face with a lower hand that never reaches under it folds: sub-dial `s` at angle 180,
  dist 102, size 30; a dial hand `x` of `shows: 'second'`, length 40, with `dist` and
  `angle` placing its arbour at angle 0 dist 120 (far from `s`); `order: ['x', 'comp:s',
  ...the centre hands]` → `liveSeq` has `{kind:'fold', id:'s'}` at that position and no
  `sub` entry.
- A centre hand long enough to sweep under it lifts it: the same face with the centre hour
  hand (length 132, centre arbour) ordered under it (`order: ['h2', 'comp:s', 'h3',
  'h4']`) → a `{kind:'sub', id:'s'}` entry; `faceReach(...).by === 'h2'`.
- The same face with only a short centre hand under it (length 40) folds.
- A hand hosted on the face, below nothing, never lifts its own face.
- A covered dial wheel whose hidden part lies under the face but whose cutout does not:
  Triple calendar itself (`E.PRESETS.calendar.design`, canonDesign'd, no `order`): ring
  wheels `c2`/`c3` (size 117, centre) have cutouts at 270 and 90, the sub-dial `c4` sits at
  180 dist 102 → no `sub` entry for `c4`; `faceReach` says `lifts: false`.
- A covered dial wheel whose cutout lies under the face lifts it: a ring wheel at the
  centre with a cutout at `at` pointing at the face (choose `read`/`at` so the cutout's hole
  centre is within the face) → `c4`-like face lifted with `by` = that wheel id, emitted at
  the bottom of the live band.
- An exposed dial wheel whose disc overlaps the face lifts it; one that does not, does not.
- `handOrder` is unchanged for every preset (compare against `liveSeq` before your change:
  compute it in the test as `liveSeq(...).filter(e => e.kind === 'hand')` and assert it
  equals `face().handOrder` and that every preset's hand list is the same set as its
  `handSlots`).
- Stack: `S.rowCount` of an input whose `order` has `comp:s` and `folded: ['s']` counts no
  still row for `s`; without `folded` it counts one. `S.build` gives the folded face the
  `baked in · nothing passes under it` badge.
- Every preset's `face()` builds and its `liveSeq` has no `sub` entry unless its design has
  an `order` naming a face that a lower hand reaches, or a dial wheel's visible area reaches
  it. Print, in the test's VERBOSE detail, each preset whose sequence has a `sub` or `fold`
  entry (for the report).

## Acceptance criteria

1. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-subdial.test.js` → exit 0, `all green`.
2. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-test.js` → exit 0.
3. `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` → exit 0.

## Out of scope

- The bake's pointer rows, compPass3, auxPass3, wheelPass3 and the depth list's content
  (steps 3 and 4). In this step the bake only has to skip `fold` entries where it loops.
- `F.compHoles` (owner's uncommitted work): do not touch it. Leave the existing
  `headless-wheels.test.js` checks alone unless one asserts behaviour this slice retires;
  then change that one check minimally and name it in your report.
- Firmware (`apps/clock/main.c`, `main/`), version files, shelf, docs.

## Notes

- The preview should look identical for a folded face whether it is drawn with the plate or
  in the band (nothing below reaches it), so this is a shipping change, not a visual one.
- Keep `faceReach` pure (no canvas reads beyond `sp.c.width/height`), so node can test it.
