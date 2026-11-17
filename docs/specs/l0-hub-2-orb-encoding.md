# L0 hub step 2: a sound file is the orb's own encoding, and the baker bakes it as it is

## Goal

Slice L0 (ruling 41), gorb-os step 2 of 4. Step 1 (`docs/specs/l0-hub-1-wav-kit.md`, done) gave
`GorbBake` a WAV writer that always writes lossless PCM16 and a 20 s bound. The owner has since
ruled (`docs/specs/l0-design.md` §2–§3.1, revised 2026-11-17, binding) that the stored file is the
orb's own encoding — `soundEncode`'s choice, ADPCM unless under the SNR floor — in a WAV header the
site can bound without decoding; that an unchanged sound keeps its bytes across saves; and that
the bake uses those stored bytes instead of re-encoding. This step rewrites step 1's kit to that,
and teaches the baker's sound stage to take pre-encoded clips. No editor changes yet.

## Read first

- `docs/specs/l0-design.md` — §2 (numbers), §3.1 (the two byte layouts, verbatim), §5.
- `docs/specs/l0-hub-1-wav-kit.md` — what step 1 built; its fixtures stay exactly as they are.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` — step 1's additions (`SOUND_WAV_PREFIX`,
  `SOUND_MAX_*`, `b64bytes`, `unb64bytes`, `wavHeader`, `soundWav`, `soundWavUrl`, `riffAt`,
  `wavBytes`, `soundFromWav`, `soundPcm`); `soundEncode` / `adpcmEncode` / `adpcmDecode` /
  `soundBytes` (~2170–2240); the sound stage `sounds(list, o)` (~3960–4030) and its callers
  (grep `\.sounds(` and `o.sounds`, ~4203).
- `tools/orb-manager/web/runtime/_selftest/headless-wav.test.js` (step 1's suite, to rewrite) and
  `headless-soundpane.test.js` (the sound stage's suite, must stay green).
- `tools/theme_schema.py` `sound_encode` (~764), `tools/host_adpcm_test.py` — the reference encoder
  the JS one is pinned to.
- `main/audio/orb_adpcm.h` — the block layout.

## Contract

### `GorbBake` (replace step 1's sound-file API; one encoder: `soundEncode`)

```js
SOUND_WAV_PREFIX   // 'data:audio/wav;base64'                       (unchanged)
SOUND_MAX_S        // 10                                             (was 20)
SOUND_MAX_SAMPLES  // 220500                                         (SOUND_MAX_S * SOUND_RATE)
SOUND_MAX_BYTES    // 1048576  — ORB_VOICE_CAP                       (unchanged)
soundFile(pcm)     // Int16Array | int array -> { url, bytes, enc, n, body }:
                   //   se = soundEncode(pcm); bytes = l0-design §3.1's header for se.enc and n + se.bytes;
                   //   body = se.bytes; url = SOUND_WAV_PREFIX + ',' + base64(bytes)
soundFileUrl(pcm)  // soundFile(pcm).url
soundRead(x)       // a data URL string, Uint8Array or ArrayBuffer -> null, or
                   //   { enc, n, body: Uint8Array (the data chunk), pcm: Int16Array (decoded:
                   //     PCM16 as is, ADPCM via adpcmDecode(body, n)) }
soundPcm(v)        // as step 1, except a 'data:audio/' string -> soundRead(v).pcm (or null)
```

- Remove `soundWav`, `soundWavUrl` and `soundFromWav` from the code and the export block: nothing
  may write a PCM file when ADPCM qualifies. (No editor uses them yet; step 1's suite is rewritten.)
- `soundRead` accepts exactly the two layouts of §3.1, walking chunks like step 1's reader (unknown
  chunks skipped, odd sizes padded): PCM16 = fmt tag 1, 1 ch, 22,050 Hz, 16 bits, align 2, even
  `data`; ADPCM = fmt size ≥ 20, tag 0x11, 1 ch, 22,050 Hz, 4 bits, align 132, cbSize ≥ 2,
  samplesPerBlock 256, a `fact` chunk before `data` naming n ≥ 1, and `data` exactly
  `ceil(n / 256) * 132` bytes. The `data` body must lie wholly inside the bytes. Anything else →
  null, never an exception. It enforces no length or byte bound.
- `soundRead(soundFile(p).bytes).body` is byte-identical to `soundEncode(p).bytes`, and
  `soundFile(soundRead(f).pcm)` is **not** used anywhere to "refresh" a file (that would be a
  second generation).

### The baker's sound stage takes stored bytes

`sounds(list, o)`: a list item may carry `coded: { enc, body }` (from `soundRead`) beside its `pcm`
(the decoded samples, `pcm.length === n`). For such an item the plane is `put(coded.body, n, 1)`
with `enc: coded.enc` — **no `soundEncode`**. Plane sharing: two coded items share a plane when
their `enc` and `body` bytes are equal; two `pcm`-only items share as today; a coded and an uncoded
item never share. Items without `coded` behave exactly as today. Validate `coded`: `enc` one of the
two encodings, `body.length === soundBytes(enc, n)`, else throw
`'sound slot ' + slot + ' carries a stored sound whose bytes do not match its length'`. The comment
above `sounds` says so. Pass `coded` through whatever caller layer maps an app's
`o.sounds` items into this list (grep), unchanged when absent.

### Tests: rewrite `tools/orb-manager/web/runtime/_selftest/headless-wav.test.js`

Named PASS lines (exact):
- `soundFile writes the orb's ADPCM when it qualifies` — for the 2,205-sample 660 Hz sine of step 1:
  `enc === 1`, the 60-byte header equals the literal bytes of §3.1 for n = 2205 (B = 9), and the
  body equals `soundEncode(sine).bytes`.
- `soundFile writes PCM16 only under the SNR floor` — for a clip `soundEncode` keeps as PCM16 (a
  sharp click, as `headless-soundpane.test.js` or `theme_schema.py`'s note on the tick describes):
  `enc === 0`, the 44-byte header literal, body = the samples.
- `the body is theme_schema.sound_encode's bytes` — spawn `python3` on `tools/theme_schema.py`'s
  `sound_encode` for the sine and the click and compare bytes (skip with a PASS-less SKIP line only
  if `python3` is missing; it is present in the gate).
- `soundRead round-trips both encodings` — enc, n and body back; `pcm` equals `adpcmDecode(body, n)`
  for ADPCM and the samples for PCM16.
- `the same samples give the same file` — two `soundFile` calls, identical url.
- `an unchanged file keeps its bytes` — read a file, hand its `url` on as the stored value, read
  again: identical; and `soundFile(soundRead(f).pcm).url !== f.url` is allowed (documented
  generation loss) but nothing in the kit calls it.
- `soundRead refuses what the orb does not play` — stereo, 8-bit, 44,100 Hz, tag 3, a standard
  IMA-WAV (samplesPerBlock 257), ADPCM without `fact`, ADPCM `data` one block short, PCM `data`
  declared past the bytes, cut inside the header, `data` before `fmt `, not RIFF, bad base64,
  `data:audio/mpeg;base64,…`: each null, none throws.
- `soundRead skips unknown chunks` — a `LIST` chunk of odd size between `fmt ` and `data`.
- `soundPcm takes every legacy form` — as step 1, plus a §3.1 ADPCM url → decoded samples.
- `the bounds are the design's numbers` — 10, 220500, 1048576, the prefix.
- `the baker bakes a stored sound as it is` — `stage3().sounds([{slot, pcm: decoded, coded}])` puts
  exactly `coded.body` in the bank (read the plane back) with the row's `enc`; and a pcm-only item
  of the original sine still produces `soundEncode(sine).bytes` (today's path untouched).
- `a stored sound with the wrong byte count is refused` — the exact error above.
- `the clock fixture carries inline samples` / `the pomodoro fixture carries inline samples` — kept
  from step 1.

## Behaviour and edge cases

- `soundFile` of fewer than 2 samples: throw `'a sound is at least two samples'` (a plane needs 2).
- `soundEncode`, `adpcmEncode`, `adpcmDecode` are not modified.
- A file `soundRead` accepts may carry extra chunks; keeping it verbatim keeps them — the site's
  1 MB bound is what limits that.

## Acceptance criteria

1. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
2. `cd tools/orb-manager/web/runtime/_selftest && node headless-wav.test.js` → every named check
   PASS, no FAIL, exit 0; `node headless-soundpane.test.js` → exit 0.
3. `python3 tools/host_adpcm_test.py` → passes; `make check-runner check-migrations` → green.
4. `python3 tools/check_app_versions.py` and `python3 tools/rebake_shelf.py --check` → pass
   (the runtime change must not alter any shelf theme's bytes; if it does, that is a bug here).

## Out of scope

- Any editor (`apps/*/editor/*.js`, `editor.json`), the fixtures, `theme@1`, `studio@1`, `host@1`,
  Orb Manager's Go code, `main/`, `tools/theme_schema.py`.
- Do not modify: `apps/`, `main/`, `tools/theme_schema.py`, `tools/orb-manager/web/runtime/theme@1`,
  `tools/orb-manager/web/runtime/studio@1`, `tools/orb-manager/web/runtime/host@1`,
  `tools/orb-manager/*.go`.
