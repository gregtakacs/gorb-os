# L0 hub step 1: GorbBake writes and reads a sound as a WAV file; the real pre-L0 fixtures

*Amended by `l0-hub-2-orb-encoding.md` after the owner's 2026-11-17 steer (l0-design §5.5): the PCM-only writer and the 20 s bound below were replaced; the fixtures stand.*

## Goal

Slice L0 of orb-os-net's `docs/design.md` §12 (ruling 41), gorb-os step 1 of 3. Editors are
about to send each owner's sound as one canonical WAV `data:` URL instead of an inline sample
array (see `docs/specs/l0-design.md`, which is binding). This step adds the one shared writer and
reader to the baker runtime, with its bounds, and — **before any editor changes** — records what
today's editors write when a design carries an owner's sound, as each app's `schema-1` fixture.
Steps 2 and 3 move the clock and the pomodoro onto it; the site's tests reuse these fixtures.

## Read first

- `docs/specs/l0-design.md` — all of it. §3.1 is the byte format, §2 the bounds.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — `SOUND_RATE` (~2173),
  `soundEncode`/`soundBytes` (~2209–2217), and the `root.GorbBake = { … }` export block
  (~5564–5660). The new functions go next to the sound ones and into that block.
- `tools/orb-manager/web/runtime/_selftest/headless-soundpane.test.js` — the suite style to
  copy (`ok`, a PASS/FAIL line per check, the final `N checks` line, exit code).
- `tools/orb-manager/web/runtime/theme@1/session.js` ~186–270 — `assetOf`/`b64`/`refify`:
  how a `data:` URL of ≥ 512 decoded bytes becomes an `orb-asset:` ref. Do not change it.
- `apps/clock/editor/clock-host.js` ~114–133 and ~346–361 (`pcmOf`, `upsOf`, `toDoc`,
  `fromDoc`), `apps/clock/editor/clock-logic.js` `sndUpload` (~900–940), `designContent`
  (~3405–3412), `sndUpd`. `apps/clock/editor/_selftest/headless-host.test.js` ~110–130 sets an
  upload directly with `ctl.sndUpd(...)`: that is how to give the clock a sound without
  `decodeAudioData`.
- `apps/pomodoro/editor/pomodoro-editor.js` ~31–60 (`pcmOf`, `keep`, `waveOf`), ~101–124 (the
  upload), ~395–420 (`toDoc`/`fromDoc`), ~360–370 (the app object, `schema: 1`).
- `apps/clock/editor/_fixtures/schema-1.orbdesign`, `apps/pomodoro/editor/_fixtures/schema-1.orbdesign`
  — today's fixtures (no sounds). `apps/sdk/orbsdk.py` ~400–500 — fixture naming
  (`schema-<n>.orbdesign`, one per schema).
- `tools/check_app_versions.py` (docstring), `tools/make_factory_apps.py` (docstring),
  `tools/rebake_shelf.py` (docstring), `Makefile` targets `check-sandbox`, `check-apps`,
  `check-editor`, `check-migrations`, `check-runner`.

## Contract

### `GorbBake` gains (in `orbtheme-baker.js`, exported in the `root.GorbBake` block)

```js
SOUND_WAV_PREFIX   // 'data:audio/wav;base64'
SOUND_MAX_S        // 20       — seconds (owner question 1 in l0-design.md)
SOUND_MAX_SAMPLES  // 441000   — SOUND_MAX_S * SOUND_RATE
SOUND_MAX_BYTES    // 1048576  — a whole WAV file (owner question 2)
soundWav(pcm)      // Int16Array (or array of ints) -> Uint8Array: exactly l0-design §3.1's 44-byte header + samples, LE
soundWavUrl(pcm)   // -> SOUND_WAV_PREFIX + ',' + standard padded base64 of soundWav(pcm)
soundFromWav(x)    // a data URL string, Uint8Array or ArrayBuffer -> Int16Array, or null
soundPcm(v)        // the one normaliser: Int16Array -> itself; array -> Int16Array (v | 0);
                   // {"0":v,"1":v,…} object (numeric keys, in order) -> Int16Array;
                   // a string starting 'data:audio/' -> soundFromWav(v); anything else -> null
```

- `soundWav` clamps nothing and resamples nothing: it writes the samples it is given at 22,050 Hz.
  Samples outside int16 are truncated with `| 0` and stored as `Int16Array` would.
- `soundFromWav` reads any well-formed RIFF/WAVE: walk the chunks from byte 12 (id, u32 size,
  odd sizes padded by one byte), require a `fmt ` chunk of ≥ 16 bytes before `data`, and return
  the samples only when format tag 1, 1 channel, 22,050 Hz, 16 bits, block align 2, and the
  `data` chunk lies wholly inside the bytes. Anything else — wrong type, cut short, bad base64,
  a non-`data:audio/` URL — is `null`, never an exception. It does not enforce the length or
  byte bounds (an editor reads what it was given; the site and the upload enforce them).
