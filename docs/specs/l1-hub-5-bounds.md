# L1 hub step 5: the owner's answers to L0 — sounds up to 20.0 s, every refusal names its limit, the voice bank checked at the bake

## Goal

The owner answered L0's open questions (2026-11-17; `docs/specs/l0-design.md` §5):

1. **The longest sound is 20.0 s** (it was 10.0). Everywhere L0 put 10 s, it becomes 20 s.
2. **1 MB per sound file stays**, and **every refusal states the limit it hit**: a length refusal
   says the sound's length and 20.0 s, a size refusal its size and 1 MB, a rate refusal its rate
   and 22,050 Hz, a format refusal the formats that are allowed, and the voice bank the bank's
   limit and how far the theme is over.
3. The orb-block ADPCM layout stays (sounds are for the orb only; no PCM export).
4. A file that is already a valid orb sound, or an in-bounds picture, is kept byte for byte by
   the shared pickers (`fitSound`, `fitPicture`), converted only when it needs it.

At 20 s a PCM16-fallback clip is 882,044 bytes, so one clip fits the 1 MB file bound, but two or
three long chimes can overrun the app's **voice bank**, which holds at most 1 MB
(`main/art/orb_voice.h` `ORB_VOICE_CAP`). Today only the orb refuses that, at install, with
`this theme's sounds are more than an app's voice bank holds (1 MB)` (`main/art/orb_theme.c`
~2780). This step makes the bake refuse it first, with the numbers.

## Read first

- `docs/specs/l0-design.md` §2, §5 (the questions this answers).
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` ~2220–2240 (`SOUND_MAX_S`,
  `SOUND_MAX_SAMPLES`, `SOUND_MAX_BYTES`, their comment) and the sound stage (grep `coded`,
  `soundEncode`, `codedClip`): where a bake knows every sound's bytes.
- `main/art/orb_voice.h` (the bank's layout: `ORB_VOICE_CAP`, how `voice_bank_size` adds up the
  sounds — header, table, per-sound alignment) and `main/art/orb_theme.c` ~2274–2365, ~2775–2790.
  The bake's sum must be the orb's sum (same header, table and padding), so a theme the bake
  passes is one the orb accepts and vice versa.
- `tools/theme_schema.py` — if it builds the sound plane too (grep `sound_encode`, `voice`), the
  same check goes there with the same sentence, and `tools/host_*_test.py` pins them equal.
- `apps/clock/editor/clock-logic.js` `sndUpload` (~950–960: the refusal using `GB.SOUND_MAX_S`),
  `apps/pomodoro/editor/pomodoro-bake.js` `UPLOAD_MAX_S` (4.0 s, the pomodoro's own clip rule —
  it stays 4.0 s; its refusal already names it: check it does everywhere it is refused).
- Tests that pin 10: `apps/clock/editor/_selftest/headless-l0-sound.test.js` (~258, `a 10.1 s
  upload is refused with its length`), `tools/orb-manager/web/runtime/_selftest/headless-wav.test.js`
  (~239, `the bounds are the design's numbers`), and anything `git grep -nE "10\.0 s|220500|SOUND_MAX_S"` finds.
- `tools/orb-manager/web/runtime/studio@1/studio.js` `fitPicture`, `fitSound`, `checkSvg`, and the
  clock's `watch-engine.js` `assetFromFile` refusals.

## Contract

1. **20 s.** `SOUND_MAX_S = 20` (so `SOUND_MAX_SAMPLES` 441,000), `SOUND_MAX_BYTES` stays
   1,048,576. Update the comment (owner, 2026-11-17). The clock's upload refusal is unchanged in
   form and now reads `<file> is 20.1 s: a sound can be at most 20.0 s, so it was not taken`.
   Rename the clock test `a 10.1 s upload is refused with its length` → `a 20.1 s upload is
   refused with its length` (20.1 s of samples) and add `a 19.9 s upload is taken`.
2. **The voice bank at the bake.** New `GorbBake.VOICE_BANK_MAX = 1048576` and
   `GorbBake.voiceBankBytes(coded)` (the orb's `voice_bank_size` arithmetic over the theme's
   sound items). A bake whose sounds come to more refuses with exactly
   `this theme's sounds come to <X> KB: an app's voice bank holds at most 1,024 KB, so it is <Y> KB over`
   (KB = 1024 bytes, `X` and `Y` rounded up, `toLocaleString('en-US')`), thrown/returned the way
   the baker's other refusals are so the editor's status line shows it. Mirror it in
   `theme_schema.py` if that bakes sounds.
3. **Every refusal names its limit.** Walk every place an editor or the kit refuses a sound or a
   picture (`fitSound`, `fitPicture`, `checkSvg`, the clock's `sndUpload` and `assetFromFile`, the
   pomodoro's upload and its bake check, the bake's sound stage). Each sentence must state the
   bound with the actual value where there is one (length, size, rate, dimensions, the bank), or
   the allowed formats for a format refusal. `fitSound`'s decode refusal becomes `That file is
   not a sound this browser can read: a sound must be audio the browser can decode (WAV, MP3,
   OGG …), at most 20.0 s once converted`. Fix any that do not; list each sentence you checked,
   before → after, in the report.
4. **Byte for byte when valid.** Pin with tests (they may exist from step 1: keep them): a WAV
   `soundRead` accepts goes through `fitSound` unchanged; a 1024 × 1024 PNG and an SVG go through
   `fitPicture` unchanged; a 44,100 Hz or stereo WAV is converted.
5. `docs/specs/l0-design.md`: §2's table says 20.0 s = 441,000 samples, and §5 question 1 and 2
   gain `**Answered (owner, 2026-11-17):** …` lines (20.0 s; 1 MB stays and every refusal names its
   limit; the block layout stays).

Ship: clock and pomodoro (and any other app whose editor files change) stay at or above the
versions step 3 gave them so `check_app_versions.py` passes; run `make_factory_apps.py` and
`rebake_shelf.py` for whatever `--check` calls stale.

## Tests (PASS lines)

`tools/orb-manager/web/runtime/_selftest/headless-wav.test.js`: `the bounds are the design's
numbers` asserts 20 s / 441,000 / 1,048,576; new `the bake refuses a voice bank over 1 MB with
its numbers` (three 20 s chimes that only PCM16 can carry, or the smallest set that overruns),
`a voice bank of exactly the limit bakes`, `the bake's bank size is the orb's` (against the
arithmetic of `orb_voice.h`, worked by hand in the test for two sounds).
Clock: the two upload checks of contract 1. `sound-fit.js`: `fitSound's refusal names what a
sound must be`.

## Acceptance criteria

1. `make check-sandbox` → green apart from the named skips.
2. `(cd tools/orb-manager/web/runtime/_selftest && VERBOSE=1 node headless-wav.test.js)` → the PASS lines above.
3. `(cd apps/clock/editor/_selftest && node headless-test.js)` → `suites green`; `VERBOSE=1 node headless-l0-sound.test.js` → `PASS a 20.1 s upload is refused with its length`.
4. `make check-web check-pictures check-migrations check-runner` → pass.
5. `python3 tools/host_adpcm_test.py && python3 tools/check_app_versions.py && python3 tools/rebake_shelf.py --check` → pass.
6. `git grep -nE "SOUND_MAX_S = 10|220500|at most 10\.0 s" -- ':!docs'` → nothing.

## Out of scope

- The firmware (`main/`): read it, do not change it. The orb's own install refusal stays.
- No change to the ADPCM layout, the 1 MB file bound, the pomodoro's 4.0 s clip rule.
