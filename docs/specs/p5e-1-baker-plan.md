# P5e-1: the baker stops planning sub-dials as value rows

## Goal

Slice P5e of D43 (`docs/specs/p5e-design.md`, a verbatim copy of the owner's §4.5 with its
binding amendments: read it all first). This step is the baker half: a sub-dial is never a
value row in `compPlan3` again, the pointer-plan path that existed only for the
plate-per-value sub-dial (`pointerPlan3`, `pointerDefault3`, `COMP3_RING_FULL`) is retired
with its two refusals, and the baker's writer learns to place a turning wheel row at a depth
the stack names (`wheel:<id>`), which later steps use for a wheel hosted on a lifted face.
The editor (`apps/clock/editor/clock-logic.js`) already makes every hand without a hand slot a
pointer row once no sub-dial is a plate row, so after this step a date sub-dial's hand is a
pointer row; later steps clean up the editor side.

## Read first

- `docs/specs/p5e-design.md` (all of it: the rulings, the wire, the amendments).
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js`:
  - `liveComps3`, `COMP3_SCALE_SRC`, `HAND3_SCALE`, `faceHand3`, `subScale3`, `subTrack3`,
    `COMP3_SHOW_SRC`, `COMP3_RING_FULL`, `compPlan3`, `pointerDefault3`,
    `COMP3_POINTER_SPAN`, `pointerPlan3`, and the `root.GorbBake = {` export list at the end
    of the file (grep the names; line numbers drift).
  - the writer's wheel rows ("D43 P5d.3: the wheels. Each disc is a turning row"), the cover
    check right after it ("wheels with no cover"), the still rows ("D43 P4b: a still row")
    and the comp rows' `bank:<id>` depth just below them. Read how `rung3` and `dOrder3` are
    built near "o.depthOrder — the author's ONE hands list".
- `apps/clock/editor/_selftest/headless-forms.test.js` lines near 85-100 (pointerDefault3 /
  compPlan3 checks), `apps/clock/editor/_selftest/headless-bundle3.test.js` sections "7e"
  and "7e2" (compPlan3 / pointerPlan3 checks), `apps/clock/editor/_selftest/bake3-test.html`
  (calls `G.pointerPlan3`), `apps/clock/editor/_selftest/headless-wheels.test.js` (style of
  a suite: `ok(cond, name, detail)`, the `face(comps)` helper, the preamble requiring
  `headless-stub.js`, `clock-logic.js`, the baker via `runtime-paths.js` and `stack.js`).
- `apps/clock/editor/watch-engine.js` near "delete c.carrier" in `canonDesign` (a comment
  names `pointerDefault3`).

## Contract

Files to modify:

- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js`
- `apps/clock/editor/_selftest/headless-forms.test.js`, `headless-bundle3.test.js`,
  `bake3-test.html` (existing checks of the retired behaviour are rewritten to the new
  rulings, never just deleted without a replacement check where a ruling covers them)
- `apps/clock/editor/watch-engine.js`: the one comment near `delete c.carrier` only (it now
  reads that a sub-dial's hands are always pointer rows, D43 P5e). No code change in this
  file in this step.

File to create:

- `apps/clock/editor/_selftest/headless-subdial.test.js`: the P5e suite. Same preamble and
  `ok()` style as `headless-wheels.test.js`; ends with the same `all green (N checks)` /
  `RESULT: FAIL` line and exit code. `headless-test.js` picks it up by its name. Later steps
  add to it.

Baker behaviour:

1. `compPlan3(comps)`: a comp of type `subdial` never produces a row and never a refusal,
   whatever it hosts, whatever its (legacy) `scale`, `form` or `carrier` fields say. It is
   skipped exactly as `hand` and `cutout` are, with the comment
   `// D43 P5e: a sub-dial is a face; its hands are pointer rows, its wheels are wheels`.
   A comp hosted on a sub-dial (window, moon, wheel) therefore plans with `hosted: null`
   (its host is not a row) and its `host` field still names the sub-dial. Every other type
   plans exactly as before (byte-identical rows for designs with no sub-dial).
2. Remove `pointerPlan3`, `pointerDefault3`, `COMP3_RING_FULL`, `COMP3_POINTER_SPAN`,
   `subTrack3`, `faceHand3`, `subScale3`, `HAND3_SCALE` and `COMP3_SCALE_SRC` if nothing
   else in the file uses them after the change (grep each one; keep and say why in your
   report any that something else still uses). Remove `pointerPlan3` and `pointerDefault3`
   from the `GorbBake` export list. The two refusal sentences
   (`hosts another indication` / `does not close under its own hand`) must not exist anywhere
   in the repository afterwards (`grep -rn` over `apps tools` finds nothing outside
   `docs/`).
3. `liveComps3`: a `subdial` is not a live comp (returns false for it). Update the comment
   above it accordingly (a sub-dial is face paint; its hands are hands).
4. The writer (`bakeClock3`), turning wheel rows: today every disc goes at `order: 0`. New:
   when `o.depthOrder` lists `'wheel:' + w.id`, that wheel row takes `order:
   Math.max(1, Math.min(63, rung3['wheel:' + w.id]))`, exactly as a comp row takes its
   `bank:<id>` rung. A wheel not listed keeps order 0. Also accept on a wheel row the field
   `face` (a string, the id of the lifted face that hosts it). A wheel with `face` set:
   - must be listed as `wheel:<id>` in depthOrder, and depthOrder must list `comp:<face>`
     with a higher rung, else throw
     `'the wheel ' + q(id) + ' turns under the lifted face ' + q(face) + ' and has no place just under it in depthOrder'`;
   - does not need the wheels' cover: the cover check becomes "throw `wheels with no cover:
     every disc would show whole over the face` only if some wheel is neither `exposed` nor
     has `face` set and `o.cover` is absent". A cover with no wheels under it still throws as
     today; a cover whose only wheels all have `face` set throws
     `'a wheel cover over no wheel it covers: every wheel here turns under a lifted face'`.
5. Nothing else in the writer changes. The pointer (aux) rows, shadow twins, `SUBDIAL_RUNG`
   natural ladder and the still rows stay as they are.

## Behaviour and edge cases (each is a check in `headless-subdial.test.js`)

- Two hands on one date sub-dial: `compPlan3([{id:'s',type:'subdial'},{id:'a',type:'hand',host:'s',shows:'date'},{id:'b',type:'hand',host:'s',shows:'hour'}])`
  has no rows and no refusals.
- No sub-dial ever appears in `compPlan3(...).rows` for every preset in
  `WatchEngine.PRESETS` (iterate all of them, canonDesign'd: `E.canonDesign(p.design).comps`),
  and no preset's `compPlan3` or `wheelPlan3` refuses.
- A moon wheel hosted on a sub-dial plans exactly as one hosted on the dial: for
  `{type:'wheel',shows:'moon'}` (snapping) and `{type:'wheel',shows:'moon',cadence:'smooth'}`
  (turning), the `compPlan3` row (snapping) or `wheelPlan3` row (turning) is deep-equal
  between host `'dial'` and host `'s'` after deleting the `host` and `hosted` fields.
- A legacy face with `scale:'date'`, `form:'fan'` or `carrier:'pointer'` plans no row and no
  refusal; a sub-dial hosting a window keeps the window's own row (`hosted: null`).
- The retired names: `B.pointerPlan3 === undefined && B.pointerDefault3 === undefined`, and
  the baker source (read with `fs`) contains neither refusal sentence nor `COMP3_RING_FULL`.
- Writer, wheel depth: bake (with `B.bakeClock3`, tiny planes as in headless-wheels'
  "with no cover, since nothing covers it" check) one turning wheel with `face:'s'`, a still
  row `stills:[{id:'s',...}]` and `depthOrder:['h','wheel:w','comp:s']`: no throw, and read
  the rows back (use whatever headless-wheels or headless-bundle3 already use to read a
  baked table: `B.readDesign3`/`verifyBundle3`/the bake's returned table; find it) so that
  the wheel row's order is lower than the still row's and higher than 0. The same bake
  without `wheel:w` in depthOrder throws the "has no place just under it" sentence. A
  `face`-set wheel with no cover does not throw "wheels with no cover"; a plain covered
  wheel with no cover still does.

## Acceptance criteria

Run each; all must hold.

1. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-subdial.test.js` → exit 0, prints `all green`.
2. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-test.js` → exit 0 (every clock suite green).
3. `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js` → exit 0.
4. `grep -rn "does not close under its own hand\|hosts another indication\|COMP3_RING_FULL\|pointerPlan3\|pointerDefault3" apps tools` → no output.

## Out of scope

- `apps/clock/editor/clock-logic.js` (steps 2 to 4 change it), `stack.js`, `watch-engine.js`
  code, `apps/clock/main.c` and anything under `main/` (firmware: read only, unchanged).
- Do not touch `F.compHoles` in watch-engine.js. Put new checks in
  `headless-subdial.test.js`; leave the existing `headless-wheels.test.js` checks alone
  unless one asserts behaviour this slice retires, then change that one check minimally and
  name it in your report (the owner has uncommitted edits in that file).
- No version bumps (`version.txt`, `make_factory_apps.py`), no shelf re-bake, no docs edits.

## Notes

- The firmware already drives any row from any source about its own pivot; nothing on the
  wire changes. If you come to believe a firmware change is needed, stop and report it as
  `blocked` with the reason; do not make it.
- `canonDesign` already deletes a sub-dial's `scale` and `carrier`; raw comps in tests may
  still carry them, which is why compPlan3 must ignore them outright.
