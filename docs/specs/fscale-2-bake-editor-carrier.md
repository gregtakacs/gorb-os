# fscale-2: the design carries the scale, the baker bakes it, the editor shows it

## Goal

Slice fscale (read `docs/specs/fscale-design.md` and `fscale-1` first). fscale-1 made the
firmware draw a scale and read it from a layout-2 (136 B) values carrier, defaulting to a ring
when absent. This step makes the **baker and the design actually carry it**: every flights start
and preset grows a `d.scale`/`d.layout.scale`/`d.words.scale`; the baker always writes the values
row (map or not) at layout 2 with the scale region filled; the editor gets the radar-style Scale
section and the preview draws the ring/bar. `bake_check.py` is updated to layout 2 and to allow
values with no map.

No firmware change (fscale-1 owns `main.c`). `version.txt` untouched (fscale-3).

## Read first

- `docs/specs/fscale-design.md` (the byte table and the "written by flights-bake.js after
  `M().valuesBytes(m)`" rule), `docs/specs/fscale-1-firmware-scale-round.md` (the firmware side).
- `apps/flights/editor/flights-bake.js`: the `STARTS`/`SCOPES`/`FRAMES`/`WORDS` tables (68-233),
  `upgrade1` (256-271), `sizes`/`layout` (724-757), `frameSvg` (759), `pillCanvas` (794), and
  `bake()` (919-969) — especially `var Mp = d.map && d.map.on ? d.map : null; if (Mp) { ... S.values(M().valuesBytes(Mp), M().VALUES_VER); }`
  (957-969).
- `apps/flights/editor/flights-map.js`: `VALUES_VER` (21), `ranges`/`valuesBytes` (91, 143), the
  `GorbMaps` export (196-198). **Do not edit `ranges()` or `valuesBytes()`** — the
  `bare-airports` task owns those bodies; the scale bytes go on in `flights-bake.js` after the
  call, and `VALUES_VER` here moves to 2 (that one line is the map file's only change here).
- `apps/radar/radar-bake.js`: `d.scale`/`d.layout.scale`/`d.words.scale` in presets (~46-51, 75)
  and how `valuesBytes` writes the scale bytes (~1144-1154, `b[1178]`), `ringInk`, `scaleOf`; and
  `apps/radar/main.c` `draw_scale` (526) + `frame()` preview for the look to mirror in the
  flights preview.
- `apps/flights/editor/flights-editor.js`: the Scope/Words step `fields` (204-…), `K.Ch` helpers
  (200-202), `frame`/`drawWords` (98, 131-186).

## Contract

Files to modify:

- `apps/flights/editor/flights-map.js`: `VALUES_VER` 1 -> **2** (one line; nothing else).
- `apps/flights/editor/flights-bake.js`: (a) give **every** start/preset a scale — add a
  `SCALE` look table keyed like `WORDS`, and in `preset`/`upgrade1`/the explicit starts set
  `d.layout = d.layout || {}`, `d.layout.scale = 'ring'|'bar'` (per family; 'bar' for
  sectional/blueprint, 'ring' elsewhere, mirroring radar's choice), `d.scale = {ink, edge, width
  2-8, tick 0-24, max 60-200}` and `d.words.scale = {face, size, colour}` (radar-bake defaults
   are the reference); (b) in `bake()`, build the values row **unconditionally** — always call
   `S.values(bytes, 2)` — where the base 120 B come from `M().valuesBytes(Mp || <a fresh blank
   map preset at d.words.face/d.words.size>)` (map on, else an all-off carrier), **copied into a
   136 B array** and then **bytes 117-135 written** (little-endian, per the design table, over the
   120 B array's old `pad[3]` and the 16 new bytes) from `d.layout.scale`/`d.scale`/`d.words.scale`.
   Do not change `valuesBytes` itself (it stays a 120 B writer); do the extend-and-write in a small
   `scaleBytes` helper in `flights-bake.js`. The font-4 (names) and
   six name rows stay gated on `Mp` as today; only the values row leaves the `if (Mp)`.
- `apps/flights/editor/flights-editor.js`: add the radar-style Scale section to the Scope (or
  Words) step — a Ring/Bar choice bound to `d.layout.scale`, the bar's thickness/ticks/max-length
  and ink/edge fields bound to `d.scale.*`, and the label's size/colour bound to
  `d.words.scale.*` (radar-editor.js:156, 193-199, 213-216 are the pattern); and draw the ring or
  bar into the editor `frame()`/preview at the round length (mirror `radar-bake.js` `frame()`).
- `apps/flights/editor/_selftest/bake_check.py`: `VALUES_B` 120 -> **136**; check 5 — accept a
  values row at layout/src **2** and at 136 B; the `elif vals: bad.append("values with no map")`
  refusal (97) becomes: a theme with no map must STILL carry values of 136 B at src 2 (so the
  scale is present), and `want_fonts` (mapped?6:5) is unchanged (a no-map theme still has no font
  4). Update the `mapped` handling so `mapped=0` themes are checked for a 136 B values row too.

The scale region's byte order and defaults must match `fscale-1` exactly (style 1=ring/0=bar at
byte 117, …, `scale_lbl_argb` at 132-135).

## Acceptance criteria

Run each; all must hold (Chrome and clang required — run in the owner's env):

1. `python3 apps/flights/editor/_selftest/bake_check.py` → exit 0, prints `0 failed` and every
   theme `ok` (including the no-map starts, which now carry a 136 B values row).
2. `test -z "$(git diff --name-only 4e783bf5 -- main apps/radar apps/flights/main.c apps/flights/version.txt)"` → no output (no firmware/OS/other-app drift; `flights-map.js` only its `VALUES_VER` line changed — `git diff 4e783bf5 -- apps/flights/editor/flights-map.js` shows one line).
3. `grep -n 'VALUES_VER = 2' apps/flights/editor/flights-map.js` matches; `grep -n 'VALUES_B = 136' apps/flights/editor/_selftest/bake_check.py` matches; `grep -n 'function scaleBytes\|S.values' apps/flights/editor/flights-bake.js` shows the values row built outside the `if (Mp)`.

## Out of scope

`apps/flights/main.c` (fscale-1), `version.txt` (fscale-3), `flights-map.js`'s `ranges`/
`valuesBytes` bodies (bare-airports), the pill re-bake (fscale-3), any shelf re-bake, other apps.

## Notes

- The `bare-airports` task may edit `flights-map.js` in the same window: the ONLY change this
  step makes there is `VALUES_VER` 1 -> 2. If a merge is needed, keep that one line and land the
  rest in `flights-bake.js`.

## Review amendments

`docs/specs/fscale-design.md` "Orchestrator's review" is binding on this step; where it and this spec disagree, it wins.
