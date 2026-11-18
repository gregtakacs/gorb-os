# L1 hub step 1: the library seam in theme@1 and the "Your library" picker in studio@1

## Goal

Slice L1 (orb-os-net ruling 42; read `docs/specs/l1-design.md` in full, it is the contract).
This step gives every editor the means to browse the owner's asset library and to stop taking
files on the site: two new host flags and a library client in `theme@1`, and in `studio@1` the
file row `Fi` learns a "Your library…" button and a picker modal, `S.Sound` passes it through,
and `S.fitSound` joins `S.fitPicture` as the shared sound converter the site's Assets page will
run. No app editor changes in this step (step 3 does that), and no Go (step 2).

## Read first

- `docs/specs/l1-design.md` — all of it; §4.1–4.4 are this step.
- `tools/orb-manager/web/runtime/theme@1/session.js` lines 1–80 (the GorbHost block, `hostUrl`,
  `hostText`) and ~186–270 (art by hash: `assetBytes`, how a data URL is rebuilt from a sidecar
  prefix — imitate its base64 and fetch handling).
- `tools/orb-manager/web/runtime/theme@1/send.js` ~35–56: the second copy of the GorbHost block.
  The two blocks between `// ---- GorbHost: keep this block byte-identical` and
  `// ---- end GorbHost` must stay byte-identical (`_selftest/headless-gorbhost.test.js` already compares them).
- `tools/orb-manager/web/runtime/studio@1/studio.js`: `S.fitPicture` (~734–800, its doc comment
  ~416–427), `S.fieldKit` and `Fi` (~804–905), `FieldRow` (~907–950, it already uses
  `React.useState`), the modals `S.Confirm` / `S.DupModal` / `S.SendModal` (~320–377: the markup
  and classes to reuse), `S.Sound` (~1070–1135).
- `apps/clock/editor/clock-logic.js` `sndUpload` (~900–990) and
  `apps/pomodoro/editor/pomodoro-editor.js` upload (~95–130): how an editor decodes a picked
  sound today (decodeAudioData, mono, 22,050 Hz). `fitSound` does the same work once, shared.
- `tools/orb-manager/web/runtime/baker@1/orbtheme-baker.js` ~2225–2380: `soundFile`,
  `soundFileUrl`, `soundRead`, `SOUND_RATE`, `SOUND_MAX_S`, `SOUND_MAX_BYTES`.
- `tools/orb-manager/web/runtime/_selftest/headless-studio.test.js` (the React stand-in and the
  PASS/FAIL style), `headless-gorbhost.test.js` (host flags), `picture-fit.js` (the headless
  Chromium driver for `fitPicture`; `sound-fit.js` copies its shape), and the `check-pictures`
  target in `Makefile` (~122–127).

## Contract

### theme@1 (`session.js` and `send.js`, the GorbHost block in both, byte-identical)

`OrbTheme.hostInfo()` returns, in addition to today's fields:
- `files`: `typeof raw.files === 'boolean' ? raw.files : kind === 'hub'`
- `library`: `raw.library === true`

Update the comment above the block to name the two flags. Nothing else in the block changes.

### theme@1 `session.js`: `OrbTheme.library`

```js
OrbTheme.library = OrbTheme.library || {
  list: function () { … },   // → Promise<{ ok: true, assets: Array } | { ok: false, message: string }>
  pick: function (asset) { … } // → Promise<{ ok: true, url, name, sha256, kind, mediaType } | { ok: false, message }>
};
```

- `list()`: `fetch(OrbTheme.hostUrl('/assets'), { cache: 'no-store' })`. A 2xx JSON
  `{ ok: true, data: { assets: [...] } }` → `{ ok: true, assets }` (keep only entries whose
  `sha256` is 64 lower-case hex, `kind` is `picture` or `sound`, and `prefix` starts with
  `data:`). `{ ok: false, error }` (any status) → `{ ok: false, message: error }`. A 401 →
  `{ ok: false, message: 'Sign in to use your library' }`. Anything else (other status, bad
  JSON, a network error) → `{ ok: false, message: 'Your library could not be reached' }`.
  Never throws.
