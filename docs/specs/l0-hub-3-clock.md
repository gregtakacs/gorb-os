# L0 hub step 3: the clock sends each owner sound as the orb's own file (design schema 2)

## Goal

Slice L0 (ruling 41), gorb-os step 3 of 4. The clock's design document stops carrying an
owner's sound as an inline sample array and carries it as one sound file — the orb's own
encoding in a WAV header, `GorbBake.soundFile` (`docs/specs/l0-design.md` §3, binding) — so the
site can store it once by hash. The file is made once (at upload, or by the migration step) and
then kept verbatim: re-saving an unchanged sound never re-encodes it, and the bake puts its bytes
in the theme as they are. The clock's
design schema rises 1 → 2 with one pure migration step, so a pre-L0 design (the schema-1
fixture step 1 recorded) opens, plays, bakes the same, and its next save writes the file form.
An upload longer than the site will store is refused in the editor, with its length, at upload.

## Read first

- `docs/specs/l0-design.md` — all of it (§3.1 revised), `docs/specs/l0-hub-1-wav-kit.md` and
  `docs/specs/l0-hub-2-orb-encoding.md` (what steps 1 and 2 built).
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `GorbBake.soundFile`,
  `soundFileUrl`, `soundRead`, `soundPcm`, `SOUND_MAX_S`, `SOUND_MAX_SAMPLES`, and the sound
  stage's `coded` items (step 2).
- `apps/clock/editor/clock-logic.js` — where the bake hands the theme its sounds (grep `sounds:`
  and the sound-for-slot reader ~780–815: `if (id === 'upload') return d.up[slot].pcm`, the tick
  loop made from an upload with `SE.loopFromTick`).
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
  (when `doc.sounds` and `doc.sounds.up` are objects), `pcm = GorbBake.soundPcm(e.pcm)`; when it
  has at least 2 samples, set `e.wav = GorbBake.soundFileUrl(pcm)`; always `delete e.pcm`. Every
  other field (`name`, `loop`, `rate`, `first`, the rest of the document) is untouched. Pure: no
  state, no DOM, no clock; a document without `sounds.up` comes back equal.
- **In memory** each upload entry is `{ ...fields, pcm, wav }`: `wav` the file string exactly as
  it was read or made, `pcm` its decoded samples (`soundRead(wav).pcm`, what the orb will play) for
  play, readouts and anything derived. An upload makes its file once: `wav = soundFileUrl(rendered)`,
  `pcm = soundRead(wav).pcm`.
- **Writing** (`designContent(true)` and therefore `toDoc(d, true)`): every `sounds.up.<slot>` is
  the entry without `pcm`, carrying its held `wav` string verbatim — never re-made from `pcm`.
  `designContent(false)` keeps writing `up: {}` as today. Host snapshots (`sndSnap`/`upSig`) never
  carry `wav` or `pcm`; the held entries stay in `ups` by ref as today.
- **Reading**: wherever a document's sounds come into the editor — `clock-host.js` `upsOf` (used by
  `fromDoc`) and `clock-logic.js` `applyDesignContent` — each entry with a string `wav` that
  `soundRead` accepts becomes `{ ...entry, wav, pcm: decoded }`; an entry with only a legacy `pcm`
  (array or `{"0":…}`) becomes `{ ...entry without pcm, wav: soundFileUrl(samples), pcm: soundRead(that).pcm }`
  (encoded once, like the migration); an entry neither reads keeps its name with `pcm: null`, no
  `wav`, and behaves like today's missing upload (the slot plays and bakes its built-in sound).
- **Bake**: where the theme plays an upload as it is (a chime slot, a recorded loop with
  `loop: true`), the sound handed to the baker carries `coded: { enc, body }` from
  `soundRead(entry.wav)` beside the decoded `pcm`, so its plane is the stored bytes. Where the clock
  derives a clip from an upload (a tick loop built by `SE.loopFromTick` from a one-tick upload), it
  derives from the decoded `pcm` and hands no `coded`.
- **Upload bound**: in `sndUpload`, after the samples are rendered, a sound longer than
  `GorbBake.SOUND_MAX_SAMPLES` (10 s) is not taken: `sndMsg` is exactly
  `` `${f.name} is ${secs.toFixed(1)} s: a sound can be at most ${GorbBake.SOUND_MAX_S.toFixed(1)} s, so it was not taken` ``
  and the design is unchanged. The `SND_LONG_S` "long sound" warning (also 10 s) can no longer
  fire, so remove that clause from the upload message (and `SND_LONG_S` if nothing else reads it).
  The message's `KB in the theme` figure is the file's body length.
