# fu2-3: import accepts https://www.orb-os.net, and the SDK doc records ruling 37 as built

## Goal

Two small, independent gorb-os changes. (Item 3, ruling 35 as amended 2026-11-11:) Orb Manager
imports only from known sites — the configured site, `orb-os.net`, `www.orb-os.net`, and loopback —
so `https://www.orb-os.net` is accepted by the same door that already accepts `orb-os.net`, https
only, and every other subdomain (`api.orb-os.net`, `orb-os.net.evil.example`) is still refused
before a socket opens; the refusal sentence names both allowed hosts so the owner recognises them.
(Item 5, ruling 37 as built:) the SDK runtime doc records that on orb-os.net a `support@1` editor
gets `'unsafe-eval'` on its own origin only, with two `support@1` features still blocked by the
studio CSP. The hub's own CSP mirror of R37 is G5 and is out of scope; do not move CSP code, and do
not touch orb-os-net.

## Read first

- `docs/specs/fu2-design.md` §"Item 3" and §"Item 5".
- `tools/orb-manager/sitelimits.go`: `orbOSNet`, `siteAllowed` (~46–70 — the `hostport == orbOSNet`
  branch and the loopback branch), `siteOriginParts`, `siteNotAllowed`, and the two sentences that
  spell the allowed hosts.
- `tools/orb-manager/siteimport.go`: `parseSiteLink` (~94, how it canonicalises host[:port] and
  gates through `siteAllowed`), `resolveOrigin` (~618–640, how it calls `siteAllowed` and what it
  does with an origin sidecar), `handleSiteImport`.
- `tools/orb-manager/sitelimits_test.go`: `TestSiteAllowed*` (host rows, e.g. line ~36 asserting
  `https://evil.orb-os.net` is false) and `TestSiteAllowedRefusalSentences` (~61).
- `tools/orb-manager/siteimport_test.go`: `TestSiteImportRefusesAnotherHostBeforeAnyFetch`-style
  tests (the zero-requests assertion pattern).
- `apps/README.md`: the `editor` field row (~49) and the "Theme designs, schemas and migrations"
  section — where the doc sentence goes.
- How R37 reads as built: `docs/design.md` ruling 37 (in the site checkout, read-only) — the
  helmet/x-import blocks and the eval relaxation.

## Contract

### Item 3 — `tools/orb-manager/sitelimits.go`

- `siteAllowed(site string) bool` becomes true also for the canonical `https://www.orb-os.net`
  (and `https://www.orb-os.net:443`; `siteOriginParts` strips `:443`), https only. Loopback and the
  configured-site branches are unchanged. Any other subdomain of `orb-os.net` (e.g.
  `api.orb-os.net`) and any suffix look-alike (`orb-os.net.evil.example`) stay false.
- `siteNotAllowed`'s sentence (and the allowed-name side it prints) names both public hosts as
  `orb-os.net/www.orb-os.net`. Keep it one sentence and the `not from <host>` tail. Reuse the
  existing format constant; do not add a second resolver.
- `parseSiteLink` and `resolveOrigin` need **no logic change** — both already gate through
  `siteAllowed`; verify by test that a `www.orb-os.net` link/origin passes and a `www.`-look-alike
  does not, and that `resolveOrigin` opens no socket for a refused host.

### Item 5 — `apps/README.md`

Add one sentence (in the `editor`-field area) recording R37 as built: on orb-os.net an editor whose
`editor.json` lists `support@1` is served with `'unsafe-eval'` on its own origin only, and two
`support@1` features stay blocked by the studio CSP — its helmet mount's inline `<script>` (which
would need `'unsafe-inline'`) and its x-import from other origins (`connect-src 'self'` stays). Note
`babel@7.29.0` editors likewise get `'unsafe-eval'` on their own origin only, and that the
relaxation follows the release being served. Documentation only.

## Tests

- `sitelimits_test.go`: add rows — `https://www.orb-os.net` and `https://www.orb-os.net:443` → true;
  `http://www.orb-os.net` → false; `https://api.orb-os.net`, `https://orb-os.net.evil.example` →
  false; keep `https://orb-os.net` true and `https://evil.orb-os.net` false. Update
  `TestSiteAllowedRefusalSentences` to the `orb-os.net/www.orb-os.net` spelling.
- `siteimport_test.go`: the before-any-fetch test gains a `www.orb-os.net` accepted and an
  `api.orb-os.net` refused-with-zero-requests case; add/extend a `resolveOrigin` test asserting a
  `www.orb-os.net` origin sidecar passes the gate with zero requests to a refused host.

## Gate

`export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH` then: `go vet ./...`;
`go test -run 'SiteAllowed|SiteImport|ResolveOrigin|ThemeOrigin' -v` (www accepted, other subdomains
refused before fetch); `grep -q 'unsafe-eval' apps/README.md && grep -qi 'support@1' apps/README.md`;
`make check-hub`; `sh docs/specs/s3-gotest.sh`. Only `sitelimits.go`, its tests, `siteimport_test.go`
and `apps/README.md` change.