- `pick(asset)`: `fetch(OrbTheme.hostUrl('/asset?sha256=' + asset.sha256))`; non-2xx → `{ ok:
  false, message: 'That asset could not be fetched' }` (404 the same). The bytes' SHA-256
  (`crypto.subtle.digest`) must equal `asset.sha256`, else `{ ok: false, message: 'That asset
  did not arrive whole' }`. Success: `url = asset.prefix + ',' + base64(bytes)` (standard,
  padded), `name = asset.name`, the rest copied from `asset`. Never throws.

### studio@1 `studio.js`

1. **`Fi(label, on, valText, accept, lib)`** — `lib` optional: a function `(pick) => void` or an
   object `{ pick: fn, play: fn }`. The made field carries `lib` and `kind` (`'sound'` when
   `accept` contains `audio/`, else `'picture'`). `FieldRow` renders for an `isFile` field:
   - `<input type="file">` exactly as today, only when `OrbTheme.hostInfo().files` (missing
     `OrbTheme` → treat as the hub default: files true, library false);
   - a `button` with class `ui-ghost st-sm`, text `Your library…`, title `Pick a picture from
     your orb-studio.net library` / `Pick a sound from your orb-studio.net library`, only when
     `hostInfo().library` and `lib` is given; pressing it opens `S.LibraryPicker` for this row
     (state held in `FieldRow` with `React.useState`);
   - the value line as today. Under `files: false` with no `lib`, no control at all.
2. **`S.LibraryPicker(p)`** — `p: { open, kind, accept, onPick(pick), onClose(), play(pcm, name) }`.
   Reuses `st-modal` / `st-mtitle` / `st-mbody` / `st-hint` / `st-mfoot` and the existing button
   classes (no new CSS file; at most a few rules appended to `studio@1/studio.css` for the row
   grid and the 48 px thumbnail, using existing tokens `var(--fg)` etc.). Title `Your library`.
   - On open it calls `OrbTheme.library.list()`; while waiting the body reads `Opening your library…`.
   - It shows only assets of `kind`, and for pictures only those whose `mediaType` is one of the
     types `accept` names (default accept of `Fi`: `image/png,image/svg+xml,image/jpeg,image/webp`;
     `audio/*` matches every sound).
   - A row: for a picture a 48 px `img` whose `src` is that asset's `library.pick(...)` url
     (fetched once per asset when the row first shows; until then an empty 48 px box); for a sound
     a `▶ Play` button (only when `p.play` is given) that picks the asset, reads it with
     `GorbBake.soundRead(url)` and calls `p.play(pcm, name)`; then the name; the size
     (`< 1 KB`, `84 KB`, `1.2 MB`); `512 × 512` or `2.4 s` when known; `In <name>, <name>, <name>`
     and `+N` past three design names, or nothing when none. The row is a `button`-like element
     (role `button`, `tabIndex 0`): click or Enter picks.
   - Picking: `OrbTheme.library.pick(asset)`; `ok` → `p.onPick(result)` then `p.onClose()`;
     not ok → the message in the body, the modal stays open.
   - Empty (after the filter): `No pictures in your library yet.` / `No sounds in your library yet.`
     and a hint line `Add them on orb-studio.net, under Assets.`
   - A failed list: the message from `list()`.
   - Footer: one `Cancel` button → `onClose`. Escape closes; a click on the backdrop closes
     (as `S.Confirm`).
3. **`S.Sound(Kit, o)`** — the `Your own` row passes `lib` = `{ pick: function (pk) {
   o.library(key, pk); }, play: o.play }` when `o.library` is a function; otherwise no `lib`.
   Nothing else in `S.Sound` changes.
4. **`S.fitSound(file) → Promise<{ dataUrl, seconds, samples, converted }>`** next to
   `fitPicture`, with a doc comment in the same style:
   - read the file's bytes; if `GorbBake.soundRead('data:audio/wav;base64,' + b64)` accepts them
     (it returns samples; it refuses anything not 22,050 Hz mono PCM16 / orb ADPCM) → keep the
     bytes as they are: `dataUrl` that string, `converted: false`.
   - otherwise decode with `decodeAudioData` (a `new (AudioContext||webkitAudioContext)()` or an
     `OfflineAudioContext`; close what you open), render through an
     `OfflineAudioContext(1, ceil(duration × 22050), 22050)` (the browser downmixes and
     resamples), convert to Int16 (clamp to ±1, × 32767, round), and
     `GorbBake.soundFile(pcm).url` → `dataUrl`, `converted: true`.
   - `samples` / `seconds` describe the result (`seconds = samples / 22050`, unrounded).
   - No length or size refusal here: the site's shared check judges (l1-design §3.2). Rejects
     with `Error('That file is not a sound this browser can read')` when the file cannot be read
     or decoded, or has no audio.

