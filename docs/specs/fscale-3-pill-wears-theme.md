# fscale-3: the range pill wears the theme (accent words on the card's frame)

## Goal

Slice fscale, final step (read `docs/specs/fscale-design.md` and fscale-1/fscale-2 first). The
range pill (slot 38) today is grey text (`W.body`) on a plain grey rounded fill (`pillCanvas`
draws only `F.fill`). Ruling §7.5: the pill wears the theme — its words go to the theme's
**accent** tone and its backing becomes the **card's frame** (the same `frames.fill`/line/edge/
style the labels use), not a bare pill. This step is pure bake + the version bump. `apps/flights/`
`version.txt` goes v1.6.3 -> **v1.6.4** here (last step).

No firmware change; the firmware (fscale-1) already draws the pill's round text.

## Read first

- `docs/specs/fscale-design.md` ("The pill wears the theme").
- `apps/flights/editor/flights-bake.js`: `pillCanvas` (794-800), `frameSvg` (759), `panelCanvas`
  (820-823), and `bake()`'s pill rows (977-982) — `font(... W.body ...)` at 979 and
  `plane(pillCanvas(d, PILL_W, ph))` at 982.
- `apps/flights/editor/_selftest/bake_check.py`: the pill check (98-100, "no range pill at 38 in
  font 5").
- `apps/radar/radar-bake.js`/`radar-editor.js` for how the radar's zoom pill takes the theme's
  look (reference only).

## Contract

Files to modify:

- `apps/flights/editor/flights-bake.js`:
  1. `pillCanvas` (794): draw the backing in the **card frame look** — reuse the same path the
     labels use (`frameSvg`/`panelCanvas` style with `F.fill`, `F.line`, `F.edge`, `F.style`,
     `F.opacity`), keeping the pill's rounded silhouette when `F.style === 'rounded'` but adding
     the frame's line/edge so it reads as a small card, not a flat grey lozenge.
  2. The pill font (979): colour the `PILL_FONT` row `W.accent` (the theme's scope green) instead
     of `W.body`. `PILL_SLOT`/`PILL_FONT` slot numbers unchanged (still 38 / font 5).
- `apps/flights/editor/_selftest/bake_check.py`: keep requiring the pill at slot 38 in font 5;
  add that the pill row's frame is present (its plane is non-trivial / its backing carries the
  frame — assert the pill element exists as today and that the theme still passes the readback
  and `orb_clock3.c` projection). Do not weaken the existing checks.
- `apps/flights/version.txt`: `v1.6.3` -> `v1.6.4`.

## Acceptance criteria

Run each; all must hold (Chrome + clang required — run in the owner's env):

1. `python3 apps/flights/editor/_selftest/bake_check.py` → exit 0, every theme `ok`, `0 failed`
   (the pill still at slot 38 in font 5, the design region still carried).
2. `test -z "$(git diff --name-only 4e783bf5 -- main apps/radar apps/flights/main.c apps/flights/editor/flights-map.js)"` → no output.
3. `grep -n 'v1.6.4' apps/flights/version.txt` matches; `git diff 4e783bf5 -- apps/flights/editor/flights-bake.js` shows the `PILL_FONT` font row colouring `W.accent` and `pillCanvas` using the frame (no bare `x.fillStyle = F.fill`-only backing left).

## Out of scope

`main.c` (fscale-1), the scale region/values carrier (fscale-2), `flights-map.js`, other apps,
the OS, any shelf `.orbtheme` re-bake (the orchestrator re-bakes the shelf).

## Notes

- After this step the shelf's `flights-scope.orbtheme` must be re-baked by the orchestrator to
  pick up v1.6.4's layout-2 values and re-styled pill; that re-bake is NOT a step here.

## Review amendments

`docs/specs/fscale-design.md` "Orchestrator's review" is binding on this step; where it and this spec disagree, it wins.