- base64 in both directions works in node (≥ 16 has `btoa`/`atob`) and in browsers, chunked so
  a 1 MB file does not blow the stack (see session.js's `b64`).

### The pre-L0 fixtures (written by the **unchanged** editors)

- `apps/clock/editor/_fixtures/schema-1.orbdesign`: today's fixture design, opened in this
  tree's clock editor as it is now, given one owner sound on the `chime_hour` slot through the
  class's own state update (`ctl.sndUpd`, the slot set to `'upload'`, `up.chime_hour = { name:
  'l0-chime.wav', pcm: <Int16Array>, loop: false }`), then saved with the editor's own
  `designDoc(true)` (the session's envelope + `designContent(true)`). The `pcm` is 2,205 samples
  (0.1 s) of a 660 Hz sine at amplitude 8000, `Math.round(8000 * Math.sin(2π·660·i/22050))`.
  Whatever the editor writes is the fixture — the `{"0": v, …}` object included; that is the
  point. Pretty-print with `JSON.stringify(doc, null, 1)` like the current file.
- `apps/pomodoro/editor/_fixtures/schema-1.orbdesign`: likewise, today's fixture design opened
  in the unchanged pomodoro editor, given the same 2,205 samples as one upload named
  `l0-chime.wav` chosen for the focus phase's end chime the way the upload handler records it
  (`uploads` gains the entry, `chimes.<phase> = { upload: <index> }`), saved with the editor's
  own document path with art (`toDoc(d, true)` inside the session's envelope).
- Drive the editors with node (the clock's `_selftest/headless-stub.js` harness) or the headless
  Chromium that `make check-migrations` already uses — whichever works; do **not** commit the
  generator script. Each fixture's `schema` stays 1 and every field the current fixture has
  stays as the editor writes it.
- Both fixtures must still open in today's editors: `make check-migrations` green.

### Shipping what changed (ruling 39)

The fixtures ride inside each app's `.orbapp` (ruling 36), so both shelf containers change:
`apps/clock/version.txt` v1.8.0 → **v1.8.1**, `apps/pomodoro/version.txt` v1.8.1 → **v1.8.2**
(one patch above each app's version at tag v1.8.1, which `tools/check_app_versions.py` measures
against). Then `python3 tools/make_factory_apps.py`, and `python3 tools/rebake_shelf.py` for
every shelf theme `python3 tools/rebake_shelf.py --check` calls stale (it needs the Chromium in
the image; `ORB_CHROME` picks one). No other app's version moves unless `check_app_versions.py`
names it. The firmware and root `version.txt` do not change.

### Tests: `tools/orb-manager/web/runtime/_selftest/headless-wav.test.js` (new)

At least these named checks (PASS lines):
- `soundWav writes the canonical 44-byte header` — bytes 0–43 for n = 3 equal the literal
  header (`52 49 46 46 2a 00 00 00 57 41 56 45 66 6d 74 20 10 00 00 00 01 00 01 00 22 56 00 00
  44 ac 00 00 02 00 10 00 64 61 74 61 06 00 00 00`) and the samples follow LE.
- `soundWavUrl round-trips through soundFromWav` — for a 2,205-sample sine
  and for a 441,000-sample buffer, sample for sample.
- `the same samples give the same bytes` — two calls, identical base64.
- `soundFromWav refuses what the orb does not play` — stereo, 8-bit, 44,100 Hz, format tag 3
  (float), tag 0x11 (IMA ADPCM), a `data` chunk declared longer than the bytes, a file cut
  inside the header, `data` before `fmt `, not RIFF, bad base64, `data:audio/mpeg;base64,…`:
  each `null`, none throws.
- `soundFromWav skips unknown chunks` — a `LIST` chunk (odd size, padded) between `fmt ` and
  `data` still reads.
- `soundPcm takes every legacy form` — Int16Array, plain array, `{"0":…}` object (also with
  keys out of order), a WAV URL; `null` for `undefined`, a number, `'x'`.
- `the bounds are the design's numbers` — `SOUND_MAX_S === 20`, `SOUND_MAX_SAMPLES === 441000`,
  `SOUND_MAX_BYTES === 1048576`, `SOUND_WAV_PREFIX === 'data:audio/wav;base64'`.
- `the clock fixture carries inline samples` / `the pomodoro fixture carries inline samples` —
  the committed schema-1 fixtures parse, have `schema === 1`, and `soundPcm` of the clock's
  `sounds.up.chime_hour.pcm` and the pomodoro's upload `pcm` both give the 2,205-sample sine
  exactly.

## Behaviour and edge cases

- `soundWav` of zero samples writes a valid 44-byte WAV with `data` size 0; `soundFromWav` of it
  returns an empty `Int16Array` (length 0 is the editor's business, not the reader's).
- `soundFromWav` of a WAV whose header names 22,050 Hz but whose `data` size is odd: `null`.
- Nothing in any editor changes in this step: `git diff` of `apps/*/editor/*.js` is empty.

## Acceptance criteria

1. `make check-sandbox` → ends `check-sandbox: green apart from the named skips`.
2. `cd tools/orb-manager/web/runtime/_selftest && node headless-wav.test.js` → every named check
   above PASS, exit 0.
3. `make check-migrations` → both apps' schema-1 fixtures open; `make check-runner` green.
4. `python3 tools/check_app_versions.py` and `python3 tools/rebake_shelf.py --check` → pass.

## Out of scope

- Any change to clock or pomodoro editor code, `editor.json`, `theme@1`, `studio@1`, Orb
  Manager's Go code, the firmware, `main/`.
- ADPCM files, resampling, trimming, loudness.
- Do not modify: `apps/*/editor/*.js`, `apps/*/editor/editor.json`,
  `tools/orb-manager/web/runtime/theme@1`, `tools/orb-manager/web/runtime/studio@1`, `main/`,
  `tools/orb-manager/*.go`.

## Notes

- The site reads these fixtures as "a real pre-L0 design": they must be what the unchanged editor
  writes, not hand-built JSON. If an editor cannot be driven in node, use Chromium.
- The fixture replaces the current schema-1 file (one fixture per schema is the SDK's rule); it
  is still a schema-1 design that every check opens, now with a sound.