## Behaviour and edge cases

- Orb Manager with no `GorbHost` (today's default): `files: true`, `library: false` → every
  `Fi` row renders exactly as before (a test pins the tree equal to today's).
- The site (`{ kind: 'web', library: true }`): no file input anywhere a row gives `lib`; a
  `Your library…` button instead.
- `{ kind: 'hub', library: true }`: both the file input and the button.
- `{ kind: 'web', files: true }` is allowed (a host may opt in); `files: false` under hub too.
- `list()` with an asset whose prefix is not a data prefix, or a bad hash → dropped silently.
- Two quick presses do not open two pickers.

## Tests

### `tools/orb-manager/web/runtime/_selftest/headless-library.test.js` (new; node; the runner picks it up)

Each a `PASS <name>` line:
- `the GorbHost blocks stay byte-identical`
- `files defaults to true under the hub and false under the site`
- `library is on only when the host says so`
- `library.list reads the host's assets` (stub `fetch`: asserts the URL is `/studio-api/assets`
  under web and `/api/assets` under hub, and the answer's assets come back)
- `library.list says why it failed` (`ok:false,error` → that message; 401; a thrown fetch; bad JSON)
- `library.pick builds the data URL from the prefix and checks the hash` (stub bytes, real
  `crypto.subtle`; a wrong hash → `That asset did not arrive whole`)
- `a file row under the site has no file input and a library button`
- `a file row under the hub without a link is today's row` (tree equal to the row rendered with
  `lib` omitted and no GorbHost)
- `a file row under a linked hub has both`
- `the picker lists only the row's kind and accepted types` (a png, a jpeg and a sound; a
  clock-like accept `image/png,image/svg+xml` shows only the png)
- `the picker's empty, loading and error states read as the contract says`
- `picking calls the row's pick with the fetched url and closes`
- `the sound row passes the library through` (`S.Sound` with `o.library` → the `Your own` row has `lib`)

### `tools/orb-manager/web/runtime/_selftest/sound-fit.js` (new; headless Chromium, `picture-fit.js`'s shape; run by `make check-pictures`)

Loads `baker@1/orbtheme-baker.js` and `studio@1/studio.js` in a blank page and prints PASS lines:
- `fitSound keeps a 22,050 Hz mono PCM16 WAV byte for byte`
- `fitSound keeps the orb's ADPCM file byte for byte` (`GorbBake.soundFileUrl` of 1 s of tone)
- `fitSound converts a 44,100 Hz stereo WAV to the editors' file` (result is what `soundRead`
  accepts, `converted: true`, length within 1% of the source's seconds)
- `fitSound converts a 12 s WAV without refusing it` (`seconds` ≈ 12)
- `fitSound refuses a file that is not a sound` (bytes `hello` → the contract's message)

The WAVs are generated in the script (a sine), not committed.

## Acceptance criteria

1. `(cd tools/orb-manager/web/runtime/_selftest && node headless-library.test.js)` → every PASS
   line above, no FAIL; `VERBOSE=1` prints them.
2. `(cd tools/orb-manager/web/runtime/_selftest && node headless-test.js)` → all suites ok.
3. `make check-pictures` → `picture-fit.js` and `sound-fit.js` both pass.
4. `make check-sandbox` → `check-sandbox: green apart from the named skips`.

## Out of scope

- Any app under `apps/` (step 3), any Go file (step 2), `baker@1` (read it, do not change it),
  `host@1`, `ui@1`.
- The StartFlow design Import stays as it is (owner question 6).
- No new runtime id, no new dependency, no new colours or icons.
- Do not change the editors' own upload paths.