- **Fixture**: `apps/clock/editor/_fixtures/schema-2.orbdesign` is
  `_fixtures/schema-1.orbdesign` opened in this step's editor (so migrated) and saved with
  `designDoc(true)`, pretty-printed `JSON.stringify(doc, null, 1)`. Its `sounds.up.chime_hour.wav`
  is exactly `soundFileUrl` of the schema-1 fixture's 2,205 samples (the orb's ADPCM form). Do not commit the generator.
- **Ship** (ruling 39): `apps/clock/version.txt` stays v1.8.1 (step 1 bumped it past the tag);
  `python3 tools/make_factory_apps.py`, and `python3 tools/rebake_shelf.py` for whatever
  `--check` calls stale.

### Tests: `apps/clock/editor/_selftest/headless-l0-sound.test.js` (new, the existing suites' style; `headless-test.js` picks it up)

Named checks, each a PASS line:
- `the clock reads design schema 2` — the session's schema and `editor.json` agree on 2.
- `a pre-L0 design opens with its sound` — the committed schema-1 fixture through the session's
  open path: the migrated document has `sounds.up.chime_hour.wav === soundFileUrl(samples)` and no
  `pcm`; after `fromDoc`, the held upload's `pcm` equals `soundRead(wav).pcm`.
- `its next save stores a file` — `toDoc(opened, true)` carries that same `wav` string, no `pcm`
  anywhere under `sounds.up`.
- `an unchanged sound keeps its bytes across saves` — open the schema-2 fixture, save, open the
  saved document, save again: the `wav` string is identical all three times (no re-encode).
- `the bake uses the stored bytes` — the bake of the schema-2 fixture puts exactly
  `soundRead(wav).body` in the theme for `chime_hour` (read the plane back, or spy on the sound
  stage's input), with no `soundEncode` call for it.
- `the migration step is pure` — running it twice on copies gives equal results, the input is
  unchanged, a document with no `sounds` comes back equal.
- `a legacy pcm in a schema-2 file still reads` — an `up` entry carrying `pcm` (array) and no
  `wav` reads to `wav === soundFileUrl(samples)`.
- `an unreadable sound keeps its name` — `wav: 'data:audio/wav;base64,AAAA'` reads to `pcm: null`
  and the slot bakes its built-in sound, no exception.
- `a 10.1 s upload is refused with its length` — `sndUpload` with a stubbed decode giving 222,705
  samples leaves the design unchanged and sets the exact `sndMsg`
  (`… is 10.1 s: a sound can be at most 10.0 s, so it was not taken`); 10.0 s is taken.
- `the bake does not change` — the schema-1 fixture's bake (opened, so migrated) and the schema-2
  fixture's bake produce the same theme bytes.

## Behaviour and edge cases

- An `Undo` back to a state with an upload, and the host snapshots (`upSig`), behave as before:
  they never see `wav`, only the in-memory `pcm`.
- A tick loop upload (`loop: true`, `rate`, `first`) round-trips with those fields intact.
- The fixture's sound is a 1,248-byte ADPCM file (60-byte header + 9 blocks), above theme@1's
  512-byte ref floor, so a save under the site's host sends it as one asset.

## Acceptance criteria

1. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
2. `cd apps/clock/editor/_selftest && node headless-test.js` → `all N suites green`, and
   `node headless-l0-sound.test.js` prints each named check above as PASS.
3. `make check-migrations` → clock opens schema-1 and schema-2 fixtures; `make check-runner`
   green.

## Out of scope

- The pomodoro (step 4), `theme@1`, `studio@1`, `baker@1` (step 2's API is used, not changed),
  Orb Manager's Go code, `main/`.
- Trimming, resampling or re-encoding a sound; ADPCM files; any new UI beyond the refusal line.
- Do not modify: `apps/pomodoro/`, `tools/orb-manager/web/runtime/`, `tools/orb-manager/*.go`,
  `main/`, `apps/clock/editor/_fixtures/schema-1.orbdesign`.

## Notes

- design.md ruling 21: the schema alone decides what an editor may open. That is why this is a
  schema bump and not "read both": an older clock would open a schema-2 file and play silence.
- `SoundEngine.SR` and `GorbBake.SOUND_RATE` are both 22,050; samples go into `soundFileUrl`
  unchanged.
- Owner's steer (l0-design §5.5): never store PCM when ADPCM qualifies (that is `soundFile`'s
  choice, not the clock's), and never write a second ADPCM encoder.
