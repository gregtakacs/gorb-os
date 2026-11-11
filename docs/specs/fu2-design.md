# fu2 design: gorb-os follow-ups 2–6 and the site's half of fu2

Slice **fu2** of orb-os-net `docs/design.md` §0, rulings 35, 37 (the "As built (R37)" bullet)
and 39, as amended 2026-11-11. Item 1 (the SVG checker: `docs/specs/fu2-svg-rules.md`,
`docs/specs/fu2-1-svg-parse.md`) is binding, already specified, and being built now on
`delegate/fu2-1-svg-parse`; this design's gorb-os steps start from its result and do not re-open
it. This is a **design**: one slice design, a spec per step, and two `delegate chain` plans. No
product code here.

The five items split into four gorb-os steps and two site steps. G5 (ruling 38, the hub's
per-app editor-origin relaxation) is a separate slice and is out of scope, as is any change
outside `docs/specs/`.

## Chains

- gorb-os: `docs/specs/fu2-chain.json`, `"repo": "../.."`, `"base": "delegate/fu2-1-svg-parse"`,
  `"retries": 2`, `"timeout": 5400`, four hard steps `fu2-2`…`fu2-5`. Verify runs in image
  `delegate-worker:gorb2` (`export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH`), pipes
  under `set -o pipefail`, and each step runs its own tests plus `sh docs/specs/s3-gotest.sh` and
  the make targets it touches (`check-apps check-hub check-editor check-web check-runner
  check-migrations check-editor-origin`).
- site: `docs/specs/fu2-site-chain.json`,
  `"repo": "/home/greg/docker/worktrees/orb-os-net-fu2-lead"`, `"base": "fu2"`,
  `"service": "postgres"`, `"read": ["GORB_FU2_FINAL"]` (a literal placeholder the lead replaces
  with the gorb-os checkout holding the finished `fu2-2..5`), bare spec file names, verifies
  imitating `w4b-chain.json` (`npm ci`, `npx prisma generate && npx prisma migrate deploy`,
  `typecheck`, `lint`, `test`, `build`). Its two steps carry the TS half of `fu2-svg-rules.md` and
  item 2's floor plus the byte-for-byte contract-fixture copy. The site's svg-vectors and
  editor-api fixtures are copied **out of `GORB_FU2_FINAL`** by the worker, never hand-written.

## Item 2 — a raster payload under 10 bytes is refused (ruling 35)

Ruling 35, as amended 2026-11-11: "A payload under 10 bytes is refused … 10 bytes is the smallest
header that can state a size, so anything shorter cannot be a picture. The contract fixtures carry
real, tiny images instead."

**Hub (`tools/orb-manager/sitelimits.go`, step fu2-2).** `artRefusal` today lets a raster of
`len(b) < artMinHeaderBytes` through (`return ""`, sitelimits.go:345) so the hub's contract
placeholders — `designDoc`'s `strings.Repeat("A", art)` — survive, and the site lets them through.
That pass-through becomes a refusal: a raster under `artMinHeaderBytes` now answers
`artRefusalUnreadable` (`"an image in it can't be read as a PNG, JPEG, GIF or WebP"`). The two
paths are split explicitly so a ref that carries **no bytes** is never refused for being short:
- `artPrefixRefusal(prefix string) string` — checks only that the prefix names a type a design may
  hold (one of `artPrefixes`, or `svgArtPrefix`); `""`/sentence, no bytes seen.
- `artRefusal(prefix, b []byte)` — SVG prefix: `b == nil` → `artPrefixRefusal`, else `checkSvgArt`.
  Raster prefix: unknown → `artRefusalType`; `len(b) < 10` → `artRefusalUnreadable`; unreadable
  header → `artRefusalUnreadable`; side over `artMaxSide` → the size sentence; else `""`.

`checkInlineArt`'s prefix-only pass (sitelimits.go:386, `artRefusal(prefix, nil)`) calls
`artPrefixRefusal`; `checkSidecarArt` keeps passing real bytes. Tests for both paths: the nil rows
move to a `TestArtPrefixRefusal*`, and `TestArtRefusalSentences` gains a sub-10-byte raster row
expecting `artRefusalUnreadable`. `artMinHeaderBytes` stays 10.

**Studio (`web/runtime/studio@1/studio.js`, step fu2-2).** `S.fitPicture` rejects a picked file
under 10 bytes with its existing "not a picture" error, before it reads or decodes the file: a
picture under 10 bytes cannot exist, so there is nothing for the picker to scale.

**Contract fixtures (step fu2-2, hub).** `designDoc` in `designs_test.go:66` builds a
`data:image/png;base64,` art field from `strings.Repeat("A", art)`; the contract scenario
(`contract_test.go` `scenario()`) calls it with `art` of 8, 12, 4, 1 and 6 (via `theme()`), all
under 10 decoded bytes. The scenario's assertions (`assertScenario`) read only step count,
per-step status, `format`, entry names/states and `made_with` — never the art bytes — so replacing
the placeholder with a **real tiny image** (a valid 1×1 or 2×2 PNG, ≥ 10 bytes) keeps every
assertion true. `designDoc` is changed to embed real, valid, minimal PNGs chosen per `art` value so
each body is a readable header within `artMaxSide`. Regenerate with `make -C tools/orb-manager
contract` (that target sets `contractWriteEnv`; `tools/orb-manager/contract/editor-api.json` is
never hand-edited — the Go test re-encodes and byte-compares). Other tests that pass small
`designDoc` `art` values through a door that now enforces the floor (`designs_test.go`, and the
large round-trip assertion at `designs_test.go:138`) have their **test inputs** fixed to real tiny
PNGs; the rule is not loosened for them.

