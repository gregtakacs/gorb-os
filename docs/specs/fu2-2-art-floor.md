# fu2-2: a picture under 10 bytes is refused everywhere a design's art is checked

## Goal

Ruling 35 (as amended 2026-11-11) in gorb-os: a raster payload shorter than `artMinHeaderBytes`
(10 bytes) is no longer waved through — it is refused as unreadable, because 10 bytes is the
smallest header that can state a size, so anything shorter cannot be a picture. The contract
fixtures that relied on the old pass-through stop being base64 of the letter `A` and become real,
tiny, valid PNGs. A reused ref (art with no bytes present) is still judged by its prefix alone and
is never refused for length. The studio's picture picker refuses a sub-10-byte file before it
reads it. The site's mirror of the floor is a later step (`fu2-site-2`); do not touch orb-os-net.

## Read first

- `docs/specs/fu2-design.md` §"Item 2".
- `tools/orb-manager/sitelimits.go`: `artMinHeaderBytes`, `artPrefixes`, `svgArtPrefix`,
  `artRefusalType`, `artRefusalUnreadable`, the size sentence, `artRefusal` (~325–350 — note the
  `b == nil` → `""` svg branch and the raster `len(b) < artMinHeaderBytes` → `""` branch),
  `checkInlineArt` (~380–390, its prefix-only pass `artRefusal(prefix, nil)`), `checkSidecarArt`.
- `tools/orb-manager/sitelimits_test.go`: `TestArtRefusalSentences` and any `artRefusal(_, nil)`
  row (these exercise the prefix-only path and move to the new function).
- `tools/orb-manager/designs_test.go`: `designDoc` (~66, embeds `strings.Repeat("A", art)`), the
  save round-trip that asserts the body comes back verbatim (~138, asserts `strings.Contains(...
  strings.Repeat("A", 9000))` — that one is a valid large raster, leave its intent, fix the input
  to a real ≥10-byte image).
- `tools/orb-manager/contract_test.go`: `scenario()` (~242) and the `theme()`/`designDoc(...)`
  calls with art 8/12/4/1/6; `assertScenario` (~reads step count, status, format, entry
  names/states, made_with — not art bytes).
- `tools/orb-manager/web/runtime/studio@1/studio.js`: `S.fitPicture` (~417+) and the existing
  "not a picture" error it raises.
- `docs/specs/fu2-svg-rules.md` (item 1, binding) for how `artRefusal` must treat the SVG prefix.

## Contract

### `tools/orb-manager/sitelimits.go`

- Add `artPrefixRefusal(prefix string) string`: `""` when `prefix` is one of `artPrefixes` or
  equals `svgArtPrefix`; otherwise `artRefusalType`. It looks at the prefix only, never bytes.
- Rewrite `artRefusal(prefix string, b []byte) string`:
  - if `prefix == svgArtPrefix`: return `artPrefixRefusal(svgArtPrefix)` when `b == nil`
    (which is `""`), else `checkSvgArt(b)`.
  - else if `artPrefixRefusal(prefix) != ""`: return `artRefusalType`.
  - else (a known raster prefix, bytes present): if `len(b) < artMinHeaderBytes` return
    `artRefusalUnreadable`; if `readImageHeader(b)` fails return `artRefusalUnreadable`; if a side
    exceeds `artMaxSide` return the size sentence; else `""`.
  Keep every existing sentence string exactly (only the sub-10-byte raster case changes from `""`
  to `artRefusalUnreadable`, and nil-svg stays `""`). `artMinHeaderBytes` stays 10.
- `checkInlineArt`: its prefix-only pass calls `artPrefixRefusal(prefix)` instead of
  `artRefusal(prefix, nil)`. `checkSidecarArt` still calls `artRefusal(prefix, b)` with real bytes.

### `tools/orb-manager/web/runtime/studio@1/studio.js`

`S.fitPicture(file)` returns its existing "not a picture" rejection when `file.size < 10`, before
`file.arrayBuffer()`/decode, so a truncated download cannot reach the canvas. Do not add a new
sentence; reuse the current one.

### Contract fixtures (`designs_test.go`, `contract_test.go`)

- `designDoc(name, ver, art int)`: instead of `strings.Repeat("A", art)`, embed a real, minimal,
  valid PNG whose decoded length is `>= artMinHeaderBytes` and whose header parses (e.g. a 1×1 or
  2×2 PNG). Pick the body from `art` so distinct `art` inputs still produce distinct, valid bodies
  (a hash/derived choice is fine); the value must be a readable header within `artMaxSide`. Keep
  the `data:image/png;base64,` prefix.
- Every test that sends a `designDoc` art under 10 through a door that now enforces the floor gets
  its **input** fixed (real tiny PNG), not the rule loosened.
- Regenerate `tools/orb-manager/contract/editor-api.json` with `make -C tools/orb-manager contract`
  (sets `contractWriteEnv`). It is never hand-edited: `TestContractFixture` re-encodes the scenario
  and byte-compares, so an unchanged scenario with new art bytes must re-encode equal or the step
  fails. Do not edit `assertScenario`.

## Tests

- `sitelimits_test.go`: move the `artRefusal(_, nil)` rows into `TestArtPrefixRefusalSentences`
  (assert svg/prefixes → `""`, unknown → `artRefusalType`). In `TestArtRefusalSentences` change the
  sub-10-byte raster expectation from `""` to `artRefusalUnreadable` and keep the unreadable/large/
  valid rows. Add a row: valid 1×1 PNG (≥10 bytes) → `""`.
- `contract_test.go` / `designs_test.go` pass unchanged in intent after the fixture regeneration.

## Gate

`export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH` then: `go vet ./...`;
`go test -run 'ArtRefusal|ArtPrefixRefusal|Svg|BundleArt|SiteImport|Contract' -v` (a sub-10 raster
is refused, a nil svg is not, the contract fixture re-encodes equal); `make -C tools/orb-manager
contract` leaves the fixture unchanged (`git diff --stat` clean on it); `make check-pictures`;
`make check-editor`; `make check-web`; `sh docs/specs/s3-gotest.sh`. `tools/orb-manager/contract/`
and `apps/` show no diff other than the regenerated fixture.

