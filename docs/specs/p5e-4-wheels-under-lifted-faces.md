# P5e-4: a wheel hosted on a lifted face turns just under that face

## Goal

Slice P5e of D43 (`docs/specs/p5e-design.md`: ruling 3 and the retired refusal in ruling 4).
A wheel hosted on a sub-dial is a wheel: snapping plates cut to its cutouts, or a turning row
under its cover. When the face is lifted (step 2's `liveSeq` `sub` entry), the face's own
still row is the wheel's cover, and the wheel's rows sit just under that still row, above
whatever passes beneath. Snapping wheels already ride under a lifted face as `bank:<id>`
(step 3's `depthOrder3`); this step does the turning wheel, retires the refusal
`the wheel ... turns under the face ..., which is lifted among the hands`, and gives the
bench driver page the two parameters the owner's Gold check needs.

## Read first

- `docs/specs/p5e-design.md`.
- `apps/clock/editor/clock-logic.js`: `_export3Body`'s plan block (`wp`, `lifted3`, the
  refusal), `wheelPass3(wp, onlyBase)` (the `seen` map from the black/white `wheelFill`
  renders, the per-wheel band `r0..r1`, the disc crop, the cover), `livePlan4` (calls
  `wheelPass3` too), `depthOrder3` (step 3), the `t3bake` lines that set
  `opts.wheels`/`opts.cover`.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js`: the wheel writer and cover
  check as step 1 left them (a wheel row with `face` set needs `wheel:<id>` in depthOrder
  under `comp:<face>`, and needs no cover).
- `apps/clock/editor/watch-engine.js`: `lay.wheels` entries (`x`, `y`, `r`, `host`,
  `holes`), `lay.subs` (`x`, `y`, `r`, `slide`), `F.wheelDisc`, `liveSeq`.
- `apps/clock/editor/_selftest/bank3-drive.html`: the `?lift=` and `?strip=` blocks (the
  pattern for a design edit before the real export runs).

## Contract

1. New pure method `wheelFaces3(wp, seq)`: for every row of `wp.rows` whose `host` is the id
   of a `sub` entry in `seq`, set `r.face = r.host`. Rows hosted on the dial, on a folded
   face, or on nothing keep no `face` field. `_export3Body` and `livePlan4` call it right
   after `G.wheelPlan3(D.comps)` with `this.face().liveSeq`.
2. Remove the `lifted3` map and its refusal from `_export3Body` (the sentence
   `turns under the face` must not exist in `apps/` or `tools/` afterwards).
3. `wheelPass3(wp, onlyBase)`:
   - a row with `face` set is not measured against `seen` (its face is not in the plate
     renders, so nothing shows it there) and never refuses "read through no cutout". Its
     band is the whole disc, `r0 = 0`, `r1 = R + 1` (R = `w.r * kP`), additionally clipped to
     the host face's disc (`lay.subs` entry for `r.face`: centre and `r` mapped through `kP`
     about 233, plus 1 px; for a slider face the circle `Math.hypot(slide.hl, slide.hh)`),
     so no disc texel lands outside the face that covers it. Disc pixels come from
     `F.wheelDisc` exactly as for other rows; `pivot_x`/`pivot_y` as for other rows.
   - the cover is built only from rows without `face` (their bands); when every row is
     `exposed` or has `face`, the answer is `{ rows, cover: null }`.
   - drums (`r.drum`) with `face` set: keep the drum path as is (its strip and window); just
     do not add its band to the cover.
4. `t3bake` passes the rows through unchanged (`face` travels to the baker), and
   `depthOrder3` (step 3) already lists `wheel:<id>` under `comp:<face>`.
5. `bank3-drive.html`, after the `?lift=` block and before `?strip=`:
   - `?add=<URL-encoded JSON array of comps>`: append those comps to the mounted design's
     `comps` (after the existing ones, or right after the comp whose id is given as
     `after` on an added comp, the field then removed), say `added <ids>`.
   - `?set=<URL-encoded JSON object { compId: { field: value } }>`: `Object.assign` each
     onto the named comp, say `set <ids>`.
   - `?order=<comma-separated ids>`: set the design's `order` to that list (entries
     `comp:<id>` allowed), say `order <list>`.
   Each, when present, does `IT.faces = {}`, `IT.buildMovement()` and the same settle check
   as `?lift=` (throw `'did not settle after the ' + name` when it does not). Applied in the
   order add, set, order. The page's existing parameters keep working unchanged.

## Behaviour and edge cases (checks in `headless-subdial.test.js`)

- `wheelFaces3`: Triple calendar with `c5` set `cadence: 'smooth'` (a turning moon) and
  `order: ['h2', 'comp:c4', 'h3', 'h4']`: `wheelPlan3` has a row `c5` and `wheelFaces3` sets
  its `face` to `'c4'`; without the `order` (the face folds) `face` is absent; a ring wheel
  hosted on the dial never gets `face`.
- `depthOrder3(seq, [], rows)` for that lifted case lists `wheel:c5` right under `comp:c4`
  and above `h2`.
- The refusal sentence is gone from the source; `_export3Body`'s source has no `lifted3`.
- Baker end to end on synthetic planes (as in step 1's writer checks): a still row for `c4`,
  a `face: 'c4'` wheel row and no cover, `depthOrder` from `depthOrder3` → `bakeClock3`
  does not throw and the wheel's order sits between `h2`'s and the still row's.
- `wheelPass3` under the node stub canvas: if the stub lets it run (it returns zero pixels),
  check that a lone `face` row does not refuse and returns `cover: null`; if the stub cannot
  run it at all, say so in the report and skip only that check.
- `bank3-drive.html`: a source check (read it with `fs`) that the three parameter names are
  parsed and that each path calls `buildMovement` and the settle check.

## Acceptance criteria

1. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-subdial.test.js` → exit 0, `all green`.
2. `set -o pipefail; export PATH=/home/greg/.local/share/nodeenv-gorb/bin:$PATH; cd apps/clock/editor/_selftest && node headless-test.js` → exit 0.
3. `cd apps/clock/editor/_selftest && GORB_HOST='{"kind":"hub"}' NODE_OPTIONS="--require=$PWD/../../../../tools/orb-manager/web/runtime/_selftest/gorb-host-preload.js" node headless-test.js` → exit 0.
4. `cd tools/orb-manager/web/runtime/_selftest && PY=python3 node headless-test.js` → exit 0.
5. `grep -rn "turns under the face" apps tools` → no output.

## Out of scope

- The pointer rows, compPass3, auxPass3 (step 3), the engine's lift verdict (step 2).
- `F.compHoles` (owner's uncommitted work). Leave the existing `headless-wheels.test.js` checks alone unless one asserts behaviour this slice retires; then change that one check minimally and name it in your report.
- Firmware, version files, shelf re-bake, docs.

## Notes

- The face's still row is opaque wherever the face is and open at its cutouts, so a disc
  under it shows only through the cutouts, and hands passing beneath the face are hidden by
  both. That is why the disc is clipped to the face and needs no cover of its own.
