# Pomodoro editor: the looks, the chimes and the kit's sound pane

The design this chain implements. The looks themselves are already drawn and are not reopened here:
`docs/specs/pomodoro-looks.md` and its working sketch `docs/specs/pomodoro-looks-preview.html` are the
reference (plan §64.9's last line). What is designed here is the **build**: the eight faces as a bake,
the chimes as a kit sound pane, the hosted editor, and the ship.

This withdraws `docs/specs/pomodoro-3-editor-looks.md` and `docs/specs/pomodoro-4-ship.md`. Both were
written before the looks design, before plan §64.8 (the sound pane is the kit's) and §64.9 (every theme
gets an overlay colour), and both say things the shipped code refuses — see §3. Their content is folded
in here, with those things corrected.

**Base.** `EDITOR_BASE` is this chain's base, set by the orchestrator. It is the tip of the `pomodoro`
plumbing chain, so that `apps/pomodoro/{app.json,version.txt,main.c,pomodoro.c,pomodoro.h}` exist, the
app is `"shelf": false` and names no editor. Nothing here works without that: an editor with no app to
theme has nothing to bake into.

## 1. The chain

| # | Step | Lands |
|---|---|---|
| 1 | `pomodoro-editor-1-kit-sound` | `GorbBake.synth` (recipe → PCM), `stage3().sounds()`, `S.Sound` (the pane) in the kit, with a node suite |
| 2 | `pomodoro-editor-2-bake` | `apps/pomodoro/editor/pomodoro-bake.js`, the eight looks + New, `_selftest/`, the PNG and WAV evidence |
| 3 | `pomodoro-editor-3-editor` | `apps/pomodoro/editor/pomodoro-editor.js`, `editor.json`, `editor/_fixtures/schema-1.orbdesign`, `"editor": "editor"` in `app.json` |
| 4 | `pomodoro-editor-4-ship` | `"shelf": true`, the `.orbapp`, the Scope shelf default in `rebake_shelf.py`, the user-guide row |

The split is the one the clock's editor already makes: the bake is a pure module the `_selftest` can run
without the UI, and the editor is a shell over it. Step 1 is first because the pomodoro is the kit pane's
first user (owner, §64.8) and nothing downstream can be checked without it.

## 2. The contract with the app

`apps/pomodoro/main.c` (plumbing chain, step 2) finds the theme by row, and its numbers by values. That
layout is fixed there, not here:

| Class | Slot | Row | Who writes it |
|---|---|---|---|
| plate | 0 | the ground, full panel | the bake |
| plate | 1 | the furniture, one plane per look, masked | the bake |
| text | 0, 1, 2 | the time face, the words face, the small face | the bake |
| aux | 8 | the time `mm:ss` | the app, with the digits it is given |
| aux | 9 | the phase name | the app |
| aux | 10 | the count `3 of 4` | the app |
| aux | 11 | the state line (`Paused`, `Waiting`) | the app |
| aux | 12 | the header caption, or the choice while pressing | the app |
| aux | 13 | the dots: 13 frames, `f` filled for `f = 0..12` | the app |
| sound | 1, 2, 3 | the chimes | the app, by the slot the values name |
| values | 0 | `pm_values_t`, 16 B, `VALUES_VER` 1 | the bake writes, the app reads |

`pm_values_t` (design amendment B): `uint32_t ring_rgb[3]` little-endian `0xRRGGBB`, `uint8_t chime[3]`,
`uint8_t _res`. **`chime[3]` carries the sound slots this design bakes: `1, 2, 3`.** The app's own
defaults, used when there are no values at all, stay `0, 0, 0` — silence, which is right for an app with
no theme. One correction the plumbing chain must carry: `pomodoro-2-app.md:101` says
`orb_sound_play(0, 0, 0)`; amendment D of the same file already says the slot comes from the values, and
slot 0 is refused outright (§3). No step of *this* chain edits `main.c`; the orchestrator moves that
line in the app chain, and `bake_check.py` (§6) is what catches a theme that could not be played.

`fit` is `{rows: 12, cap: 64}` — two plates, three fonts, five labels, the dots row. Values and sounds
are rows too and the cap counts them: 12 + 3 + 1 = 16 against 64 is not close.

## 3. Why the chimes sit at 1, 2 and 3

Slot 0 is not free. Every themed app is loaded by the same projector, and it refuses a sound at slot 0
that is not flagged to loop:

```
main/art/orb_clock3.c:1479  if (slot == CLK_SND_TICK_LOOP && !loop)  reject("the tick loop is not flagged to loop: …")
main/art/orb_clock3.c:1482  if (slot != CLK_SND_TICK_LOOP && loop)   reject("sound slot %u is flagged to loop: …")
main/art/orb_clock3.h:51    CLK_SND_TICK_LOOP = 0
```

Only the tick loop may carry a beat (`orb_clock3.c:1504`), and every sound in a theme must name the same
hour pattern (`:1473`), with `none` the fifth (`theme_schema.HOUR_PATTERNS`, mirrored by `baker@1`'s
`HOUR_PATTERNS`). So a pomodoro theme bakes **one-shot** sounds at slots **1, 2, 3**, each row `kind` =
`none` (4), `flags` 0, `vmax` = `vmin` = 0. `main/audio/orb_sound_app.c:30` plays by slot out of the
app's voice bank, so `orb_sound_play(1|2|3, 0, 0)` is the whole story on the device, and the voice bank
itself is generic: `main/art/orb_theme.c:2278` builds one from any app's sound rows.

Sound planes go in at the **head of the bank**, as `bakeClock3` does (`orbtheme-baker.js:3946`), so every
one lands at an even offset (`orb_clock3.c:1466` refuses an odd one: a 16-bit load would fault); ADPCM
blocks are 132 B and PCM16 planes 2n, so the head stays even however many there are. That moves the
plate, and the plate's offset is a scalar the scheduler paints from directly
(`main/gfx/orb_sched.c:163` seeds the canvas at `blob() + clk_theme.plate_off`; `orb_clock3.c:1079`
refuses a `plate_off` that is not the plate's own plane offset). A bake with sounds must therefore set
`scalars.plate_off` to the head's byte count, which is what `orbtheme-baker.js:3965` does for the clock.
Step 1 makes the kit do it for any app, so no app's bake has to remember it.

The room: a voice bank is capped at 1 MB and a theme at 8 MB less the two table sectors
(`apps/radar/editor/_selftest/bake_check.py:39`), and three chimes of 1.8 s at 22050 Hz ADPCM are ~9.5 KB.
Never a question, and `bake_check.py` refuses an over-budget theme rather than trusting arithmetic in
prose.

## 4. The sound pane is the kit's

Owner, plan §64.8: one sound pane in the kit that any app's editor can offer, like the colour picker
(`studio@1/studio.js`'s `Co`, :641). The clock keeps its own engine until it is moved onto the kit as a
separate job, so `apps/clock/**` is untouched by this chain.

Layering, following `docs/theme-editor-kit-plan.md` (studio = shell, theme = headless services,
baker = format) and what already exists there:

- **`baker@1` owns the format and the waveform.** `GorbBake.soundEncode`, `soundBytes`, `adpcmEncode`
  and `checkClockSounds` are already exported (the list at `:5311`). It gains two:
  `GorbBake.synth(recipe, o)` — a **pure, deterministic** renderer from a recipe table to an
  `Int16Array` at 22050 Hz, high-passed at 350 Hz and peak-normalised — and `stage3().sounds(list, o)`,
  the write path §3 describes: planes at the head, `plate_off` set on the caller's scalars, rows appended
  last (so `elems[0]` stays the plate), refusals run.
- **`studio@1` owns the pane.** `S.Sound(Kit, o)` returns fields: a pick of the generated recipes, the
  owner's upload row (`Kit.Fi`, `accept: 'audio/*'`), a play chip, and a readout of seconds, samples and
  encoded bytes. Decoding a file needs `decodeAudioData` and an `OfflineAudioContext`, so the upload half
  lives with the UI; the *generation* half is pure and lives with the format, where node can test it.
- **The app's bake owns its recipes as data.** The nine recipes below are pomodoro design content; they
  ride in `pomodoro-bake.js` and are handed to the pane. The kit offers the primitives (sine and bell
  partials, an envelope, a band-passed noise burst), not anybody's chimes.

No new runtime id, and no hub Go change. `host@1/page.html`'s script list is a fixed sequence
(`:23-34`), and the only other doors are `{{range .Extras}}` (which `editor.go`'s `hostedExtras` fills for
`maps@1` alone) and `{{range .Modules}}` (the editor's own files). Everything in `tools/orb-manager/web/`
runtime dirs is served as-is, so the two additions above are new exports in two files the page already
loads — which is also how they ride the SDK tarball (`apps/sdk/orbsdk.py`'s `cmd_dist` walks that tree).

## 5. The bake: eight faces, one layout


`apps/pomodoro/editor/pomodoro-bake.js`, shaped as `apps/radar/editor/radar-bake.js` is: `SCHEMA`,
`PRESETS` (the eight and `scope` first, New last as `blank`), `STARTERS`, `STARTER_TITLES`,
`STARTER_NOTES`, `FACES`, `preset(key)`, `complete(d)`, `scheme(d, key)`, `picture(d, px, o)`,
`frame(...)`, `bake(d, o)` and the recipe table `CHIMES`. Grounds, faces and inks are the same-named
look's in radar, weather and flights, taken **by value** — the values are in `pomodoro-looks.md`'s
table, which was read from those files. Two looks sharing a piece of furniture is plan §64.4's failure,
so each look's `frame()` draws its own, translated from `furniture(id, L)` in the sketch.

Four rules the sketch already obeys and the bake must too:

1. **A timer, not a clock.** No hour numerals, no hands, no chapter ring.
2. **No scale that names minutes.** Cardinal pips and unlabelled circles only.
3. **Digits of one width.** The kit's font step already knows how: `font(face, px, {digits: true})` in
   `apps/weather/editor/weather-bake.js:646-675` sizes the cell to the widest figure the set must carry
   and centres each glyph's ink in it. Font 0 is baked that way, and `bake_check.py` asserts it by
   measuring the digit cells in the baked font row: ten frames `0`-`9`, one width, ink centred. This is
   a property of the font bake, not of a face that happens to have tabular figures — Playfair, Cormorant
   and Barlow are all three used, and all three must land at one width.
4. **The rim is the ring's.** Every drawn mark at radius ≤ 200. The check measures it: it walks the
   plate-1 plane's ink and refuses a pixel outside r 200 (the ring's band is 224–231).

Fonts: slot 0 at 96 px (editable 64..144) carrying `0-9 :  %` and nothing else — every other charset
entry an empty frame, which the format allows (`orbtheme-baker.js:3535`); slots 1 and 2 at 30 and 24 px
carrying printable ASCII whole. Labels 8..12 name their fonts through `str`'s tone nibbles, and tone 1
(accent) always names a font, so a break and a choice read in the look's own accent.

The preview (row `preview`, one plane, the whole glass as drawn) comes from `picture()`, the same
function the start sheet tiles and the PNG evidence renders from. A second renderer for the evidence is
the drift this rule exists to prevent.

## 6. The check, and the evidence

`apps/pomodoro/editor/_selftest/bake.html` prints `B64 <name> <ms> <base64>` per theme, `PNG <name>
<base64>` per true-size render and `WAV <name> <base64>` per chime, then `DONE`; `bake_check.py`
(radar's pair in shape) decodes them and gates every theme: the slug, plates `[0, 1]`, fonts `[0, 1, 2]`,
labels at 8..12, the dots row with 13 frames, three sound rows at slots 1, 2, 3 all one-shot and all
`none`, a values row of 16 bytes at version 1 whose `chime` reads `1, 2, 3`, the preview present, the
design region carried, `plate_off` equal to the plate's own plane offset, the whole theme inside the
pool's room, the digit cells one wide, the furniture inside r 200, and the shipped projector accepting
it (`host_clock3_test.run(exe, "--dump3f", path, slug)`, as radar's does) — the gate that proves the slot
choice against the code that will actually refuse it. `POMODORO_BAKE_KEEP=DIR` keeps the themes there (a
second verb keeps the PNGs and WAVs for the evidence run) and `scope.orbtheme` is what the shelf takes.

The looks are judged by eye by the orchestrator, not by a check: nine PNGs (eight looks and blank) at
466 px true size, and one WAV per chime plus its break-end variant.

## 7. The editor

`apps/pomodoro/editor/pomodoro-editor.js`, registered through `OrbHost.register` with radar's key set
(`slug`, `title`, `schema`, `check`, `complete`, `blank`, `templates`, `picture`, `pictureKey`, `steps`,
`parts`, `fields`, `views`, `draw`, `bake`, `fit`), and an `editor.json` of weather's shape with `maps@1`
dropped and `"schema": 1` named — so `editor/_fixtures/schema-1.orbdesign` is owed
(`apps/sdk/orbsdk.py`'s `schema_fixture_rule`) and
`python3 apps/sdk/orbsdk.py check-migrations apps/pomodoro` is the gate that proves it opens.

Steps, as the looks design lists them: **Ground**, **Furniture** (each part can be hidden), **Words**
(three faces, three sizes, inks), **Count** (the dots), **Ring** (three colours, each previewed on the
look's ground *and* over a busy app, so a low-contrast pick is visible before it ships), Chimes (step
1's kit pane). Views reach the drawn frame: the three phases, the four states, `1 of 4` to `4 of 4`, and
the `00:00` of a finished one. The kit does the rest: the start sheet, the fields, the send, the undo
history — `gorb-os-editor-ui-is-kit-level`, and the ban is greppable (no `StartSheet`, no
`showOpenFilePicker`, no bare `type="color"`, no ruler of the editor's own).

**The overlay colour (plan §64.9) is collected and not yet shipped.** `pm_values_t` is full at 16 bytes,
the format-3 scalar table has no free word (`orbtheme-baker.js:50-61`), and nothing on this base reads an
overlay colour off a theme. So the design carries `design.overlay` (a `#rrggbb`, its default the look's
accent — the looks table's first ring colour), the Ring step sets it with `Kit.Co`, and the bake writes it
into the design region only: the OS never reads a design (`orbtheme-baker.js:42`), so the fact is stored
where the next slice can find it and nothing pretends the OS obeys it. Step 3 leaves a one-line
`TODO(§64.9)` in the bake naming the two ways it can ship — a values version 2 spending `_res` plus three
bytes, or a new section scalar — and `bake_check.py` asserts the field is present, so a theme cannot be
baked without the note attached to it.

## 8. Ship

`"shelf": true`, `tools/orb-manager/apps/pomodoro.orbapp` built by `tools/make_factory_apps.py`,
`tools/rebake_shelf.py` gaining three lines (`DEFAULTS["pomodoro"] = "pomodoro-scope.orbtheme"`,
`bake_pomodoro(v)` shaped as `bake_radar(v)` at `:145`, and the `BAKE` entry at `:162`), and one row plus
one paragraph in `docs/user-guide.md`. `version.txt` stays `v1.7.0`; `os_min` stays the literal `v9.9.9`
the orchestrator replaces. No hub Go change: `seed.go` embeds `apps/*.orbapp themes/*.orbtheme` by glob,
and `make check-hub` is run anyway to prove it.

## 9. Owner questions

1. **Three chime slots, or two?** Chosen: three — a focus's end, a break's end and a long break's end each
   take their own row, and the bake shares one plane between two slots naming the same recipe, so an
   author who changes nothing pays for two waves, not three. Values reading `1, 2, 2` is that theme.
2. **Does the long break's chime differ from a short one's?** Chosen: not by default — both are the
   brighter variant. The author can make it differ in the Chimes step, which is what three slots are for.
3. **An upload longer than the longest generated chime (Brass, 1.8 s)?** Chosen: allowed to 4.0 s; the
   pane prints the seconds and the encoded bytes and refuses past that with the reason, as the clock's
   pane names a long upload rather than truncating it (`clock-logic.js`'s `SND_LONG_S`). A pomodoro chime
   is heard across a desk, not studied.
4. **Do the ring colours move out of the values now that §64.9 allows an override?** No. Amendment G put
   the override in the settings, the look's colours stay in the values, the app picks. This chain changes
   neither.

## 10. Out of scope for the whole chain

`main.c`, `pomodoro.c`/`.h` and the core's host suite; `apps/clock/**`; any hub Go; another app's
`PRESETS`, `STARTERS`, editor or shelf file; the real `os_min`; a `stage_tint` import; the kit's
re-baking of the clock onto the new sound pane (§64.8 names that a separate job); and the looks
themselves, which are settled.



## Orchestrator amendments (2026-11-11)

1. Owner questions 1–4: accepted as chosen.
2. **Sequencing.** This chain runs after the ring, the overlay slice and the pomodoro plumbing
   chain have landed. Its base (`EDITOR_BASE`) is a branch holding all three, so step 3 adds the
   **overlay colour** field (the overlay slice's kit field, with the look's accent as default)
   rather than a TODO, and step 4's shipping finds `apps/pomodoro/main.c` and `app.json` in place.
3. The app chain carries the slot fix: chimes at 1–3, and slot 0 means no chime (pomodoro-design
   round 5, K).
