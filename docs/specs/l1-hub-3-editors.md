# L1 hub step 3: every editor's pickers take "Your library", verbatim

## Goal

Slice L1 (orb-os-net ruling 42; `docs/specs/l1-design.md` §4.3–4.4). Step 1 taught studio@1's
file row `Fi(label, on, valText, accept, lib)` to show "Your library…" (and to drop the file input
on the site). This step passes `lib` at every picture and sound picker of the four editors that
take files — clock, flights, weather, pomodoro — so a picked library asset lands where an upload
would, **with its bytes untouched**: the url the picker hands over is what the design holds, so
the next save names the same sha256 and the site stores nothing new. Then ship them (ruling 39).

## Read first

- `docs/specs/l1-design.md` §4.3, §4.4, owner question 7.
- `docs/specs/l1-hub-1-library-kit.md` — the `Fi` / `lib` / `pick` contract (a pick is
  `{ ok: true, url, name, sha256, kind, mediaType }`; `lib.pick` receives it).
- clock: `apps/clock/editor/clock-logic.js` `pickArt` (~724), the four `Fi` rows at ~2369,
  ~3878, ~4114 (pictures) and ~4398 (sound), `sndUpload` (~940–990), `sndTake` (~895);
  `apps/clock/editor/watch-engine.js` `assetPut` (~263), `assetFromFile` (~301–345) and its
  export list (~5354). The clock's art is PNG or SVG only.
- flights: `apps/flights/editor/flights-editor.js` `readPicture` (~54) and the three `K.Fi('Picture', …)`
  rows (~264, ~440, ~520).
- weather: `apps/weather/editor/weather-editor.js` the `K.Fi(… 'Your picture' …)` row (~319–335).
- pomodoro: `apps/pomodoro/editor/pomodoro-editor.js` `keep` (~45), `takeUpload` (~146), the
  `S.Sound(K, {…})` call (~362–375); `apps/pomodoro/editor/pomodoro-bake.js` `UPLOAD_MAX_S`.
- The existing suites: `apps/clock/editor/_selftest/headless-l0-sound.test.js` (how the clock
  is driven headless) and `tools/orb-manager/web/runtime/_selftest/headless-pomodoro-sound.test.js`.
- `docs/specs/l0-hub-3-clock.md` "Ship" and `tools/check_app_versions.py`,
  `tools/make_factory_apps.py`, `tools/rebake_shelf.py` — how a changed editor ships.

## Contract

For every picker below, `lib` is given and `lib.pick(pk)` stores `pk.url` **exactly** (no
`fitPicture`, no canvas re-draw, no `toDataURL`, no `soundFile`/`soundFileUrl`, no
normalising), in the same place the file path stores its result, then says what happened in
the editor's own status line the way the file path does.

- **clock pictures** (2369, 3878, 4114): new `WatchEngine.assetFromUrl(pick)` (exported beside
  `assetFromFile`) → `assetPut({ id: <stem>-<sha256 first 8>, name: pick.name, mime: pick.mediaType,
  dataUrl: pick.url, sha: <the same sha32 the file path computes from the bytes>, w, h })`,
  loaded with `assetLoad` like the file path, resolving `{ error }` when the mediaType is not
  `image/png` or `image/svg+xml` or the picture does not load. `pickArt` gains a sibling
  `pickLibraryArt(pick, slot, apply)` that does what `pickArt` does after the asset exists
  (status line included). The `Fi` rows pass `accept` `image/png,image/svg+xml` (the 2369 row
  already does; the other two get it) so the picker offers only PNG and SVG.
- **clock sound** (4398): `lib = { pick: (pk) => this.sndLibrary(pk, slot), play: <the clock's
  own play of a pcm> }`. `sndLibrary(pk, slot)`:
  - reads `GorbBake.soundRead(pk.url)`; null → `sndMsg` `Not used: that sound can't be read`.
  - a chime slot (anything but `tick_loop`): `d.up[slot] = { name: pk.name, wav: pk.url, pcm:
    <soundRead's pcm>, loop: false }` and `d.slots[slot] = 'upload'` — verbatim, no `SE.finish`.
  - `tick_loop`: a loop is derived from the sound exactly as an upload's is (L0 §3.1: a derived
    clip is made from the decoded samples). Factor the post-decode part of `sndUpload` (from the
    Float64 samples on) into one method both call, so the loop rules are not copied; the
    derived loop is a new file, as it is for an upload. Say so in a comment.
  - status: `<name> → <slot words> (<secs> s, <KB> KB in the theme)` as the upload path.
