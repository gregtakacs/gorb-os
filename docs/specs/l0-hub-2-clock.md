# L0 hub step 2: the clock sends each owner sound as a WAV file (design schema 2)

## Goal

Slice L0 (ruling 41), gorb-os step 2 of 3. The clock's design document stops carrying an
owner's sound as an inline sample array and carries it as one canonical WAV `data:` URL
(`docs/specs/l0-design.md` §3, binding), so the site can store it once by hash. The clock's
design schema rises 1 → 2 with one pure migration step, so a pre-L0 design (the schema-1
fixture step 1 recorded) opens, plays, bakes the same, and its next save writes the file form.
An upload longer than the site will store is refused in the editor, with its length, at upload.

## Read first

- `docs/specs/l0-design.md` — all of it, and `docs/specs/l0-hub-1-wav-kit.md` (what step 1 built).
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `GorbBake.soundWavUrl`,
  `soundFromWav`, `soundPcm`, `SOUND_MAX_S`, `SOUND_MAX_SAMPLES` (step 1).
- `apps/clock/editor/clock-logic.js` — `DESIGN_SCHEMA`/`DESIGN_MIGRATE` and their comment
  (~3390–3402, the rules for a migration step), `designContent` (~3405), `applyDesignContent`
  (~3424), the `session` getter (~636–655), `sndDesign`/`sndUpd` (~750–800), `sndUpload`
  (~900–945), `SND_LONG_S` (~824), the sound pane (~4270–4340).
- `apps/clock/editor/clock-host.js` — the app object (~331: `schema: 1`, `migrate: {}`),
  `pcmOf`/`upsOf` (~114–133), `toDoc`/`fromDoc` (~346–361), `sndSnap`/`upSig` (~93–103).
- `apps/clock/editor/editor.json` (`"schema": 1`).
- `apps/clock/editor/_selftest/headless-host.test.js` (~80–130) and
  `apps/clock/editor/_selftest/headless-sound.test.js` — test style and how they drive uploads.
- `tools/orb-manager/web/runtime/theme@1/session.js` ~684–710 — `api.migrate` and the open
  path: steps run on a deep copy, one schema at a time, the result stamped `schema: n+1`.
- `apps/clock/editor/_fixtures/schema-1.orbdesign` — the pre-L0 design with an inline sound.

## Contract

- `apps/clock/editor/editor.json`: `"schema": 2`. `clock-logic.js` `DESIGN_SCHEMA = 2`.
  `clock-host.js`'s app object `schema: 2`.
