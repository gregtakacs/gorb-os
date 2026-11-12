# pomodoro-editor-3: the editor — six steps, five states, and the schema's first fixture

## Goal

`apps/pomodoro/editor/pomodoro-editor.js`: the hosted editor an author opens at the hub's `/editor/…` and
sees one of the eight looks on the glass, the six steps down the left and their fields on the right, with
the kit's sound pane offering the chimes. Plus `editor.json`, the schema-1 fixture, and `app.json` naming
the editor so the hub offers it. After this step a design can be made, changed, saved, reopened and baked
from the browser; the bytes are already step 2's. Slice **pomodoro-editor** of
`docs/specs/pomodoro-editor-design.md` §7.

## Read first

- `docs/specs/pomodoro-editor-design.md` §7, and §2 for what the baked theme must end up carrying.
- `docs/specs/pomodoro-looks.md` "What the editor offers" — the six steps and the views, as asked for.
- `apps/radar/editor/radar-editor.js` — the registration object (:227-260) and `root.OrbHost.register(app)`
  (:260): `slug`, `title`, `schema`, `check`, `complete`, `blank`, `templates`, `picture`, `pictureKey`,
  `steps`, `parts`, `fields`, `views`, `draw`, `bake`, `fit`. Radar is the closest app: a themed face, five
  steps, one art row. `apps/weather/editor/weather-editor.js` for a schema-naming hosted editor.
- `tools/orb-manager/web/runtime/host@1/host.js:10-20` and `:601-620` — the registration contract and what
  the start sheet asks an editor for. `studio@1/studio.js`'s `S.StartFlow` (:252), `S.fieldKit` (:579),
  `S.Fields` (:727) and `S.Sound` (step 1). The kit does the shell: nothing here re-does it
  (`gorb-os-editor-ui-is-kit-level`).
- `apps/pomodoro/editor/pomodoro-bake.js` and `_selftest/bake.html` — step 2's, already gated. Every field
  the editor offers must already be a design field the bake reads; the editor writes a design and calls the
  bake, and nothing in between.
- `apps/clock/editor/clock-host.js:91-145` — how an editor keeps an uploaded wave out of its undo history
  and back on reopen (the `upRef`/`upSig`/`pcmOf` pattern). The pomodoro's Chimes step has the same problem
  and should copy the answer.
- `apps/sdk/orbsdk.py`'s `editor_schema` (:422), `fixtures_dir` (:501), `schema_fixture_rule` (:512) and
  `cmd_check_migrations` — what a hosted editor naming `"schema": 1` owes, and the command that proves it.
- `tools/orb-manager/web/runtime/_runner/migrate.js` — the runner that opens a fixture.

## Contract

Create:

- `apps/pomodoro/editor/editor.json` —
  `{"baker": 3, "editor": 1, "host": 1, "module": ["pomodoro-bake.js", "pomodoro-editor.js"],`
  `"runtime": ["baker@1", "fonts@1", "host@1", "react@18.3.1", "studio@1", "theme@1", "ui@1"], "schema": 1}`
  — radar's runtime list without `maps@1`, with weather's `schema`. `schema` 1 means a fixture is owed.
- `apps/pomodoro/editor/pomodoro-editor.js`, registering
  `{slug: 'pomodoro', title: 'Pomodoro', schema: 1, check, complete, blank, templates, picture,
  pictureKey, steps, parts, fields, views, draw, bake, fit: {rows: 12, cap: 64}}`, ending
  `if (root.OrbHost) root.OrbHost.register(app);`. `blank` is `{note, make: () => PomodoroBake.blank()}` (host.js:16-17), `templates` is
  `PomodoroBake.STARTERS` in that order, so the start sheet shows Scope first and Sectional last.
  `parts` is the design's furniture parts, named per look, each hideable; the step is a `S.Layers` list
  through `S.layerRows`, as radar's rows are.
- `apps/pomodoro/editor/_fixtures/schema-1.orbdesign`, written by
  `python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro --save-current <path>` — never hand-written,
  and it must be a real look (Scope), not the blank.

Modify `apps/pomodoro/app.json`: add `"editor": "editor"`. Nothing else in it — `"shelf"` stays `false`,
`"os_min"` stays the literal `"v9.9.9"`, `"caps"` keep `["ring", "sound"]` (the plumbing chain put `sound`
there because the theme carries chimes; if it is missing on the base, add it and say so in the summary).

Steps and what each offers:

| step | fields |
|---|---|
| Ground | `look` (a `Sw` of the eight and New, through `Kit.Sw`), `px` (64..144), the palette the look brings |
| Furniture | one `Ch`/`Sw` toggle per part in `parts`, plus a note naming the rule it obeys (no minute scale, nothing past r 200) |
| Words | `face`, `bodyFace`, `quietFace` (`Sl` over `PomodoroBake.FACES`), `ink`, `accent`, `quiet` (`Kit.Co`, the kit's picker only) |
| Count | `dots` (the shape), the dots' colours, and a `Cv` sample the app's `draw` uses |
| Ring | `ring.focus`, `ring.short`, `ring.long` (`Kit.Co`), each previewed twice — on the look's ground and over a busy app — and `overlay` (the §64.9 field, `Kit.Co`, with the one-line comment naming the TODO) |
| Chimes | `S.Sound(Kit, …)` over `PomodoroBake.CHIMES` for the three phase ends, with the upload wired to `PomodoroBake`'s `uploads` field |

`views` is the kit's own mechanism: `phase` (focus, short, long), `state` (running, paused, waiting,
choosing), `count` (1..4 of 4) and `other` (the ring seen over someone else's app). `draw` calls
`PomodoroBake.picture(d, px, {view: …})`, so a view change redraws the glass rather than a second renderer.
`bake(d, o)` calls `PomodoroBake.bake`.

## Behaviour and edge cases

- **The ban list is the point.** No `StartSheet` of its own, no `showOpenFilePicker`, no bare
  `type="color"` outside `Kit.Co`, no calibration ruler, no hand-rolled field row, no second picture
  renderer. `grep` for them in this step's own acceptance.
- **`check(d)` refuses what `bake()` would refuse**, before the author sends: a `look` outside the nine, a
  `px` outside 64..144, a phase end with no chime, an upload selected with no wave behind it. The host
  prints `check`'s sentence in its own bar, so it must read as advice and not as a stack trace.
- **Every field round-trips.** A design saved from the editor, reopened and baked, answers the same bytes as
  the first bake — including a look whose chime is an upload, which is the case that fails silently (an
  `Int16Array` coming back from JSON as an object of indices is a chime of silence; see
  `clock-host.js:116-127`). Step 2's `roundtrip` case is the proof; this step must not break it by writing a
  design shape the bake does not read.
- **An upload is not undo history.** The wave itself stays out of the snapshots the host keeps, held by
  reference exactly as the clock's host does, and the readout shows the file name, the seconds and the
  encoded bytes. Undo back to an uploaded chime must still play it.
- The first open with nothing stored is the kit's start sheet, not a blank canvas: `blank()` answers a
  complete Scope design, and New answers the plain-greys face with every row and no furniture.
- `fit` answers 12 rows against the cap of 64 and the host shows the number; the row cost note is
  `gorb-os-row-cost`.
- Choosing a look resets its own defaults (furniture, dots shape, ring colours, chime, overlay) and leaves
  the author's face and size choices alone — a look is a starting point, not a lock.
- The choosing state is reachable from the editor: `view.choosing` draws the press-and-turn text on the
  header row (AUX 12), which is what the app will do, per §10.1's amendment 4.
- The Chimes step never offers `Silent`: an app whose theme carries no chime cannot chime, and amendment D
  promises one.

## Acceptance criteria

1. `test -f apps/pomodoro/editor/editor.json -a -f apps/pomodoro/editor/pomodoro-editor.js -a -f
   apps/pomodoro/editor/_fixtures/schema-1.orbdesign` and `grep -q 'OrbHost.register'
   apps/pomodoro/editor/pomodoro-editor.js` and `grep -q '"schema": 1'
   apps/pomodoro/editor/editor.json` → 0.
2. `grep -q '"editor": "editor"' apps/pomodoro/app.json && grep -q '"shelf": false' apps/pomodoro/app.json
   && grep -q '"os_min": "v9.9.9"' apps/pomodoro/app.json` → 0.
3. `python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro` → 0: the fixture opens in the packed editor
   under the runner, which is the only way a schema-1 fixture can be proved.
4. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → 0 — the app now carries an editor region, packed
   and validated, and is still not on the shelf.
5. `make check-editor` → 0.
6. `! grep -nE 'StartSheet|showOpenFilePicker|<input[^>]*type="color"' apps/pomodoro/editor/pomodoro-editor.js`
   → 0, and `grep -c 'S.Sound' apps/pomodoro/editor/pomodoro-editor.js` ≥ 1.
7. `POMODORO_BAKE_KEEP=/tmp/pmkeep3 ORB_CHROME=$(command -v chromium) python3
   apps/pomodoro/editor/_selftest/bake_check.py` → 0, and its output still names the `roundtrip` case:
   adding an editor must not disturb the bake it sits beside.
8. `$DELEGATE_EVIDENCE_DIR/states/` holds Scope's five states (running, paused, waiting, choosing, bare) and
   each of the eight looks' Chimes-step field list as printed text, and
   `test $(ls $DELEGATE_EVIDENCE_DIR/states/*.png | wc -l) -ge 5` → 0. They come from `picture()` through
   `bake.html`, never from a second renderer.
9. `git diff --name-only` lists only `apps/pomodoro/editor/`, `apps/pomodoro/app.json` and `docs/specs/`.

## Out of scope

`"shelf": true`, `tools/rebake_shelf.py`, `tools/make_factory_apps.py`, `docs/user-guide.md` (step 4); the
kit's two files (step 1) and the bake's tables (step 2); `main.c`, `pomodoro.c/.h`, `version.txt`,
`apps/clock/**`, another app's editor; and any new runtime file or runtime id. `make check-hub` and
`make check-web` need Go, which may not be on PATH: run them if `command -v go` answers, and say which you
ran — the orchestrator has Go and will run the pair anyway.

## Notes

- The editor is 100 % a shell. If a field you want does not exist in `S.fieldKit`, express it with the rows
  that are there (`Cv` draws, `Ch` toggles, `Rd` reports) and put the gap in your summary under SUGGESTIONS
  rather than adding to the runtime: that is the ruling behind `gorb-os-editor-ui-is-kit-level`.
- `parts` is per look, and the eight sets are different words: the same list on eight looks is §64.4's
  failure wearing a good face. Take them from `pomodoro-bake.js`'s furniture, not from the prose.
- The Ring step's second preview (over a busy app) is `picture(d, px, {other: true})` — step 2's flag, which
  draws the look's glass under a mock app's words. A colour that reads only on its own ground is the mistake
  this step exists to make visible before it ships.
- Keep `overlay` out of the picture: it is collected, stored and shown as a swatch with its note. Nothing
  on this base would draw it, and a preview that draws a colour the OS ignores is worse than a swatch.

