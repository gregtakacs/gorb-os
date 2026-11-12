# pomodoro-editor-1: the kit's sound pane, and a theme's sound rows for any app

## Goal

Give the editor kit a sound pane, so an app's editor can offer generated chimes and the owner's own upload
the way it already offers a colour (`studio@1`'s `Co`). Three pieces: a pure recipe synthesiser in the
format layer, a generic sound-row write path on `stage3`, and the pane that puts them in front of an
author. The pomodoro (step 2) is its first user. Slice **pomodoro-editor** of
`docs/specs/pomodoro-editor-design.md` §3–§4; the owner's ruling is `docs/app-platform-plan.md` §64.8.

## Read first

- `docs/specs/pomodoro-editor-design.md` §3 (slots 1, 2, 3 and `plate_off`, with the refusals quoted) and
  §4 (who owns which half). Those two sections are this step's contract.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `soundEncode`/`soundBytes` (:2209-2217), the
  ADPCM constants (:2178) and `ADPCM_MIN_SNR_DB` (:2186), `stage3` (:3491) and what it returns (:3821), the
  clock's own sound write path (:3946-3965, :4287-4303) as the model for the generic one,
  `checkClockSounds` (:3379), and the export list (:5311).
- `main/art/orb_clock3.c:1439-1533` — the refusals. The kit refuses for the same reasons, in the same words
  where a sentence can be shared; the device is the ground truth being imitated.
- `apps/clock/editor/sound-engine.js` — `SR`, `HP`, the second-order Butterworth high-pass run forward then
  backward (:328-352), `finish` (:369), the recipe builders. **Port what you need; do not edit the file and
  do not import it.** Apps are atomic, which is §64.8's own reason.
- `apps/clock/editor/clock-logic.js` — `sndUpload` (:900) and the sound pane's fields (:4270-4305): how an
  upload is decoded, resampled and named, and how a long one is called out.
- `tools/orb-manager/web/runtime/studio@1/studio.js` — `S.fieldKit` (:579), the row builders (:605-675) and
  how each renders (:690-720). `Fi` (:662) is already a file row, `Ch` (:620) already a row of buttons.
- `tools/orb-manager/web/runtime/_selftest/headless-stage.test.js`, `headless-orbtheme.test.js` — the suite
  shape, and how a suite loads the baker in node.
- `apps/clock/editor/_selftest/headless-sound.test.js` — that suite's subject is the clock's own engine, and
  it is the closest thing in the repo to the tests to write here (it lives in the clock's `_selftest`, and
  `headless-test.js` in each directory globs its own).

## Contract

Modify `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` and
`tools/orb-manager/web/runtime/studio@1/studio.js`; create
`tools/orb-manager/web/runtime/_selftest/headless-soundpane.test.js`. Nothing else: the hosted page already
loads both runtime files (`host@1/page.html:23-34`), so there is no new runtime id, no `editor.json`
change and no Go change.

**`GorbBake.synth(rec, o)` → `Int16Array` at 22050 Hz.** Pure and deterministic — no `Math.random`, no Web
Audio, no `Date`. `rec` is data:

```js
{ name: 'sonar', label: 'Sonar ping', dur: 1.2,
  events: [ { t: 0.00, kind: 'tone', f: 1200, dur: 0.9, gain: 1.0, attack: 0.004, type: 'sine' },
            { t: 0.18, kind: 'tone', f: 1200, dur: 0.6, gain: 0.35 } ],
  brighter: { f_mul: 1.25, gain_mul: 0.9, events: [ /* extra events, same shape */ ] } }
```

- `kind`: `'tone'` (one oscillator of `type`, exponential decay), `'bell'` (partials at `f × ratios[]`, each
  with its own decay; ratios need not be harmonic), `'click'` (a noise burst through a band-pass at `f`,
  width `q`), `'beep'` (a soft-edged square: odd harmonics to the 7th, 8 ms edges).
- Noise comes from a per-event seeded LCG (`seed = 0x5eed + eventIndex`), so a re-bake is byte-identical.
- Events are summed. `o = {variant, hp, peak, max_s}`: `variant: 'brighter'` applies `rec.brighter`; `hp`
  defaults 350, `peak` 0.92, `max_s` 4.
- The sum is high-passed (2nd-order Butterworth at `hp`, forward then backward — a one-shot, so no seam
  rule applies), peak-normalised to `peak` and clipped to `Int16`. Past `max_s` it throws
  `the <name> chime is X.Y s: an app's chime is at most 4.0 s`.

**`stage3().sounds(list, o)`**, on the object `stage3` already returns (:3821).
`list = [{slot: 1..12, pcm: Int16Array, name: 'focus'}]`, `o = {pattern: 'none'}`. In order: encode each
with `soundEncode`; `put` the bytes at the bank **head**, before any pixel plane; remember the count;
append the SOUND rows **last** — `n_frames` 1, `kind` the pattern index, `src` the encoding, `flags` 0,
`vmax` = `vmin` = 0; run `checkClockSounds`. Two slots whose `pcm` is byte-equal share one plane (one
`put`, two rows naming it). Answers `{bytes, rows, enc: [...]}`, and `finish()` merges
`plate_off: <bytes>` into the caller's scalars unless the caller set `plate_off` itself — a bake that
forgets it paints PCM as its plate (`main/gfx/orb_sched.c:163`). Refusals carry the device's sentences,
including the one that refuses a non-looping sound at slot 0.

**`S.Sound(Kit, o)` → an array of field rows.** `Kit` is the object from `S.fieldKit(…)`; `o`:

```js
{ recipes: [rec, …],              // the app's own, built with GorbBake
  slots: [ {key: 'focus', label: 'Focus ends', silent: false}, … ],
  get(slotKey),                   // → {rec: 'sonar'} | {upload: true} | null
  set(slotKey, sel),
  uploads: {focus: {name, pcm}},
  upload(slotKey, file),          // → Promise<{name, pcm}>
  play(pcm, name), stop() }       // the app's playback, so the kit opens no AudioContext
```

Per slot: a heading (`Kit.H`), a `Sl` of the recipes with `Yours · <file name>` after the first upload and
`Silent` only where the slot allows it, a `Ch` row of `Play` and `Stop`, an `Fi` row accepting
`'audio/*'`, and an `Rd` readout — seconds, samples, and the ADPCM bytes those samples will take
(`GorbBake.soundBytes(GorbBake.soundEncode(pcm).enc, pcm.length)`). `upload` decodes with
`decodeAudioData`, resamples through `OfflineAudioContext(1, n, 22050)` and hands back `{name, pcm}`: the
clock's `sndUpload` path without its tick-loop beat logic, which is the clock's and stays there. It
refuses an upload over 4.0 s with the seconds named, and reports a warning row — never a refusal — when
peak-normalising would clip.

## Behaviour and edge cases

- **Determinism**: `synth(rec)` twice in one process, and in two, gives identical bytes. A chime is design
  content, and a bake that drifts re-ships themes nobody changed.
- **The floor**: everything below 350 Hz at least 40 dB under the clip's own RMS. The suite measures it
  with a naive DFT at 100, 200 and 300 Hz, for every recipe and both variants.
- A recipe whose top partial passes 8 kHz is rendered and reported, not truncated: the DAC runs at 22050.
- `sounds([])` answers `{bytes: 0, rows: 0, enc: []}`, leaves `plate_off` alone and writes no row — four
  apps bake today with no sounds and must keep producing the bytes they produce now.
- A repeated **slot** in `list` is refused before the bank moves; a repeated **pcm** is not an error, it
  shares a plane.
- `synth` throws, naming the recipe, on an unknown `kind`, a negative `t`, an empty `events`, or a `dur`
  of 0 or less. The pane lets it through to the author.
- An upload that decodes to 0 samples, to fewer than 2 (the device refuses a plane under 2 samples,
  `orb_clock3.c:1462`), or that is not audio, is refused with a sentence an author can act on.
- The generic path calls the exported `checkClockSounds` rather than restating its rules, so a change to
  one door moves both.

## Acceptance criteria

1. `grep -q 'synth: synth' tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js && grep -q 'sounds: sounds' tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js && grep -q 'S.Sound = ' tools/orb-manager/web/runtime/studio@1/studio.js` → 0.
2. `command -v node` then `cd tools/orb-manager/web/runtime/_selftest && node headless-test.js sound` → 0,
   printing the new suite's name among those it ran.
3. `cd tools/orb-manager/web/runtime/_selftest && node headless-test.js` → 0: every existing kit suite
   still green, which is the proof the additions are additive.
4. `make check-editor` → 0 (it runs that directory, the clock's own suite, and
   `tools/verify_js_bake.py --rows`).
5. `headless-soundpane.test.js` contains, and names in its output, a case for each bullet above: determinism
   across two runs, the 350 Hz floor, the ADPCM round trip at `ADPCM_MIN_SNR_DB`, `sounds([])`, the shared
   plane, the slot-0 refusal, and the `plate_off` merge proven by unpacking a finished bake
   (`unpackBlob3`: the plate row's sprite offset equals the `plate_off` scalar).
6. `git diff --name-only | grep -vE '(baker@1/orbtheme-baker.js|studio@1/studio.js|_selftest/headless-soundpane.test.js)$'`
   prints nothing → the step touched only its three files.

## Out of scope

Every `apps/**` file — the pomodoro's recipes are step 2, the clock's engine is not this job — plus the
hub's Go (`runtime.go`'s catalog, `editor.go`'s `hostedExtras`), `host@1/page.html`, `theme@1`, `ui@1`,
`support@1`, `tools/theme_schema.py` and the firmware. No `sound-engine.js` in the kit, no WAV in the repo,
no audio in any committed theme.

## Notes

- The clock keeps its own copy of the high-pass. Two implementations of one filter is the price of the
  owner's atomicity ruling; §64.8 names moving the clock onto the kit as its own job, and this step makes
  that job possible without doing it.
- Node has no `AudioContext`, which is exactly why the generator is pure JS and the browser API lives in
  the pane. Do not fix the suite by shimming `OfflineAudioContext`.
- Order matters and cannot be asserted from inside `sounds()`: it must run **before** the bake's first
  `row()`. Say so in the comment at its definition, and prove it in step 2's `bake_check.py` instead.
- `stage3().put` already appends and returns the sprite index; the head write is three lines against it.
  The rows go through the same `elems` array the clock path pushes to, so `finish`'s `elems[0]`-is-the-plate
  assumption keeps holding.

