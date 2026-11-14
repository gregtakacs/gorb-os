# pomodoro-3: the editor and the eight looks, judged at true size

> **Withdrawn 2026-11-11** in favour of the `pomodoro-editor` chain (`docs/specs/pomodoro-editor-design.md`,
> `docs/specs/pomodoro-editor-1-kit-sound.md` … `-4-ship.md`). Its furniture, row layout and evidence
> protocol are folded in there; its chime row is corrected — plan §64.8 moved the sound pane into the kit,
> and `main/art/orb_clock3.c:1479` refuses a one-shot sound at slot 0, which this file asked for.

## Goal

The pomodoro's face, as a theme: `apps/pomodoro/editor/` — the bake (`pomodoro-bake.js`), the hosted
editor (`pomodoro-editor.js`), the eight looks in design §11's order with their own furniture, one
sample design for the schema, and a `bake_check.py` that bakes all nine in headless Chromium and
gates each one. This step also produces the PNGs the orchestrator judges by eye. Slice **pomodoro**
of `docs/specs/pomodoro-design.md`.

## Read first

- `docs/specs/pomodoro-design.md` §10 (the rows, the tones, the legibility numbers, why the face has
  no live arc), §11 (the looks table: grounds, faces, inks, furniture), the Owner questions (the settled choices).
- `apps/radar/editor/radar-bake.js` — `PRESETS` (:39-211, the palettes this step takes its grounds,
  faces and inks from), `STARTERS`/`STARTER_TITLES`/`STARTER_NOTES` (:214-219), the `starter()`
  builder over Scope, `bake()` with its `sounds:` argument, and `FACES`.
- `apps/radar/editor/radar-editor.js` — the registration object (:226-260) and `OrbHost.register`
  (:260); `apps/radar/editor/editor.json`; `apps/weather/editor/editor.json` for the shape with a
  `schema`.
- `tools/orb-manager/web/runtime/host@1/host.js` (:10-20 the contract, :601-620 the start sheet) and
  `studio@1/studio.js` `S.StartFlow` (:252) — what the kit already does, so that nothing here
  re-does it (`gorb-os-editor-ui-is-kit-level`).
- `docs/theme-editor-kit-plan.md` §8.2.
- `apps/radar/editor/_selftest/bake.html` and `bake_check.py` — the file pair this step copies in
  shape: what it stages, the `B64 <name> <ms> <base64>` protocol, and every gate it runs on the
  bytes it gets back.