- **flights** (264, 440, 520): `lib = (pk) => upd(<the same path the file path writes>, pk.url)`
  and the same `say(...)` the file path uses on success. No `readPicture`.
- **weather** (319): `lib = (pk) => upP({ img: pk.url, imgScale: 1, imgX: 0, imgY: 0 })` and the
  same status line as the file path. No canvas, no JPEG.
- **pomodoro** (S.Sound): `o.library = function (k, pk) { takeLibrary(k, pk, c); }` where
  `takeLibrary` reads `soundRead(pk.url)`; null → `c.say('Not taken: that sound can\'t be read')`;
  longer than `UPLOAD_MAX_S` → `c.say('Not taken: “<name>” is <s> s: a pomodoro chime is at most 4.0 s')`;
  otherwise pushes `keep(null, pk.name, null, pk.url)` (L0's verbatim path) and points the
  chime at it, as `takeUpload` does, with the same `c.say` sentence.
  Pass `o.play` through if `S.Sound` already receives one (it does: `play`/`stop`).

No other behaviour changes. Under Orb Manager with no link, every editor renders exactly as
before (the file inputs, no library button).

## Tests

- `apps/clock/editor/_selftest/headless-l1-library.test.js` (new; picked up by that folder's
  `headless-test.js`). PASS lines:
  - `a library picture lands verbatim` (a PNG pick → the asset's `dataUrl === pick.url`)
  - `a library SVG lands verbatim`
  - `a jpeg pick is refused by the clock` (`assetFromUrl` → `{ error }`)
  - `a library chime lands verbatim` (`d.up.chime_hour.wav === pick.url`, no `soundFile` call)
  - `a library chime survives a save` (`toDoc(…, true)` carries the same `wav` string)
  - `a library tick becomes a loop like an upload` (the shared post-decode path ran)
  - `the clock's picture rows offer only png and svg` (each row's `accept`)
  - `every clock file row has a library pick` (every `isFile` field the clock builds has `lib`)
- `tools/orb-manager/web/runtime/_selftest/headless-l1-pickers.test.js` (new). PASS lines:
  - `a pomodoro library chime is kept verbatim` (the held `wav === pick.url`)
  - `a pomodoro library chime over 4.0 s is refused with its length`
  - `every pomodoro file row has a library pick`
  - for flights and weather, if their editors load under node with the stubs the runtime suites
    already use: `a flights library picture lands verbatim`, `every flights file row has a library pick`,
    `a weather library picture lands verbatim`, `every weather file row has a library pick`. If
    they cannot be loaded under node, say so in the report; step 4's browser gate covers them.

## Ship (ruling 39)

Each of clock, flights, weather and pomodoro gets the next patch version in `apps/<app>/version.txt`
above the newest tag (today v1.8.1; flights and weather are v1.8.0, clock v1.8.1, pomodoro v1.8.2 →
clock v1.8.2, flights v1.8.1, weather v1.8.1, pomodoro v1.8.3, unless `check_app_versions.py`
asks otherwise — it is the judge). Then `python3 tools/make_factory_apps.py` and
`python3 tools/rebake_shelf.py` for whatever `--check` calls stale. No editor's design schema
changes (nothing about the stored design changes), so no fixture or migration step is added.

## Acceptance criteria

1. `(cd apps/clock/editor/_selftest && node headless-test.js)` → `suites green`, and
   `node headless-l1-library.test.js` prints every PASS above.
2. `(cd tools/orb-manager/web/runtime/_selftest && node headless-test.js)` → all ok, and
   `node headless-l1-pickers.test.js` prints its PASS lines, no FAIL.
3. `make check-sandbox` → green apart from the named skips.
4. `python3 tools/check_app_versions.py && python3 tools/rebake_shelf.py --check` → pass.
5. `make check-migrations check-runner` → pass.

## Out of scope

- `tools/orb-manager/web/runtime/theme@1`, `studio@1`, `baker@1`, `host@1` (step 1's), Go (step 2's).
- radar and color_picker (they take no files).
- The editors' own upload paths stay as they are, apart from the clock's shared post-decode
  refactor.
- No schema bump, no fixture change.