- **One** migration step, `1 → 2`, defined once and used by both sessions (the class's
  `DESIGN_MIGRATE` and `clock-host.js`'s `migrate`): for every entry `e` of `doc.sounds.up`
  (when `doc.sounds` and `doc.sounds.up` are objects), `pcm = GorbBake.soundPcm(e.pcm)`; when
  it is non-null, set `e.wav = GorbBake.soundWavUrl(pcm)`; always `delete e.pcm`. Every other
  field (`name`, `loop`, `rate`, `first`, the rest of the document) is untouched. Pure: no
  state, no DOM, no clock; a document without `sounds.up` comes back equal.
- **Writing** (`designContent(true)` and therefore `toDoc(d, true)`): every `sounds.up.<slot>`
  is `{ ...entry without pcm, wav: soundWavUrl(entry.pcm) }`. `designContent(false)` keeps
  writing `up: {}` as today.
- **Reading**: wherever a document's sounds come into the editor — `clock-host.js` `upsOf`
  (used by `fromDoc`) and `clock-logic.js` `applyDesignContent` — each `up` entry becomes
  `{ ...entry without wav, pcm: GorbBake.soundPcm(entry.wav !== undefined ? entry.wav : entry.pcm) }`,
  so the in-memory form stays `pcm: Int16Array` and nothing downstream (play, bake, readouts,
  fingerprints) changes. An entry whose samples cannot be read keeps its name with `pcm: null`
  and behaves like today's missing upload (the slot plays and bakes its built-in sound).
- **Upload bound**: in `sndUpload`, after the samples are rendered, a sound longer than
  `GorbBake.SOUND_MAX_SAMPLES` is not taken: `sndMsg` is exactly
  `` `${f.name} is ${secs.toFixed(1)} s: a sound can be at most ${GorbBake.SOUND_MAX_S.toFixed(1)} s, so it was not taken` ``
  and the design is unchanged. The `SND_LONG_S` (10 s) warning stays as it is.
- **Fixture**: `apps/clock/editor/_fixtures/schema-2.orbdesign` is
  `_fixtures/schema-1.orbdesign` opened in this step's editor (so migrated) and saved with
  `designDoc(true)`, pretty-printed `JSON.stringify(doc, null, 1)`. Its `sounds.up.chime_hour.wav`
  decodes to the schema-1 fixture's 2,205 samples exactly. Do not commit the generator.
- **Ship** (ruling 39): `apps/clock/version.txt` stays v1.8.1 (step 1 bumped it past the tag);
  `python3 tools/make_factory_apps.py`, and `python3 tools/rebake_shelf.py` for whatever
  `--check` calls stale.

### Tests: `apps/clock/editor/_selftest/headless-l0-sound.test.js` (new, the existing suites' style; `headless-test.js` picks it up)

Named checks, each a PASS line:
- `the clock reads design schema 2` — the session's schema and `editor.json` agree on 2.
- `a pre-L0 design opens with its sound` — the committed schema-1 fixture through the session's
  open path: the migrated document has `sounds.up.chime_hour.wav` starting `data:audio/wav;base64,`
  and no `pcm`; after `fromDoc`, the held upload's `pcm` is an `Int16Array` equal to the fixture's
  2,205 samples.
- `its next save stores a file` — `toDoc(opened, true)` of that design has the `wav` URL, no
  `pcm` anywhere under `sounds.up`, and the `wav` equals `soundWavUrl` of the same samples.
- `the migration step is pure` — running it twice on copies gives equal results, the input is
  unchanged, a document with no `sounds` comes back equal.
- `a legacy pcm in a schema-2 file still reads` — an `up` entry carrying `pcm` (array) and no
  `wav` reads to the same samples.
- `an unreadable sound keeps its name` — `wav: 'data:audio/wav;base64,AAAA'` reads to `pcm: null`
  and the slot bakes its built-in sound, no exception.
- `a 20.1 s upload is refused with its length` — `sndUpload` with a stubbed decode giving
  443,205 samples leaves the design unchanged and sets the exact `sndMsg` above
  (`… is 20.1 s: a sound can be at most 20.0 s, so it was not taken`); 20.0 s is taken.
- `the bake does not change` — the schema-1 fixture's bake (through the clock's bake path) and
  the schema-2 fixture's bake produce the same theme bytes.

## Behaviour and edge cases

- An `Undo` back to a state with an upload, and the host snapshots (`upSig`), behave as before:
  they never see `wav`, only the in-memory `pcm`.
- A tick loop upload (`loop: true`, `rate`, `first`) round-trips with those fields intact.
- The fixture's sound is 4,454 WAV bytes, above theme@1's 512-byte ref floor, so a save under the
  site's host sends it as one asset.

## Acceptance criteria

1. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
2. `cd apps/clock/editor/_selftest && node headless-test.js` → `all N suites green`, and
   `node headless-l0-sound.test.js` prints each named check above as PASS.
3. `make check-migrations` → clock opens schema-1 and schema-2 fixtures; `make check-runner`
   green.

## Out of scope

- The pomodoro (step 3), `theme@1`, `studio@1`, `baker@1` (step 1's API is used, not changed),
  Orb Manager's Go code, `main/`.
- Trimming, resampling or re-encoding a sound; ADPCM files; any new UI beyond the refusal line.
- Do not modify: `apps/pomodoro/`, `tools/orb-manager/web/runtime/`, `tools/orb-manager/*.go`,
  `main/`, `apps/clock/editor/_fixtures/schema-1.orbdesign`.

## Notes

- design.md ruling 21: the schema alone decides what an editor may open. That is why this is a
  schema bump and not "read both": an older clock would open a schema-2 file and play silence.
- `SoundEngine.SR` and `GorbBake.SOUND_RATE` are both 22,050; samples go into `soundWavUrl`
  unchanged.
