# S3 step 1: Orb Manager imports only from known sites, and only art within the site's limits

## Goal

Slice S3 of orb-os-net's design (ruling 35), gorb-os half, step 1 of 2. Orb Manager's "Import
from orb-os.net" accepts a link only when its host is the configured site (`GORB_SITE`, the
same resolver G3's release check uses, default `https://orb.takacsfamily.com`) or `orb-os.net`,
plus loopback for tests and a developer's own site, as today. Any other host is refused before
any network request. And a bundle whose art is not a PNG, JPEG, GIF or WebP of at most
1024 × 1024 pixels (read from the image header, never decoded) is refused, with the same rules
the site now applies when it stores art.

## Read first

- `tools/orb-manager/siteimport.go`: all of it. `parseSiteLink` (~82), `siteHost`,
  `checkBundle` (~147), `checkBundleArt` (~201), `fetchSiteBundle`, `resolveOrigin` (~529),
  `handleSiteImport` (~587, how `checkBundle` errors are worded).
- `tools/orb-manager/release.go` lines 39–90: `defaultSiteBase` and `siteReleaseBase()`. Reuse
  `siteReleaseBase()`; do not write a second resolver.
- `tools/orb-manager/siteimport_test.go`: `goodBundle`, `bundleOf`, `siteServer`-style helpers
  (~40–100), `TestSiteLinkNamesOneDesignOnOneSite` (~139), `importErr`, the `siteClient` field
  on `Server` (main.go ~142) for injecting a client.
- `tools/orb-manager/release_test.go` ~340–365: how tests set `GORB_SITE` (`t.Setenv`).
- The site's implementation of the same art rules, to port: `/home/greg/docker/worktrees/orb-os-net-s3-lead/src/lib/image-header.ts`
  (`readImageHeader`, the exact parsing rules for PNG, GIF, JPEG and WebP) and
  `/home/greg/docker/worktrees/orb-os-net-s3-lead/src/lib/design-assets.ts` (`ART_PREFIXES`, `ART_MIN_HEADER_BYTES`,
  `artRefusal`, the data-URL regex `/^data:[^,\s]*,/i`). That checkout is mounted read-only; its
  `tests/image-header.test.ts` has the header test matrix to mirror.

## Contract

### New file `tools/orb-manager/sitelimits.go` (+ `sitelimits_test.go`)

```go
// orbOSNet is the site's future home (§11): always allowed, https only.
const orbOSNet = "orb-os.net"
// siteAllowed reports whether a canonical site origin (scheme://host[:port], as parseSiteLink
// answers it) is one Orb Manager may import from or ask about.
func siteAllowed(site string) bool
// siteNotAllowed is the refusal for a link to any other host.
func siteNotAllowed(site string) error
// imageHeader / readImageHeader: a Go port of the site's image-header.ts, same rules, no
// image package and no new module dependency.
type imageHeader struct { Type string; Width, Height int } // Type: "png" | "jpeg" | "gif" | "webp"
func readImageHeader(b []byte) (imageHeader, bool)
const artMaxSide = 1024
// artRefusal: "" when prefix and bytes are acceptable art, else the sentence.
func artRefusal(prefix string, b []byte) string
```

`siteAllowed(site)` is true when ANY of:
1. it equals the configured site: `siteReleaseBase()` canonicalised the way `parseSiteLink`
   canonicalises (scheme lower-case, host lower-case, port kept). For `https`, an explicit `:443`
   on either side is ignored for the comparison.
2. it is `https://orb-os.net` (or `https://orb-os.net:443`). Not `www.orb-os.net`, not another
   port, not http.
3. its hostname is loopback: `localhost`, `127.0.0.1` or `::1`, any port, http or https, exactly
   as `parseSiteLink` already lets plain http through to these.

`siteNotAllowed(site)` returns, verbatim (curly apostrophes not needed; `×` is U+00D7 elsewhere):

`Orb Manager imports themes only from <A> and orb-os.net, not from <host>`

where `<A>` is the configured site's host (`siteHost` of the canonical configured origin) and
`<host>` is `siteHost(site)`. When the configured site IS `orb-os.net`, the sentence is
`Orb Manager imports themes only from orb-os.net, not from <host>`.

Art sentences (Go), verbatim:
- type: `an image in it is not a PNG, JPEG, GIF or WebP, the only pictures a design can hold`
- unreadable: `an image in it can't be read as a PNG, JPEG, GIF or WebP`
- size: `an image in it is %d × %d pixels, and a design's pictures can be at most 1024 × 1024`

`artRefusal(prefix, b)`, the site's order:
1. `prefix`, lower-cased, not one of `data:image/png;base64`, `data:image/jpeg;base64`,
   `data:image/gif;base64`, `data:image/webp;base64` → type sentence.
