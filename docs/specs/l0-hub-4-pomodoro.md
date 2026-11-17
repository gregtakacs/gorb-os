# L0 hub step 4: the pomodoro sends each owner sound as the orb's own file (design schema 2)

## Goal

Slice L0 (ruling 41), gorb-os step 4 of 4. The pomodoro gets exactly what step 3 gave the
clock: its design document carries each owner upload as one sound file — the orb's own encoding
in a WAV header, `GorbBake.soundFile` (`docs/specs/l0-design.md` §3, binding), made once and kept
verbatim, baked from its stored bytes — instead of an inline sample array, its design schema
rises 1 → 2 with one pure migration step, and the pre-L0 fixture from step 1 opens and saves in
the file form. This step also runs the whole gate, browser gates included, over the slice.

## Read first

- `docs/specs/l0-design.md`, `docs/specs/l0-hub-2-orb-encoding.md`, `docs/specs/l0-hub-3-clock.md`
  (the clock's version of this step: copy its shape).
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `GorbBake.soundFile`,
  `soundFileUrl`, `soundRead`, `soundPcm`, and the sound stage's `coded` items (step 2).
- `apps/pomodoro/editor/pomodoro-bake.js` — where `bake` hands its chimes to the sound stage
  (grep `sounds(`/`uploads`): an upload's chime goes in as it is.
- `apps/pomodoro/editor/pomodoro-editor.js` — `pcmOf`/`keep`/`waveOf`/`artOf` (~31–60), the upload
  (~101–124), the chime checks (~138–145), `scheme` copy (~240–248), the app object (~360–370:
  `schema: 1`), `toDoc`/`fromDoc` (~395–420).
- `apps/pomodoro/editor/pomodoro-bake.js` — `UPLOAD_MAX_S` (4.0 s): the pomodoro's own, tighter
  upload bound, which stays.
- `tools/orb-manager/web/runtime/host@1/` — how an app object's `schema` and `migrate: {n: fn}`
  are read (~234–240 of its main file).
- `tools/orb-manager/web/runtime/_selftest/headless-soundpane.test.js` — the suite style, and the
  closest thing to a pomodoro node harness today.
- `apps/pomodoro/editor/_fixtures/schema-1.orbdesign` — the pre-L0 design with an inline sound.

## Contract

- `apps/pomodoro/editor/editor.json` `"schema": 2`; the app object `schema: 2`,
  `migrate: { 1: <step> }`.
- **The step `1 → 2`**: for every entry `u` of `doc.design.uploads` (when it is an array of
  objects), `pcm = GorbBake.soundPcm(u.pcm)`; when it has at least 2 samples,
  `u.wav = GorbBake.soundFileUrl(pcm)` and `u.samples = pcm.length`; always `delete u.pcm`. Nothing
  else in the document changes. Pure, as in step 3.
- **In memory**: `ups[ref]` holds `{ wav, pcm }` — the file string exactly as read or made, and its
  decoded samples (`soundRead(wav).pcm`) for play, checks and readouts — or keep `ups[ref]` as the
  decoded `Int16Array` and a parallel map for the file; either way the file is never re-made from
  the samples. An upload makes its file once: `wav = soundFileUrl(rendered)`, samples decoded from it.
- **Writing** (`toDoc(d, true)`): each upload is `{ name, ref, samples, wav }` with the held `wav`
  verbatim; `toDoc(d, false)` writes `{ name, ref, samples }` as today.
- **Reading** (`fromDoc`): a string `u.wav` that `soundRead` accepts is held verbatim with its
  decoded samples; a legacy `u.pcm` (or an array/object `u.samples`) is encoded once with
  `soundFileUrl`; then the session's held ref as today. The stub and `keep` keep their shape.
- **Bake**: `artOf` / `PomodoroBake.bake` hand an upload's chime to the sound stage with
  `coded: { enc, body }` from `soundRead(wav)` beside the decoded `pcm`, so the theme's plane is the
  stored bytes; generated recipe chimes are unchanged.
- **Fixture**: `apps/pomodoro/editor/_fixtures/schema-2.orbdesign` is the schema-1 fixture opened in
  this editor and saved with the session's document path with art, pretty-printed
  `JSON.stringify(doc, null, 1)`; its upload's `wav` is exactly `soundFileUrl` of the same 2,205 samples (the orb's ADPCM form). Do not
  commit the generator.
- **Ship**: `apps/pomodoro/version.txt` stays v1.8.2 (step 1's bump); `python3
  tools/make_factory_apps.py`; `python3 tools/rebake_shelf.py` for what `--check` calls stale.

### Tests: `tools/orb-manager/web/runtime/_selftest/headless-pomodoro-sound.test.js` (new)

Loads `pomodoro-bake.js` and `pomodoro-editor.js` in node (stub only what they touch; no
AudioContext is needed for these checks). Named PASS lines:
- `the pomodoro reads design schema 2` — app object and `editor.json` agree.
- `a pre-L0 design opens with its sound` — the schema-1 fixture migrated: its upload has
  `wav === soundFileUrl(samples)`, no `pcm`, `samples === 2205`; `fromDoc` holds decoded samples
  equal to `soundRead(wav).pcm`, and the focus chime still names that upload.
- `its next save stores a file` — `toDoc(opened, true)`'s upload carries that same `wav` and no `pcm`.
- `an unchanged sound keeps its bytes across saves` — open, save, open, save: identical `wav`.
- `the bake uses the stored bytes` — the bake of the schema-2 fixture's design puts exactly
  `soundRead(wav).body` in the theme for that chime, with no `soundEncode` for it.
- `the migration step is pure` — twice on copies gives equal results, input unchanged, a design
  with no uploads comes back equal.
- `a legacy pcm still reads` and `an unreadable sound keeps its name` — as in step 3.
- `the bake does not change` — `PomodoroBake.bake` of the opened schema-1 design and of the
  schema-2 fixture's design give identical bytes.

## Acceptance criteria

1. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
2. `cd tools/orb-manager/web/runtime/_selftest && node headless-pomodoro-sound.test.js` → every
   named check PASS, exit 0.
3. `make check-runner check-migrations check-web check-editor-origin` → each green (the last two
   unpack the release web file and drive every editor and the editor origin in Chromium).

## Out of scope

- The clock (done in step 3), `theme@1`, `studio@1` (its sound pane only shows what the app hands
  it), `baker@1`, Orb Manager's Go code, `main/`.
- Changing the pomodoro's 4.0 s upload bound or its refusal sentence.
- Do not modify: `apps/clock/`, `tools/orb-manager/web/runtime/theme@1`,
  `tools/orb-manager/web/runtime/studio@1`, `tools/orb-manager/web/runtime/baker@1`,
  `tools/orb-manager/*.go`, `main/`, `apps/pomodoro/editor/_fixtures/schema-1.orbdesign`.

## Notes

- The pomodoro's `uploads[i].samples` was a length in the stub and could be the samples in a very
  old file (`pcmOf(u.pcm !== undefined ? u.pcm : u.samples)`); `soundPcm` of a number is null,
  so keep reading `u.samples` as a legacy source only when it is an array or object.