- `tools/theme_schema.py` (`CLASS_*`, `SHAPE_TEXT`, `FONT_CHARSET`, the label fields), `tools/
  orbchrome.py`, `tools/orbbundle.py`, `tools/host_label_test.py` (how a label's box is composed).
- `docs/app-design-rules.md` "Looks"; §64.4.

## Contract

Create:

- `apps/pomodoro/editor/editor.json` —
  `{"baker": 3, "editor": 1, "host": 1, "module": ["pomodoro-bake.js", "pomodoro-editor.js"],
  "runtime": ["baker@1", "fonts@1", "host@1", "react@18.3.1", "studio@1", "theme@1", "ui@1"],
  "schema": 1}`: radar's runtime list without `maps@1`, which the pomodoro has no use for, plus the
  `schema` weather's editor.json names (a hosted editor that names one owes `editor/_fixtures`).
- `apps/pomodoro/editor/pomodoro-bake.js`, exposing `PomodoroBake` with `SCHEMA`, `PRESETS`,
  `STARTERS` (`['scope','paper','brass','blueprint','station','celestial','silver','sectional']` —
  **Brass's key is `brass`**, not radar's legacy `vintage`, §64.4), `STARTER_TITLES`,
  `STARTER_NOTES`, `FACES`, `preset(key)`, `complete(d)`, `scheme(d, key)`, `picture(d, px, o)`,
  `frame(...)`, `bake(d, o)` and the built-in chime generator `chimePcm()`.
- `apps/pomodoro/editor/pomodoro-editor.js`, registering `slug: 'pomodoro'`, `title: 'Pomodoro'`,
  `schema`, `check`, `complete`, `blank`, the eight `templates` **in §11's order**, `picture`,
  `steps` (`ground`, `dial`, `count`, `words`, `ring`), `parts`, `fields`, `views`, `draw`, `bake`,
  `fit: {rows: 11, cap: 64}`, ending `if (root.OrbHost) root.OrbHost.register(app);`.
- `apps/pomodoro/editor/_fixtures/schema-1.orbdesign` — one design of schema 1, produced by
  `python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro --save-current`, not hand-written.
- `apps/pomodoro/editor/_selftest/bake.html` and `_selftest/bake_check.py`, honouring
  `POMODORO_BAKE_KEEP=DIR` and writing `scope.orbtheme` there.
- `$DELEGATE_EVIDENCE_DIR/looks/<key>.png` for all eight and `blank`, and
  `$DELEGATE_EVIDENCE_DIR/states/<state>.png` for Scope running / paused / waiting / choosing / bare.

Modify `apps/pomodoro/app.json`: add `"editor": "editor"` and `"os_min": "v9.9.9"` (design §13 —
that string is a placeholder the orchestrator replaces, and nothing in this step may "fix" it).

The theme's layout this step bakes, row for row (design §10.1): one plate at slot 0, the ground with the furniture folded in (never a second plate: slot 1 is the OS's glass, painted over every row here); fonts 0 (96 px,
editable 64..144), 1 (30 px) and 2 (24 px), each carrying `theme_schema.FONT_CHARSET`'s printable
ASCII as the schema requires; labels at slots 8 (the time), 9 (the phase), 10 (the count), 11 (the
state) and 12 (the header) with `str` naming their fonts by tone nibbles — tone 1 (accent) always
naming a font, so a break and a choice read in the look's own accent; and the dots row at slot 13,
an art row of **13 frames**, `f` filled dots for `f = 0..12`, each dot 10 px with a 6 px gap.
Values: none in this first schema — the face has no author-tuned numbers beyond fonts and the
furniture, so the theme carries no VALUES row (and `main.c` never calls `orb_stage_values`). A
schema-1 design that later needs them raises the schema by one and adds the migration step, per
apps/README "Theme designs, schemas and migrations".

## Behaviour and edge cases

- **Every look is its own.** Nothing is copied between looks but the row *layout*: each of the eight
  has its own ground, its own faces, its own inks and its own furniture, and each look's palette,
  fonts and grounds are the same-named look's in `radar-bake.js` (the values in design §11's table,
  which were read from there). Two looks sharing a piece of furniture is the failure §64.4 names.
- **New (blank)** carries every row and no furniture: no chapter marks, no rules, plain greys. An
  author must be able to reach a finished look from it and must not be shown someone else's first.
- The chime is baked into every look's sound table at slot 0 through the baker's `sounds:`
  argument, from `chimePcm()`: 16-bit mono 22050 Hz, ~0.6 s, nothing below 350 Hz (the micro
  speaker's floor), encoded ADPCM by `baker@1`'s own `soundEncode`. A look may not be silent.
- `bake_check.py` refuses, per theme: a slug that is not `pomodoro`, plates other than `[0, 1]`,
  fonts other than `[0, 1, 2]`, labels not at 8..12, a dots row with other than 13 frames, a missing
  preview, no design region, a theme over the pool's budget (radar's own `BUDGET` arithmetic), and a
  theme the shipped projector refuses (`host_clock3_test.run(exe, "--dump3f", …)`, as radar does).
- The editor's `views` reach the drawn frame: `phase` and `state` change what the labels say, and
  `count` moves the dots, so an author sees the four states the app can be in — including the
  `00:00` of a finished one. `fit()` returns 11 rows against the cap of 64 and the host shows it.