2. `len(b) < 10` → `""` (too short to declare a size in any format; the site lets these through
   for the hub's contract placeholders, and the hub must accept what the site serves).
3. header unreadable → unreadable sentence.
4. either side over 1024 → size sentence.
5. otherwise `""`.

### Where it applies

1. **`parseSiteLink`**: after the canonical site is computed and before returning, a site that
   is not `siteAllowed` → `siteNotAllowed(site)`. So `handleSiteImport` refuses before
   `fetchSiteBundle` is ever called (no dial, no DNS).
2. **`resolveOrigin`**: an origin record whose `Site` is not `siteAllowed` → the error
   `siteNotAllowed(o.Site)` with no request made (a record written before S3 may name any host).
3. **`checkBundleArt`** (and so `checkBundle`): for every sidecar row, besides the existing hash
   checks, its `prefix` (a string; missing or not a string → the type sentence) and its decoded
   bytes go through `artRefusal`. Then walk the whole `design` JSON value (objects and arrays, at
   any depth, skipping the `orbAssets` key) for string values matching `(?i)^data:[^,\s]*,`;
   each one's prefix (before the first `,`) and its base64-decoded rest (`StdEncoding`; a decode
   error → unreadable sentence) go through `artRefusal`.
4. **The wording**: an art refusal must NOT read as "damaged". Return it as a distinct error
   (e.g. a `bundleArtError` type, or `errors.Is` on a sentinel), and have `handleSiteImport`
   answer `the theme from <host> can't be imported: <sentence>` for it. Every other
   `checkBundle` error keeps today's `the theme from <host> is damaged: …`.

## Behaviour and edge cases

- `https://evil.example/t/<22-char id>` → `Orb Manager imports themes only from orb.takacsfamily.com and orb-os.net, not from evil.example`, and the injected `siteClient`'s transport sees zero requests.
- `https://orb-os.net/t/<id>`, `orb.takacsfamily.com/t/<id>`, `http://127.0.0.1:<port>/t/<id>`, `http://[::1]:8080/t/<id>` keep parsing as today.
- With `GORB_SITE=https://site.example:8443`: `https://site.example:8443/t/<id>` is allowed, `https://site.example/t/<id>` and `https://orb.takacsfamily.com/t/<id>` are refused, `https://orb-os.net/t/<id>` is allowed.
- `GORB_SITE` invalid (e.g. `http://example.com`) → the release resolver's fallback (the default site) is what is allowed.
- A bundle whose sidecar art is a 1025 × 1 PNG → `the theme from 127.0.0.1:<port> can't be imported: an image in it is 1025 × 1 pixels, and a design's pictures can be at most 1024 × 1024`; nothing is held (no ticket).
- A sidecar prefix `data:image/svg+xml;base64` → the type sentence; an inline `data:image/svg+xml;utf8,<svg…>` anywhere in the design → the type sentence; a 50000 × 50000 PNG header in a ~60-byte inline data URL → the size sentence.
- A good bundle with 1024 × 1024 PNG art still imports end to end (import, bundle, keep).

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./... && go test -run 'SiteLink|SiteImport|SiteBundle|SiteKeep|ResolveOrigin|ThemeOrigin|SiteAllowed|ImageHeader|ArtRefusal|BundleArt' -v .` → all pass, including new tests:
   - `TestSiteAllowed*`: the hosts above, the `GORB_SITE` cases, `:443` equivalence, `www.orb-os.net` refused;
   - `TestSiteImportRefusesAnotherHostBeforeAnyFetch`: a `siteClient` whose `RoundTripper` counts calls; the count stays 0 and the sentence is exact;
   - `TestImageHeader*`: the same matrix as the site's `tests/image-header.test.ts` (PNG, GIF87a/89a with and without colour tables and an oversized frame, truncated GIF, JPEG with APP0/APP1 then SOF0, SOF2, fill bytes, SOS-before-SOF, WebP VP8/VP8L/VP8X, truncations, random bytes), headers built by hand;
   - `TestArtRefusal*` and `TestSiteImportRefusesArtOutsideTheLimits` (1025-wide PNG, SVG prefix, SVG inline, 50000 × 50000 bomb), each with the exact sentence;
   - `TestResolveOriginRefusesAnUnknownSite`: zero requests.
2. `sh docs/specs/s3-gotest.sh` → vet + the whole suite green (the known flaky
   `TestThePassParksForADemandAndResumesItsOwnQueue` is re-run alone if it is the only failure).

## Out of scope

- The editors (step 2). The site's own code.
- Do not modify: `tools/orb-manager/editor.go`, `tools/orb-manager/security.go`,
  `tools/orb-manager/web/` (anything), `tools/orb-manager/contract/`, `apps/`. Another lead is
  changing `editor.go`, `security.go` and `handleSiteBundle`/`handleSiteKeep`: keep your
  `siteimport.go` edits to `parseSiteLink`, `checkBundleArt`, `resolveOrigin` and the
  `checkBundle` error wording in `handleSiteImport`; put everything new in `sitelimits.go`.
- No new Go module dependency (`go.mod` / `go.sum` unchanged); no `image/*` package for headers.

## Notes

- Existing tests pass fake art (short strings) and a sidecar row with `"mimetype"` instead of
  `"prefix"`. Update the TEST helpers: `goodBundle` writes `"prefix": "data:image/png;base64"`
  and callers pass real PNG headers from one helper (e.g. `pngArt(w, h, seed int) []byte`:
  signature, IHDR, then filler; no CRC check, nothing decodes pixels). Keep `goodBundle`'s and
  `bundleOf`'s signatures.
- Go: `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH`.
