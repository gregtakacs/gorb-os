# P5e-3: every sub-dial hand is its own pointer row, and the bake's depth list says where faces ship

## Goal

Slice P5e of D43 (`docs/specs/p5e-design.md`: rulings 1, 2 and 4, amendment 1). Steps 1-2
made the baker plan no sub-dial value row and made the engine decide which faces lift
(`liveSeq` entries `sub` = lifted, `fold` = on the plate). This step makes the editor's bake
pass (`apps/clock/editor/clock-logic.js`) match: every hand without a hand slot is one
pointer row with its own source, its own drive and its own authored pivot; the leftovers of
the plate-per-value sub-dial go; and the bake's depth list is one pure, tested function that
puts each lifted face's hosted rows just under its still row.

## Read first

- `docs/specs/p5e-design.md`.
- `apps/clock/editor/clock-logic.js` (grep the names; lines drift):
  - `_export3Body`: the plan block (`G.compPlan3`, `this.pointerRows3`, `G.wheelPlan3`,
    `lifted3`, `PTSCALE3`/`this._ptrHint`), and the `catch` near "complication tiles cost"
    that appends `this._ptrHint`.
  - `POINTER3`, `handSeats3`, `driveCols3`, `pointerRows3`.
  - `compPass3` (`DATE3`, `kids`, `ptr`, `handsOf`, the `only` list).
  - `auxPass3` (the pose, the pivot `hd3`/`g`/`kP`/`gx`/`gy`, the `holes` punch with
    `compHoles`, the shadow twin).
  - `t3bake`: the "D43 P4b: every sub-dial face lifted among the hands ships as a still row"
    block (`seq3`, `stills`) and the depth list ("The stack's live band, bottom first (D43
    P4b)": `dep`, `bank:`, `comp:`, `opts.depthOrder`).
  - `livePlan4` (it calls compPlan3/pointerRows3/wheelPlan3/auxPass3 too).
