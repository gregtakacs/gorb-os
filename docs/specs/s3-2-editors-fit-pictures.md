# S3 step 2: the editors scale a picked picture down to fit 1024 × 1024 before it is stored

## Goal

Slice S3 of orb-os-net's design (ruling 35), gorb-os half, step 2 of 2. The site now refuses
art that is not a PNG, JPEG, GIF or WebP of at most 1024 × 1024 pixels. So when a user picks a
picture in an editor, the editor scales anything larger than 1024 on either side down to fit
1024 × 1024 (aspect kept) before it goes into the design, and rasterises an SVG, so a user who
picks a phone photo never meets the site's refusal. One shared helper in the studio kit does
the work; the clock and flights editors call it from their pickers.

## Read first

- `tools/orb-manager/web/runtime/studio@1/studio.js`: line ~27 (`var S = root.OrbStudio = …`)
  and how other helpers hang off `S` (e.g. `S.layerRows`, `S.fieldKit`); `Fi` (~503), the
  file field every hosted editor uses.
- `apps/clock/editor/watch-engine.js` ~225–330: `MAX_ASSET_BYTES`, `bytesOf`, `svgStamp`,
  `assetPut`, `assetLoad`, `assetFromFile` (the clock's one front door for an upload), and
  `window.WatchEngine` (~5215) which exports `assetFromFile`.
- `apps/clock/editor/clock-logic.js` ~710–720: the caller of `E.assetFromFile`.
- `apps/clock/editor/_selftest/headless-stub.js` and `headless-test.js`: the clock's node suite
  stubs `FileReader`/`Image`; it has no `OrbStudio.fitPicture` and must keep passing.
- `apps/flights/editor/flights-editor.js` ~47–64 (`readPicture(f, max)`) and its three callers
  (~212 scope picture at 932, ~367 custom plane at 192, ~447 frame at 1200).
- `apps/weather/editor/weather-editor.js` ~318–336: already scales to ≤ 932 as JPEG. Leave it.
- `tools/orb-manager/web/runtime/_selftest/editors-smoke.js` lines 1–120: how a selftest finds
  Chromium (`--chrome`, `$CHROME`, `chromium`…), serves files and drives the page over CDP.
  Imitate that; do NOT edit `editors-smoke.js` (another lead is changing it).
- `Makefile` `check-web` (~69).

## Contract

### `OrbStudio.fitPicture(file, opts)` in `studio@1/studio.js`

```js
// file: a File or Blob. opts: { max = 1024, type, quality = 0.9 }
//   max:  the longest side allowed (clamped to at most 1024 whatever is passed)
//   type: 'image/png' or 'image/jpeg' to force the output type; omitted = keep JPEG as JPEG,
//         anything else becomes PNG
// → Promise<{ dataUrl, type, width, height, scaled }>
S.fitPicture = function (file, opts) { … };
```

1. Read the file as a data URL (`FileReader.readAsDataURL`); decode it with an `Image`
   (wait for `onload`/`decode()`). A file that does not decode → reject with
   `new Error('That file is not a picture this browser can read')`.
2. `svg` = the file's type is `image/svg+xml` or the data URL starts `data:image/svg+xml`.
   `w`, `h` = `naturalWidth`, `naturalHeight`; for an SVG with no intrinsic size (0), use
   `max × max`.
3. **Kept as it is** (`scaled: false`, the original data URL byte for byte) when: not `svg`, the
   source type is one of `image/png`, `image/jpeg`, `image/gif`, `image/webp`, `w <= max` and
   `h <= max`, and `opts.type` is unset or equals the source type.
4. Otherwise draw onto a canvas and re-encode (`scaled: true`):
   - raster: `k = Math.min(1, max / Math.max(w, h))`;
   - SVG: `k = max / Math.max(w, h)` (the longer side becomes exactly `max`, up or down: a
     vector has no pixels to lose);
   - `width = Math.max(1, Math.round(w * k))`, `height = Math.max(1, Math.round(h * k))`;
   - output type: `opts.type` if given, else `image/jpeg` for a JPEG source, else `image/png`;
     JPEG uses `opts.quality`.
   - The answer's `width`/`height` are the canvas's; `type` is the output type.
   So a 3000 × 2000 picture comes out 1024 × 683; 2000 × 3000 comes out 683 × 1024.

### Clock: `assetFromFile` (`watch-engine.js`)

- Accepted types stay `image/png` and `image/svg+xml`, with today's refusal sentence.
- When `window.OrbStudio && window.OrbStudio.fitPicture` exists: the picture goes through
  `fitPicture(file, { max: 1024, type: 'image/png' })` first; the result's data URL is what is
  stored (`mime: 'image/png'`, so `kind` is `png`, and `svgStamp` is not applied). The
  `MAX_ASSET_BYTES` check moves AFTER the fit and measures the decoded bytes of the resulting
  data URL, with the same sentence shape (`<file name> is <n> KB; the limit is 1.5 MB`). A
  fit that rejects → `{ error: '<that message>: ' + file.name }`.
- When `fitPicture` is not there (the node suite's stub world), today's path runs unchanged.
- Nothing else in the clock changes. Designs that already hold SVG art still open and draw.

### Flights: `readPicture(f, max)` (`flights-editor.js`)

- When `OrbStudio.fitPicture` exists: `return OrbStudio.fitPicture(f, { max: Math.min(max, 1024), type: 'image/png' }).then(function (r) { return r.dataUrl; })`.
  So the frame picture (asked at 1200) is now at most 1024, an SVG is rasterised instead of
  kept as drawn, and PNG stays the output as today.
- Otherwise today's code, except `max` is clamped to 1024 and an SVG is drawn to a canvas too.

### Weather: unchanged (it already stores at most 932 × 932 JPEG).

### Selftest `tools/orb-manager/web/runtime/_selftest/picture-fit.js` (new)

A Node script in the style of `editors-smoke.js` (find Chromium the same way, serve files over
a local http server, drive headless Chromium over CDP; no npm packages). It loads a page with
`react@18.3.1` (from `web/runtime/vendor/`), `studio@1/studio.js`, and the clock's
`dial-renderer.js` + `watch-engine.js` from `apps/clock/editor/`, then, in the page, builds
test pictures with a canvas (`toBlob`) or SVG text, wraps them as `File`s, and checks:

1. 3000 × 2000 PNG → `fitPicture` default → `image/png`, 1024 × 683, `scaled: true`, and the
   result really decodes at 1024 × 683.
2. 3000 × 2000 JPEG → `image/jpeg`, 1024 × 683.
3. 2000 × 3000 PNG → 683 × 1024.
4. 800 × 600 PNG → `scaled: false` and the data URL equals the file's own data URL.
5. 1024 × 1024 PNG → `scaled: false`; 1025 × 10 PNG → 1024 × 10.
6. SVG `viewBox="0 0 24 24"` with width/height 24 → PNG 1024 × 1024; with `{max: 192}` → 192 × 192.
7. 3000 × 2000 WebP (if the browser encodes WebP; else skip with a note) → PNG 1024 × 683.
8. A text file named `x.png` → rejects with `That file is not a picture this browser can read`.
9. `WatchEngine.assetFromFile(<3000 × 2000 PNG File>)` → an asset with `w === 1024`,
   `h === 683`, `kind === 'png'`, and a `dataUrl` starting `data:image/png;base64,` that
   decodes at 1024 × 683. If the clock engine cannot be loaded standalone, load the minimum
   of the clock's modules it needs; do not change the engine to make it loadable.

It prints one line per check and ends `picture-fit: N/N passed` (exit 0), or the failures and
exit 1. Add it to the Makefile as a target `check-pictures` (needs `NODE`, like `check-runner`),
and call `$(MAKE) check-pictures` as the first line of `check-web`'s recipe.

## Behaviour and edge cases

- A picture already within 1024 × 1024 of an accepted type is not re-encoded (no quality loss,
  same bytes), except where the caller forces a type (clock, flights: PNG, as they store today).
- The longer side never exceeds 1024 whatever `max` a caller passes.
- An animated GIF that is scaled keeps its first frame (canvas behaviour); one within the limits
  and kept as-is stays animated.

## Acceptance criteria

1. `node --check tools/orb-manager/web/runtime/studio@1/studio.js && node --check apps/flights/editor/flights-editor.js && node --check tools/orb-manager/web/runtime/_selftest/picture-fit.js`
2. `make check-pictures` → ends `picture-fit: N/N passed` with N ≥ 9 (8 if WebP encode is skipped).
3. `make check-editor` → green (the clock node suite, the runtime suites, the Python cross-checks).
4. `make check-web`, `make check-runner`, `make check-migrations` → green.
5. `sh docs/specs/s3-gotest.sh` → green (the runtime is embedded in the hub binary;
   `runtime_test.go` compares served bytes to the embedded files, which must still hold).

## Out of scope

- Orb Manager's Go code (step 1, done) apart from nothing: no Go changes in this step.
- Do not modify: `tools/orb-manager/editor.go`, `tools/orb-manager/security.go`,
  `tools/orb-manager/web/runtime/theme@1/` (anything), `tools/orb-manager/web/runtime/host@1/`,
  `tools/orb-manager/web/runtime/_selftest/editors-smoke.js`, `apps/weather/`, any app's
  `app.json`/`editor.json` or version, the shelf's baked themes.
- No new runtime file (a new `<script>` would need the site and the hub page to change too):
  `fitPicture` lives in the existing `studio@1/studio.js`.

## Notes

- `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH` for Go and the make targets that build the hub.
- `check-web` builds the release web archive (`make web`) and needs Chromium; the image has it.
