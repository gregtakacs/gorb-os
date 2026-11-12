# pomodoro-editor-2: the bake — eight timers, their chimes, and the check that gates them

## Goal

`apps/pomodoro/editor/pomodoro-bake.js`: the design document's shape, the eight looks and New, the rows
the app finds, the values the app reads, and the chimes it plays — plus `_selftest/`, the headless pair
that bakes all nine and gates every one, and the PNG and WAV evidence the orchestrator judges by eye.
After this step the pomodoro's face exists as bytes; it has no editor UI yet. Slice **pomodoro-editor** of
`docs/specs/pomodoro-editor-design.md` §5–§6.

## Read first

- `docs/specs/pomodoro-looks.md` — the four rules, the looks table (grounds, faces, inks, furniture, header
  caption, dots, ring colours) and the chime table. Binding; do not improve it.
- `docs/specs/pomodoro-looks-preview.html` — the working sketch: `LOOKS` (the palette values to copy),
  `furniture(id, L)` (each look's parts, one function each — that is the shape to translate),
  `fixedTime()` (rule 3 in canvas), `flapTiles()`, `dots()`, `drawRing()`, and `RECIPES` (the chimes in
  WebAudio, which become step 1's recipe data).
- `docs/specs/pomodoro-editor-design.md` §2 (the row table and `pm_values_t`), §3 (slots 1, 2, 3), §5, §6.
- `apps/pomodoro/pomodoro.h` and `apps/pomodoro/main.c` on this base — its row lookup (radar's
  `find_rows()` at `apps/radar/main.c:974` is the shape), `pm_values_t`, the tones it sends. The app is
  written already; the bake serves it.
- `docs/specs/pomodoro-editor-1-kit-sound.md` and the two files it changed — you call `GorbBake.synth` and
  `St.sounds(...)`; both are additive and both already tested.
- `apps/radar/editor/radar-bake.js` — `PRESETS`/`STARTERS`/`FACES`, `starter()` over Scope, `picture()`,
  `frame()`, `bake()` (:647-700) and how it labels its words (`tones()`, the `str` nibbles, `digit`, `end`).
- `apps/weather/editor/weather-bake.js:640-700` — `font(face, px, {digits: true})`, rule 3's mechanism.
- `apps/radar/editor/_selftest/bake.html` and `bake_check.py` — the protocol (`B64 <name> <ms> <b64>`,
  `DONE`), the staging, `BUDGET` (:39), the projector gate and the `…_BAKE_KEEP` contract.
- `tools/theme_schema.py` (`CLASS_*`, `SHAPE_TEXT`, `FONT_CHARSET`, `ALIGNS`, `TONES`), `tools/orbchrome.py`,
  `tools/orbbundle.py`, `tools/host_clock3_test.py` (`run(exe, "--dump3f", …)`).
- `docs/app-design-rules.md` "Looks"; `docs/app-platform-plan.md` §64.4 (no two looks sharing furniture).

## Contract

Create exactly:

- `apps/pomodoro/editor/pomodoro-bake.js`, exposing `PomodoroBake` with `SCHEMA` (= 1), `G` (466), `CX`,
  `FACES`, `CHIMES` (the nine recipes in step 1's data shape, keys `sonar bell shipbell beep platform bowl
  click cabin tone`, labelled as the looks table names them), `PRESETS`, `STARTERS`,
  `STARTER_TITLES`, `STARTER_NOTES`, `preset(key)`, `blank()`, `complete(d)`, `scheme(d, key)`,
  `frame(d, ctx, part)` , `picture(d, px, o)`, `bake(d, o)` and `valuesBytes(d)`. `STARTERS` is
  `['scope','paper','brass','blueprint','station','celestial','silver','sectional']` — **Brass's key is
  `brass`**, radar's legacy `vintage` appears nowhere (§64.4). Ends with the same
  `root.PomodoroBake = …` export the other bakes use and is loadable in node without a DOM (`bake()` and
  `picture()` need a canvas; the tables and `valuesBytes` must not).
- `apps/pomodoro/editor/_selftest/bake.html` and `apps/pomodoro/editor/_selftest/bake_check.py`.

The design document (`format: 'pm-design'`, `schema: 1`, `app: 'pomodoro'`, `appVersion` from
`apps/pomodoro/version.txt`, `made: {baker: 3, editor: 1}` and a `design` object) — `design`'s fields:

| field | type | what it is |
|---|---|---|
| `look` | key | one of the eight, or `blank` |
| `px` | int 64..144 | font 0's size, default 96 |
| `face`, `bodyFace`, `quietFace` | `FACES` keys | the three faces |
| `ink`, `accent`, `quiet` | `#rrggbb` | the three tones' inks |
| `parts` | `{graticule: 1, …}` | the look's furniture parts, each hideable |
| `dots` | `rounds｜square｜stud｜square2｜disc｜star｜ring｜chart` | the dots' shape |
| `ring` | `{focus, short, long}` | three `#rrggbb`, the looks table's |
| `overlay` | `#rrggbb` | §64.9, design region only — see Notes |
| `chimes` | `{focus: sel, short: sel, long: sel}`, `sel = {rec}｜{upload: 1}` | which chime each phase end plays |
| `uploads` | `{focus: {name, pcm}}` | the owner's own wave, samples as a plain array |

The rows `bake()` writes, in this order, with the kit's `stage3`:

1. `St.sounds(...)` **first** — slots 1 (focus ends), 2 (break ends), 3 (long break ends), each from
   `design.chimes`, generated with `GorbBake.synth` or taken from `design.uploads`; a slot whose selection
   equals another's shares its plane; `valuesBytes(d)` writes `chime = [1, 2, 3]`.
2. plate 0 — the ground: the look's gradient, stars, paper or enamel, full panel, `dither: true`.
3. plate 1 — the furniture, masked, `alpha: 255`: what `furniture(id, L)` draws, ink only inside r 200.
4. text 0 — `design.px` (default 96), **`{digits: true}`**, carrying `0-9`, `:`, ` ` and `%`; every other
   `FONT_CHARSET` entry an empty plane.
5. text 1 and text 2 at 30 and 24 px, printable ASCII whole.
6. `St.values(valuesBytes(d), 1)`.
7. `St.preview(...)` — `picture(d, 466, …)`, the whole glass as drawn.
8. aux 8..12 as labels — the five, with `str` naming tone 0 = font 0 for the time, tone 1 (accent) always
   naming a font, `digit`/`end` as radar's do, and the boxes the looks table implies.
9. aux 13 — the dots, an art row of **13 frames**, frame `f` drawing `f` filled dots of 10 px at 6 px
   gaps, in the look's shape and colours, hollow the rest.

`fit` is `{rows: 12, cap: 64}`. `picture(d, px, o)` takes `o.view = {phase, state, count, choosing, other}`
so one function serves the preview, the start sheet, the editor's views and the evidence.

## Behaviour and edge cases

- **Every look is its own.** Nothing is shared between looks but the row *layout*. `bake_check.py` proves it
  rather than trusting the reading: it decodes plate 1 of all nine and refuses any two looks whose furniture
  plane is byte-identical, and refuses a `frame()` that ignores `design.parts` — hiding a part must change
  the bytes.
- **Rule 4 is measured, not asked**: for each look, every opaque pixel of plate 1 lies within r 200 of the
  centre (the ring's band is 224–231). Blank's plate 1 is fully transparent, which passes.
- **Rule 3 is measured**: from the baked font-0 row the check reads the ten digit frames and refuses a
  differing cell width, or ink whose horizontal centre is more than 1 px off the cell's. Run it for at least
  one serif (Playfair or Cormorant) and one grotesque (Barlow), so a face with natural tabulars cannot hide
  a face without them.
- **No look is silent**: three sound rows or the check fails. `Silent` is not offered here.
- Values: exactly 16 bytes, version 1, `ring_rgb` little-endian `0xRRGGBB`, `chime == [1, 2, 3]`, and the
  three colours equal to the design's `ring`. Only `blank` may carry `0xD62828`/`0x0FA36B`/`0x2E93E4` — the
  app's no-theme defaults, which are not a look.
- The device's door: `host_clock3_test.run(exe, "--dump3f", path, 'pomodoro')` must accept every theme. That
  is the gate which would catch a chime at slot 0, an odd plane offset or a `plate_off` left at 0.
- Budget: `BUDGET` as radar's (:39); the chimes add ~10 KB and the check prints each theme's meter.
- `design.overlay` is present in all nine, defaulting to the look's accent.
- The same design baked twice answers the same bytes (sounds, fonts and dither are fixed by the design and by
  `o.serial`/`o.bakedS`, which the check pins). Report each theme's size.
- A design with `parts` all off is legal: plate, words and dots remain.
- **The upload round trip.** `bake()` is given the same design twice: once with `uploads.focus.pcm` as an
  `Int16Array`, once after `JSON.parse(JSON.stringify(design))`, where it is a plain array (or the object of
  indices JSON writes for a typed array). Both must bake, byte-identical, and the chime's plane must not be
  silence. `bake_check.py` prints this case by name (`roundtrip`), because a silent chime is the one failure
  no one notices until the device is on a desk.
- `bake()` refuses, with the reason, for: a `look` outside the nine, a `px` outside 64..144, a `chimes` entry
  naming no recipe in `CHIMES`, an `{upload: 1}` with no wave in `uploads`, and an upload past 4.0 s. Each
  refusal is a case in `bake_check.py` — fed through `?only=` and expected as a `FAIL <key> <reason>` line —
  not merely a branch.

## Acceptance criteria

1. `test -f apps/pomodoro/editor/pomodoro-bake.js -a -f apps/pomodoro/editor/_selftest/bake.html -a -f
   apps/pomodoro/editor/_selftest/bake_check.py && grep -q 'root.PomodoroBake'
   apps/pomodoro/editor/pomodoro-bake.js` → 0.
2. `awk '/STARTERS/{line=line $0} END{exit (line ~ /scope.*paper.*brass.*blueprint.*station.*celestial.*silver.*sectional/ && line !~ /vintage/) ? 0 : 1}' apps/pomodoro/editor/pomodoro-bake.js`
   → 0; the same command against `apps/radar/editor/radar-bake.js` → 1. Run it there once and report it.
3. `ORB_CHROME=$(command -v chromium) POMODORO_EVIDENCE=$DELEGATE_EVIDENCE_DIR python3
   apps/pomodoro/editor/_selftest/bake_check.py` → exit 0, nine themes gated, none failed, printing the
   measured digit-cell width, the largest furniture radius and each theme's bytes.
4. `test $(ls $DELEGATE_EVIDENCE_DIR/looks/*.png | wc -l) -ge 9` and
   `test $(ls $DELEGATE_EVIDENCE_DIR/wavs/*.wav | wc -l) -ge 18` and
   `python3 -c "import glob,PIL.Image as I;i=I.open(sorted(glob.glob('$DELEGATE_EVIDENCE_DIR/looks/*.png'))[0]);assert i.size==(466,466) and abs(i.info['dpi'][0]-177)<2"` → 0.
5. In that run, the line `roundtrip ok` (or the suite's own words for it) appears, and `grep -c roundtrip
   apps/pomodoro/editor/_selftest/bake_check.py` ≥ 1.
6. `POMODORO_BAKE_KEEP=/tmp/pmkeep ORB_CHROME=$(command -v chromium) python3
   apps/pomodoro/editor/_selftest/bake_check.py && ls /tmp/pmkeep | sort` → nine `.orbtheme`, one of them
   `scope.orbtheme`.
7. `ORB_CLANG=/usr/lib/llvm-21/bin/clang make check-apps` → 0: the app names no editor yet and stays off the
   shelf, so this step only adds files the build does not read.
8. `python3 tools/host_pomodoro_test.py` → 0 — no core change belongs here.
9. `git diff --name-only` lists only `apps/pomodoro/editor/` files and `docs/specs/`.

## Out of scope

`editor.json`, `pomodoro-editor.js` and the schema fixture (step 3 — until they exist `app.json` names no
`"editor"`). No `main.c` or `pomodoro.h` change, no `"shelf": true`, no `tools/rebake_shelf.py` line, no hub
Go, no other app's `PRESETS`/`FACES`, and no change to step 1's two kit files: if the pane or the bake path
lacks something you need, work around it inside the bake and report the gap under SUGGESTIONS.

## Notes

- `design.overlay` rides the design region because there is nowhere else for it yet: the values struct is
  full at 16 bytes, the scalar table has no free word, and nothing on this base reads an overlay colour off a
  theme. Mark it in `bake()` with
  `// TODO(§64.9): the overlay colour ships when the OS reads one — values ver 2, or a new scalar.`
  Do not invent a row for it and do not widen `pm_values_t`: the app reads 16 bytes and no step here edits
  the app.
- `picture()` is the only renderer. The preview row, the start-sheet tile, the editor's views and the nine
  PNGs come from it, so the thing judged by eye is the thing that ships.
- One `frame()` call and one `picture()` call per piece of furniture, as radar's: a part drawn twice is a
  part that cannot be hidden from one place.
- The palettes are copied **by value** from `pomodoro-looks.md`'s table (which was read from radar, weather
  and flights when it was written). Do not import another app's bake.
- `bake_check.py` starts a real Chromium; expect about a minute for nine themes with fonts and chimes. Give
  it a timeout you set, and stop only the pid you started.
- The uploaded wave arrives from the browser as an `Int16Array`. A design document must round-trip it: JSON
  writes a typed array as `{"0": v, …}`, which reads back as silence sample by sample — the clock already
  tripped on that (`clock-host.js:116-127`'s `pcmOf`). Store a plain array, and accept both shapes on read.