- `apps/clock/editor/watch-engine.js` `liveSeq` (step 2's `fold` entries) and `buildHands`
  (a hand's `x`, `y` is its own arbour, `lay.G[id]`).
- `apps/clock/main.c` `comp_value`, `drive_units`, `drive_t`, `row_pose` (read only) and
  `main/art/clk_types.h` cadence enum (`CLK_CAD_JUMP = 3`).
- `apps/clock/editor/_selftest/headless-wheels.test.js` near "P5d.3l: each seconds hand its
  own rhythm" (how a test builds a `ClockLogic` instance and calls `pointerRows3`), and
  `headless-subdial.test.js` (add to it).

## Contract

1. `pointerRows3(D, plan)`: every hand comp that is drawn and has no seat in `handSeats3` is
   one row, including the first hand on any sub-dial (remove the `plates` lookup and its
   early `return`). The `plan` argument stays accepted for callers but is no longer read.
   Each row additionally carries `pivot: { x, y }` = the engine's laid-out arbour of that
   hand (`drawn[c.id].x`, `drawn[c.id].y`, layout space, unrounded), whatever its host.
   Source and range from `POINTER3` as today (date `['date', 1, 32]`, day `['weekday', 0,
   7]`, month `['month', 0, 12]`, ...). Drive: when `driveCols3(c.shows, motion)` answers,
   as today; otherwise a `date` or `day` hand gets `cadence: 3` (jump, `cad_arg: 0`): the
   preview moves it at the day boundary, and so must the row; every other kind keeps
   today's default (no cadence field: the baker's sweep).
2. `auxPass3(pp, onlyBase)`: the pivot is `r.pivot` when present (mapped through `kP` exactly
   as today's `g`), else today's fallback chain. Remove the `holes` punch from the pointer
   plane (the `compHoles(...)` call, the scratch-canvas punch, and the `kids` map if nothing
   else uses it): a pointer is above its face and everything that face hosts, and a punch at
   pose 0 would turn with the hand. The shadow twin is built from the unpunched plane as
   before.
3. `compPass3`: remove `ptr`/`handsOf` and the hands appended to a row's `only` list: no row
   carries a hand any more. The `ptrIds` parameter stays (frameCanvas still takes it).
   `kids`/holes for comps hosted on another planned row stay as they are.
4. `_export3Body`: remove `PTSCALE3`, `pt3` and `this._ptrHint`, and the `_ptrHint` append in
   the catch (the hint described a plates-driven sub-dial, which no longer exists). Leave
   the `lifted3` wheel refusal in place for now (step 4 replaces it).
5. New pure method `depthOrder3(seq, compRows, wheelRows)` on the class, used by `t3bake` in
   place of the inline `dep` loop, returning the list or `null`:
   - walk `seq` bottom first: a `hand` entry pushes its id; a `fold` entry pushes nothing; a
     `sub` entry pushes, in this order, `'wheel:' + r.id` for every wheel row with
     `r.face === e.id` (step 4 sets `face`; today none has it), then `'bank:' + r.id` for
     every comp row with `r.host === e.id`, then `'comp:' + e.id`.
   - when `seq` has no `sub` entry the answer is `seq`'s hand ids (exactly today's
     `fc.handOrder` result, so dials with nothing lifted keep their depth list and bytes);
     an empty `seq` answers `null` (today's "no depthOrder").
   `t3bake` then sets `opts.depthOrder` from it (when non-null), and builds `stills` only
   from `sub` entries (composeLayers already skips `fold`), so a folded face ships no row.
6. Nothing else changes in the bake: the hand slots, the comp rows' ordering, the wheel
   pass, the plate walk.

## Behaviour and edge cases (each is a check in `headless-subdial.test.js`)

Use a `ClockLogic` instance as in headless-wheels' P5d.3l block, with
`it.state.dial = 'calendar'` (and designs edited through `it.state.designs[...]` /
`it.faces = {}` as the suites do).

- Triple calendar as shipped: `pointerRows3` has a row for `h1` with `src 'date'`,
  `vmin 1`, `vmax 32`, `span 3600`, `cadence 3`, `host 'c4'`, and `pivot` equal to
  `c4`'s layout centre (`it.face().lay.subs` entry for `c4`, within 1e-6) since `h1` has no
  offset of its own. `compPlan3` has no row with id `c4`.
- Two hands on one date sub-dial: add `{ id: 'h9', type: 'hand', host: 'c4', shows: 'hour',
  length: 30, width: 3, cap: 3 }` after `h1` → two pointer rows `h1` (`date`, cadence 3) and
  `h9` (`hour12`, `vmin 0`, `vmax 12`, no cadence field), both `host 'c4'`, both `pivot` =
  `c4`'s centre.
- Amendment 1: the same `h9` with its own place moved off the sub-dial's centre (set its
  `angle`/`dist` fields the way the engine places a hand about its host; check `buildHands`
  and `lay.G`) → `h9`'s row `pivot` equals the engine's `it.face().hands` entry for `h9`
  (its `x`, `y`), which differs from `c4`'s centre by more than 1 px.
- A weekday hand on a face → `src 'weekday'`, `vmin 0`, `vmax 7`, cadence 3; a month hand →
  `vmin 0`, `vmax 12`, no cadence; a seconds hand on a face with motion `{ cadence: 'jump' }`
  keeps cadence 3 from `driveCols3` (today's P5d.3l check still holds).
- `depthOrder3`:
  - seq `[hand h2, sub c4, hand h1, hand h9, hand h3, hand h4]` with a comp row
    `{ id: 'c5', host: 'c4' }` and a wheel row `{ id: 'w', face: 'c4' }` →
    `['h2', 'wheel:w', 'bank:c5', 'comp:c4', 'h1', 'h9', 'h3', 'h4']` (the hosted wheel's
    rows between the hand that passes under and the face).
  - the same seq with `fold` instead of `sub` → `['h2', 'h1', 'h9', 'h3', 'h4']`.
  - seq with only hands → its hand ids; empty seq → `null`.
- The variant the bench uses: Triple calendar with `h9` added after `h1` and
  `design.order = ['h2', 'comp:c4', 'h3', 'h4']` → `it.face().liveSeq` has `sub c4` right
  after `h2` (the centre hour, 132 px, reaches under `c4`), and
  `it.depthOrder3(it.face().liveSeq, [{ id: 'c5', host: 'c4' }], [])` is
  `['h2', 'bank:c5', 'comp:c4', 'h1', 'h9', 'h3', 'h4']`.
- Source checks (read `clock-logic.js` with `fs`): no `handsOf`, no `_ptrHint`, no
  `PTSCALE3`, and `auxPass3`'s body contains no `compHoles`.
- For every preset (`Object.keys(E.PRESETS)`), on a fresh instance: `pointerRows3` refuses
  nothing, and every hand comp hosted on a sub-dial appears as exactly one pointer row.
  Collect a one-line summary per preset (`<preset>: pointers <id:src:vmin-vmax:cad>...;
  comp rows <ids>; seq <kinds>`) and print the lines when `VERBOSE` is set; the final report
  quotes it.

## Acceptance criteria

1. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-subdial.test.js` → exit 0, `all green`.
2. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-test.js` → exit 0.
3. `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` → exit 0.
4. `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js` → exit 0.
5. `cd apps/clock/editor/_selftest && VERBOSE=1 node headless-subdial.test.js | grep -c '^PASS'` → a number, and your report pastes the per-preset summary lines.

## Out of scope

- Wheels under a lifted face (step 4): `wheelPass3`, the `lifted3` refusal, the `face`
  field on wheel rows.
- `F.compHoles` (owner's uncommitted work). Leave the existing `headless-wheels.test.js` checks alone unless one asserts behaviour this slice retires; then change that one check minimally and name it in your report (the owner has uncommitted edits in that file).
- Firmware, version files, shelf re-bake, docs.

## Notes

- The firmware poses a pointer at `zero + span * (units - vmin) / (vmax - vmin)`; a date
  pointer at `vmin 1, vmax 32, span 3600` puts the 1st at the top and the 31st one step
  short of it, which is how the preview's `TURN.date` draws it. No firmware change; if you
  find one is needed, report `blocked` and stop.
- `auxPass3` poses date and month rows at 2026-01-01 and weekday at Sunday 2026-01-04; keep.