**Site (steps fu2-site-1/2).** `src/lib/design-assets.ts` `artRefusal` line 89
`if (bytes.length < ART_MIN_HEADER_BYTES) return null;` becomes `return ART_UNREADABLE;`; a
prefix-only `artPrefixRefusal` is split out and `refPrefixRefusal` (which today passes
`EMPTY_BYTES`) calls it, so a reused ref with no bytes is still not refused for length. The tests
that assert the opposite today — `tests/design-assets.test.ts` "keeps a non-canonical payload, a
data URL too short to declare anything" (the `AAAAAAAA`/`###` inline cases now throw) and "is
silent about a payload too short" — are rewritten to the new rule. The site copies the new
`editor-api.json` **byte for byte** from
`GORB_FU2_FINAL/tools/orb-manager/contract/editor-api.json` to `tests/fixtures/editor-api.json`;
`tests/contract.test.ts` passes with **no other change** (it replays the recorded steps against the
site's own save route, which now enforces the floor, and the recorded art is real images so the
save succeeds).

## Item 3 — import accepts https://www.orb-os.net too (ruling 35)

Ruling 35, www added 2026-11-11: "Orb Manager imports only from known sites: the configured site
(`GORB_SITE`), `orb-os.net` and `www.orb-os.net`." https only; any other subdomain still refused.

**Hub (step fu2-3).** `siteAllowed` (sitelimits.go:60) is true only for `hostport == orbOSNet`. It
also becomes true for `https://www.orb-os.net` (and `:443`, stripped by `siteOriginParts`), https
only, loopback unchanged; `api.orb-os.net` and `orb-os.net.evil.example` stay refused.
`siteNotAllowed`'s sentence keeps its shape and its allowed side becomes `orb-os.net/www.orb-os.net`
so the owner recognises both. `resolveOrigin` (siteimport.go:625) already gates through
`siteAllowed`, so a `www.orb-os.net` origin sidecar resolves and any other subdomain is refused
before a socket opens. Tests: `TestSiteAllowed*` gains www rows; the before-any-fetch test adds a
`www.orb-os.net` accepted and `api.orb-os.net` refused (zero requests) case; a `resolveOrigin` test
proves a `www.orb-os.net` origin passes the gate with zero requests.

## Item 4 — a library name that breaks the basic-character rule gets a rename prompt (ruling 35)

An entry whose name holds `ö`, `✨` or `/` must still **open, list and delete** under its old name —
already true, because `designName` (`designs.go:55`) and the forget door apply no character rule.
But saving it goes through the character check: the studio's `api.checkName`
(`theme@1/session.js:378`, the `NAME_BASIC`/`NAME_CHARS` sentence) refuses before it posts, and the
hub's `checkNewName` (`libentries.go:214` → `checkThemeName`) refuses at the door when the save's
name is not already held exactly — so the owner hits a "Not saved: a design's name can use only
letters …" dead end on a design they already own.

**Flow (step fu2-4).** Saving such an entry does **not** fail with an error. When the open design
already exists in the library under a name that breaks the rule, the studio offers a **rename
prompt** instead of the flat refusal, pre-filled with a cleaned suggestion; saving under the new
name goes through the **normal checked door** (`checkNewName`/`checkThemeName`, unchanged). The
cleaned suggestion is computed in the studio (which holds `lib`) and mirrors the hub rule exactly:
fold accents toward their basic letter where one exists, drop `✨` and other no-mapping symbols,
treat `/` and other separators as nothing, collapse runs of spaces to one, trim leading/trailing
space and period, fall back to `"design"` if empty, then make it unique **case-insensitively**
against the app's library (suffix ` 2`, ` 3`, … like the site's remix suffix), never empty. The
prompt replaces the old `Not saved:` line. A Go test seeds a library entry with a non-basic name
and asserts it lists/opens/forgets under the old name, a save under the old name does not
hard-fail, and a save under the cleaned name succeeds through `checkNewName`. A studio suite case
drives the prompt for a seeded bad name and asserts the suggestion. The exact refusal site and the
copy are provisional: the lead will check the behaviour live and may adjust the wording.

## Item 5 — the SDK doc records ruling 37 as built (support@1 blocked features, eval relaxation)

