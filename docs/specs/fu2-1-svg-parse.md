# fu2-1: the hub's and the studio's SVG checks become a parse plus an allow-list

## Goal

Implement `docs/specs/fu2-svg-rules.md` (binding; read it in full first) in gorb-os: the Go
`checkSvgArt` and the JS `OrbStudio.checkSvg`, the shared vector file, the studio suite in
`make check-editor`, and a live Chromium gate proving no accepted vector fetches anything. The
site's TS half is a later step in orb-os-net; do not touch orb-os-net.

## Read first

- `docs/specs/fu2-svg-rules.md` — the rule set, rule ids, order, notes. It wins over this file
  wherever they disagree.
- `tools/orb-manager/sitelimits.go` lines 136–360 (`checkSvgArt`, `svgForbidden`,
  `artRefusal`) and its tests in `tools/orb-manager/sitelimits_test.go` (`svgVectors`,
  `TestCheckSvgArt*`, `TestSvgArtSharedVectors`).
- `tools/orb-manager/web/runtime/studio@1/studio.js` lines 417–560 (`S.checkSvg`, `S.fitPicture`).
- `tools/orb-manager/web/runtime/_selftest/headless-studio.test.js` (the SVG section near the end)
  and `_selftest/picture-fit.js` (how a suite drives studio.js in headless Chromium).
- Root `Makefile` targets `check-editor`, `check-pictures`.
- The input vectors, mounted read-only:
  `/tmp/claude-1000/-home-greg-docker-Projects/5278e4ea-1e48-4d88-90a5-999cf1ef2d25/scratchpad/svgreview/vectors.json`
  (a JSON list of `[name, svg]`), `.../svgreview/chrome/server.js` (more vectors, beacon form),
  and `.../svgprobe/server.js` (the object `cases`: each entry builds an SVG body around a beacon
  URL; wrap it in the `svg()` helper in that file). Copy these inputs into the vector file
  programmatically or by hand; do not paraphrase them, and do not edit those directories.

## Contract

- `tools/orb-manager/testdata/svg-vectors.json` — new; a JSON array of
  `{"name": string, "svg": string, "rule": string|null}` plus, for vectors that are not valid
  UTF-8, `"svg_base64"` instead of `"svg"`. Every input from the three sources above appears
  once (names prefixed `review_`, `review_chrome_`, `probe_`), with beacon URLs written as
  `http://127.0.0.1:47613/b/<name>`. Also every case now in `svgVectors`,
  `TestSvgArtSharedVectors` and the JS SVG section, and the accepted real-world shapes the rules
  file lists. Expected `rule` is the rule id from the rules file.
- Go: `checkSvgArt(b []byte) string` keeps its signature and sentences for its callers; add
  `checkSvgArtRule(b []byte) (rule string, sentence string)` (rule "" when accepted), and make
  `checkSvgArt` return the sentence of it. New sentences use the existing
  `svgRefusalHoldsFormat` style, worded for each rule ("an SVG in it holds …").
- JS: `S.checkSvg(input)` accepts a string, a `Uint8Array` or an `ArrayBuffer` and returns
  `null` when accepted or `{ ok: false, rule, message }`. `S.fitPicture` reads an SVG file with
  `readAsArrayBuffer`, passes the bytes, and rejects with `new Error(result.message)`. Update every
  other caller (`grep -rn checkSvg tools apps`).
- Node has no `DOMParser`, so the JS vector run happens in headless Chromium: new
  `tools/orb-manager/web/runtime/_selftest/svg-vectors.js` (pattern: `picture-fit.js`), printing
  `svg-vectors: N/N passed`. The SVG section of `headless-studio.test.js` keeps only what node can
  check (the size ceiling and size-first order, which must not need DOMParser) and loads nothing
  else. Add `headless-studio.test.js` and `svg-vectors.js` to `check-editor`.
- New `tools/orb-manager/web/runtime/_selftest/svg-fetch-live.js` and root target
  `check-svg-live` (NOT part of `check`, like check-pictures): a beacon server on
  127.0.0.1:47613; for every vector serve the SVG three ways — as `image/svg+xml`, inside an
  `<img>` page, and inlined into an HTML page — load each in headless Chromium (as
  svgprobe/drive.sh does) and record beacon hits. It fails if any vector whose expected `rule`
  is `null` got a hit, or if fewer than 10 refused vectors got a hit (the harness must prove it
  sees fetches). Prints `svg-fetch-live: accepted 0 fetched, refused K fetched (harness live)`.
- `--json FILE` on `svg-fetch-live.js` writes `{vector: [contexts that fetched]}`.

## Behaviour and edge cases

- Go uses `encoding/xml` as the rules file says. Where `encoding/xml` accepts input a strict
  XML 1.0 parser refuses (check: unquoted attributes, undefined entities, `--` in comments,
  attributes repeated), add the explicit check, and add a vector for it.
- The order of rules and the rule ids are exactly the rules file's. The first failure wins.
- Go and JS must agree on every vector; `TestSvgVectorsFile` (Go) and `svg-vectors.js` (JS)
  both iterate the whole file and fail naming the vector, expected and actual rule.
- Every `apps/**/*.svg` in the repo must be accepted (a Go test iterates them).
- `artRefusal`'s SVG branch keeps its `b == nil` prefix-only pass.

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./... && go test -run 'Svg|ArtRefusal|BundleArt|SiteImport' .` → pass.
2. `sh docs/specs/s3-gotest.sh` → whole Go suite green.
3. `make check-editor` → passes, and its output includes `svg-vectors: N/N passed` and the studio suite's `all green`.
4. `make check-svg-live` → passes with `accepted 0 fetched`.
5. `make check-pictures` and `make check-web` → pass.
6. `python3 -c "import json;v=json.load(open('tools/orb-manager/testdata/svg-vectors.json'));print(len(v))"` → at least 70.

## Out of scope

- orb-os-net; raster rules; the 10-byte floor (a later step); `resolveOrigin`; any app's bytes.
- Do not modify: `tools/orb-manager/contract/`, `apps/`, `editor.go`, `security.go`,
  `docs/specs/fu2-svg-rules.md`.