- The kit does the rest. Grep the editor for the ban: no `StartSheet`, no `showOpenFilePicker`, no
  `type="color"`, no `<canvas>` calibration ruler of its own.

## Acceptance criteria

1. `test -f apps/pomodoro/editor/editor.json -a -f apps/pomodoro/editor/pomodoro-bake.js -a -f
   apps/pomodoro/editor/pomodoro-editor.js -a -f apps/pomodoro/editor/_fixtures/schema-1.orbdesign
   -a -f apps/pomodoro/editor/_selftest/bake.html -a -f apps/pomodoro/editor/_selftest/bake_check.py`
   and `grep -q 'OrbHost.register' apps/pomodoro/editor/pomodoro-editor.js` and
   `grep -q '\"schema\": 1' apps/pomodoro/editor/editor.json`.
2. The eight, in order, and no `vintage` (the command the chain's verify runs, copied verbatim):
   `awk '/STARTERS/{line=line $0} END{exit (line ~ /scope.*paper.*brass.*blueprint.*station.*celestial.*silver.*sectional/ && line !~ /vintage/) ? 0 : 1}' apps/pomodoro/editor/pomodoro-bake.js`
   → exit 0. Against `apps/radar/editor/radar-bake.js` the same command exits 1, which is the point:
   run it there once and report it.
3. `grep -q '\"editor\": \"editor\"' apps/pomodoro/app.json && grep -q '\"os_min\": \"v9.9.9\"' apps/pomodoro/app.json`.
4. `ORB_CHROME=/usr/bin/chromium python3 apps/pomodoro/editor/_selftest/bake_check.py` → exit 0,
   nine themes baked and none failed (this is the step's own `_selftest`, imitating radar's).
5. `ORB_CHROME=/usr/bin/chromium python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro` → exit 0.
6. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → exit 0 (the app now carries an editor
   region; the container is still not on the shelf), and `make check-editor` → exit 0.
7. **The art is judged by eye, not by a check**: `$DELEGATE_EVIDENCE_DIR/looks/` holds one PNG per
   look and for blank (nine), each a 466×466 true-size render at 177 dpi (144 px/in, so the PNG is
   saved with its pHYs chunk saying 7087 px/m — `orbchrome`'s page can set it with a canvas-to-blob
   pass; if the writer cannot set pHYs, name the scale in the file name, e.g. `scope@466.png`, and
   say so), and `$DELEGATE_EVIDENCE_DIR/states/` holds Scope's five. The verify fails if the files
   are not there: `test $(ls $DELEGATE_EVIDENCE_DIR/looks/*.png | wc -l) -ge 9`.
   `bake.html` must therefore emit, besides the `B64` theme lines, `PNG <name> <base64>` lines that
   `bake_check.py` decodes and writes — the same protocol, one more verb.
8. `python3 tools/host_pomodoro_test.py` still exits 0: no core change belongs in this step.

## Out of scope

`main.c`, `pomodoro.c`/`.h`, the shelf (`"shelf"` stays false), `tools/rebake_shelf.py`,
`tools/orb-manager/themes/`, `docs/user-guide.md`, `apps/sdk/`, `main/`. No other app's `PRESETS`,
no change to radar's `vintage` key, no new runtime piece in `tools/orb-manager/web/runtime/` — if a
field the pomodoro wants is missing from the kit, use what the kit has and put the gap in your
summary under SUGGESTIONS, not in the runtime.

## Notes

- The row layout is fixed by pomodoro-2's `find_rows()`: tags 8..13 mean what §10.1 says. If a look
  genuinely cannot live with a row, the core and the app both move, and this step is the wrong place
  to decide that alone.
- `picture(d, px, o)` is what the start sheet tiles and the preview both draw, so the states' PNGs
  come from the same code the hub shows; do not write a second renderer for the evidence.
- Keep the furniture one `frame()` and one `picture()` call each, as radar's does, so the preview,
  the bake and the PNGs cannot drift apart.