**Doc (step fu2-3).** The SDK doc that lists the editor runtimes is `apps/README.md` (the `editor`
field row and the "Theme designs, schemas and migrations" section, `apps/README.md:49`). A sentence
is added there matching R37 as built: on orb-os.net an editor whose `editor.json` lists `support@1`
gets `'unsafe-eval'` on its own origin only, and two `support@1` features stay blocked by the
studio CSP — its helmet mount's inline `<script>` (needs `'unsafe-inline'`) and its x-import from
other origins (`connect-src 'self'` stays). `babel@7.29.0` editors likewise get `'unsafe-eval'` on
their own origin only. The relaxation follows the release being served. Documentation only; no CSP
code moves (the site already builds these headers under R37; the hub mirror is G5, out of scope).

## Item 6 — `make check-apps` enforces ruling 39, applied to this slice (ruling 39)

Ruling 39: a shelf `.orbapp`, or the hub's firmware bundle, that differs **in bytes** from the
newest release tag's copy while its `version.txt` is **not higher** than at that tag is a gate
failure.

**New gate (step fu2-5).** A new `tools/check_app_versions.py`, added to the `check-apps` target
after `rebake_shelf.py --check`:
- find the newest reachable release tag matching `vX.Y.Z` (numeric triple compare; ignore
  suffixed hub-rebuild tags like `v1.2.9-2`); at this HEAD that is `v1.7.0`.
- for each shelf app (`tools/orb-manager/apps/<slug>.orbapp` — the ones `make_factory_apps.py`
  lists: clock/flights/weather/radar/color_picker) and the hub's firmware bundle
  (`tools/orb-manager/internal/firmware/bundle/orb-firmware.zip`, versioned by the tree's root
  `version.txt`): if the working-tree bytes differ from the tag's, require the app's `version.txt`
  (root `version.txt` for firmware) to be higher than at the tag; else fail with a sentence naming
  the app (or `"firmware"`) and **both** versions.
- if no release tag is reachable, say so and **fail** rather than silently pass.

**CI (step fu2-5).** The `apps` job in `.github/workflows/ci.yml` checks out at default depth (1,
no tags), so the check would find no tag and fail; add `with: { fetch-depth: 0 }` to that job's
`actions/checkout@v7` (the `firmware` job already has it, ci.yml:82) so it is honest locally and in
CI.

**Applied to this slice (step fu2-5, last gorb step).** Clock, flights and weather are already
`v1.7.1` at `c648c00` and stay `v1.7.1` if rebuilt (higher than the tag's `v1.7.0`). Items 2–5
change hub runtimes (`studio@1`, `theme@1`) and docs, not any app's packed editor files, so no
`.orbapp` bytes should move — the step runs `make apps` and `tools/rebake_shelf.py`, then the new
check, to confirm. If a shelf app's bytes **do** change and it is not one of the three, bump its
`apps/<slug>/version.txt` to `v1.7.1` and rebuild. `make check-apps` must pass at the end.

## Gates per item (summary)

- fu2-2 (item 2): `go test -run 'ArtRefusal|ArtPrefixRefusal|Svg|BundleArt|SiteImport'`,
  `make check-pictures`, `make contract` idempotent, `sh docs/specs/s3-gotest.sh`,
  `make check-editor check-web`.
- fu2-3 (items 3+5): `go test -run 'SiteAllowed|SiteImport|ResolveOrigin|ThemeOrigin'`, doc grep on
  `apps/README.md`, `sh docs/specs/s3-gotest.sh`, `make check-hub`.
- fu2-4 (item 4): `go test -run 'Rename|ThemeName|Design|LibEntry|Keep|Forget'`,
  `make check-editor check-web`, `sh docs/specs/s3-gotest.sh`.
- fu2-5 (item 6): `make check-apps`, a forced `check_app_versions.py` failure/recovery case,
  `sh docs/specs/s3-gotest.sh`; ci.yml has `fetch-depth: 0` on the `apps` job.
- fu2-site-1: `npx vitest run tests/svg-art.test.ts` (loads the copied vectors + their sha), full
  suite, typecheck, lint, build.
- fu2-site-2: `npx vitest run tests/design-assets.test.ts tests/contract.test.ts`, full suite,
  typecheck, lint, build; the copied fixture is byte-identical to `GORB_FU2_FINAL`'s.

## Owner questions

1. **Item 4 — the rename prompt's copy.** A: reuse `NAME_CHARS` verbatim, appending the suggestion
   inline. B: a distinct sentence naming the old name and the suggested new one. **Chosen: A**
   (least new copy; the lead said wording may change live). The mechanism does not hinge on it.
2. **Item 6 — suffixed rebuild tags (`v1.2.9-2`).** A: only `^v\d+\.\d+\.\d+$` tags count.
   B: strip the `-N`. **Chosen: A** — a rebuild tag is not a new release; the newest `vX.Y.Z`
   reachable (`v1.7.0`) is the reference.
3. **Item 6 — firmware version source.** The firmware bundle has no `version.txt`; its release
   version is the tree's root `version.txt`. **Chosen:** compare firmware against root
   `version.txt` (consistent with RELEASING.md, which refuses a bundle whose version is not
   `version.txt`).
4. **Item 3 — sentence shape.** **Chosen:** name both allowed public hosts as
   `orb-os.net/www.orb-os.net` (matches how the rule reads in `design.md`) rather than two names
   joined by "and".

