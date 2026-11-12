# H3 hub step 3: a shelf theme is askable by name — and a rename at the same version gets through

## Goal

Slice H3, hub step 3 (docs/specs/h3-design.md §7.2, §7.3, §5.4). Two halves, one sentence of
the gate: an orb whose theme came from a **file** (no `.origin.json`) or from a copy the hub
never recorded is asked about **by name**, adopted into the tracked set when the site answers,
and the answer's rename — even at the same version, which is what *Rename them now* produces
— reaches the Updates tab as an ordinary update that renames. A homemade name is never asked
about. The site-side contract is §4 of the design doc; the fake site in these tests answers it
(this chain never waits on the site chain).

## Read first

- `docs/specs/h3-design.md` §4 (the answers a fake site must mimic), §7.2, §7.3, §5.4.
- `tools/orb-manager/updates.go` — `themeUpdateRows` (the `!ok || o == nil` continue at ~218,
  the Gone/NotYet arms at ~231/~240, the `ans.Version <= o.Version` skip at ~246, the `Renamed`
  computation at ~256), `applyThemeUpdate`'s same gate at ~498 and its three gated steps below,
  and `updateBake` (which re-fetches at `ans.Version` — the reason a same-version rename needs
  no new bake code).
- `tools/orb-manager/siteimport.go` — `resolveOrigin` (the client to copy: the Gone/NotYet
  classification, `siteBearerFor`/`liveSiteLink`, `siteAllowed`, `resolveSaysGone`, the body
  ceiling), `ThemeOrigin`, `themeOriginPath`, `writeThemeOrigin`, `readThemeOrigin`.
- `tools/orb-manager/sitelimits.go` — `sharedNameSep`, `checkSharedName` (the exact parser:
  name + ` by @` + handle, first separator, refusals as a sentence), `themeNameOK`.
- `tools/orb-manager/sitelink.go` — `liveSiteLink()`, `siteBearerFor`, and how the link records
  the site.
- `tools/orb-manager/updates_test.go` / `adopt_test.go` — how a themed shelf is built in a test
  and how rows are asserted.

## Contract

- `resolveName(ctx, site, app, name string) (*resolveAnswer, error)` in `siteimport.go` beside
  `resolveOrigin`: `GET <site>/api/hub/resolve?site=<site>&app=<app>&name=<encoded>` — **the
  `app` parameter is required by the door (review 3), so the client always sends it**, from the
  theme entry's app (`ThemeEntry.App`, internal/orbnet/themes.go:61; the library entry's own app
  for a shelf theme). Same headers as `resolveOrigin`, same `siteAllowed(site)` gate before the
  socket opens, same **`siteBearerFor(site)` rule (sitelink.go:211): the link's token when it
  has one, no `Authorization` header at all when it does not**, same `resolveSaysGone` body
  ceiling, and the **same classification table** — JSON `{"gone":true}` on 404/410 is Gone (the
  site answers misses and gone designs with that one body, §4; there is no
  `{"error":"ambiguous"}` on the wire anymore), any other 404/410 is NotYet, a redirect is an
  error, 2xx must carry a non-empty `designId` and a `version ≥ 1` (else error), transport
  failure is an error. No new HTTP machinery.
- **The site asked is the configured one** (review 6): `siteReleaseBase()` (release.go:70), the
  same base a bake is fetched from, **whether or not this hub's link points there** — one
  origin of truth, and a ` by @<someone>` name from elsewhere is simply a miss at the hub's
  site. The theme never carries a site to ask, and no host outside the gate is ever opened.
- `themeUpdateRows`: the `!ok || o == nil` arm becomes **ask-by-name** when: a configured site
  exists and passes `siteAllowed`, the title parses through `checkSharedName` with **both**
  halves present (`name != "" && handle != ""`), the entry has a non-empty app, and no
  `<path>.origin.gone` marker sits beside it. Then:
  - **Resolved** → `writeThemeOrigin` with `{Site: siteReleaseBase(), DesignID: ans.DesignID,
    Version: ans.Version, ImportedAt: now}` (adoption happens in the row pass; the list is the
    only place a name is asked — re-read the theme entry after writing so the rest of the loop
    body runs on the record, as if it had always been there).
  - **Gone** → write the `.origin.gone` marker and **continue with no row** (review 5): the
    marker *is* the news, and a row that reappears whenever the tab opens is the noise the
    review called it. The marker's presence skips the theme in every later pass (no request, no
    row), and one hub log line names the theme. The record path's existing Gone row (updates.go's
    own shape) is untouched — the silence is the by-name path's policy, not the site's news.
  - **NotYet / error** → **one log line (`orb-manager: site resolve: …`), no row, no sidecar,
    no marker** — *ask again next time*, exactly as the inbox pass treats a transport failure.
    The `can't trace` row G3 drew on this path is **dropped**.
  A title that fails `checkSharedName` (a homemade name, or a plain one with no ` by @`), or an
  entry with no app, is skipped exactly as the `nil` arm skipped everything before.
- **The pure-rename gate** (§7.3): the version skip reads
  `if ans.Version < o.Version || (ans.Version == o.Version && (ans.SharedName == "" ||
  ans.SharedName == t.Title)) { continue }` — equal version + different current shared name
  falls through to the ordinary row with `Renamed` already computed; `>` stays as today.
  Same-version skips store `SkippedVersion = ans.Version` exactly as newer ones do (the
  existing skip door writes it), so **Skip** silences a rename until the name moves again,
  and Keep (the sidecar delete) retires the theme from asking as it does now.
  `applyThemeUpdate`: the gate at ~498 is the same expression; the "no newer version" error
  only answers when neither moved; the app gate (~502) is unchanged (a rename never changes
  app). The code comment states the gate as h3-design.md §0 words it: the site renames nothing
  today (ruling 29; H4's Replace case rule), so the equal-version row in practice arrives from
  the handle-change migration — the row and the apply are still written for the general case,
  and `TestSiteRenameVersionName` proves that arm.
- No web changes beyond none: the Themes-tab Updates rows and the Rename wording are G3's and
  already render `From`/`To`/`Renamed`.

## Behaviour and edge cases

- **Adoption**: a shelf theme titled `Skeleton by @greg` (app `clock`), no sidecar; the fake
  site answers the name 200 (`Skeleton by @g`, v6 — the retired-handle shape, which is the
  site's job to provide and the fake mimics): after one row pass the sidecar exists with exactly
  those fields, and a **second** pass sends no name request at all (the recorded path is taken).
  The fake asserts every by-name request carried `app=clock` and went to the configured-site
  base, and that it bore no `Authorization` when the link has no token.
- **Homemade**: `My Doodad`, `Plain`, `weird @@ name`, ` by @greg` (empty name half), and an
  entry whose app is empty → zero requests for them, rows as today (nothing), sidecar paths
  absent.
- **No link, configured site set** (review 6): the ask still happens anonymously and adoption
  still works.
- **Gate host**: `siteAllowed` false → zero requests, no rows, one log line.
- **Gone once**: fake answers `{"gone":true}` → **no row**, the marker exists, and a second pass
  neither asks nor lists it. Deleting the marker restarts the asking.
- **NotYet is silent**: a plain 404 (not the gone body), and a transport failure, each produce no
  row, no marker, no sidecar — and the next pass asks again (request count proves it). This
  replaces round 1's `can't trace` row expectation (review 5).
- **Same-version rename row**: sidecar at v6, title `Skeleton by @greg`; fake resolve answers
  v6 with shared name `Skeleton by @g` → exactly one row, `Renamed:true`, `From`/`To` the two
  names, `Version:6`; pressing **Skip** records `SkippedVersion:6` and the next pass lists
  nothing; a later v7 answer lists it again.
- **A version named differently from its predecessor** (review 14's general arm): sidecar at v6,
  title `Old by @greg`; fake resolve answers **v6** with shared name `New by @greg` → one rename
  row `Old by @greg → New by @greg`. The site never builds such a version itself (ruling 29);
  the test proves the hub answers the answer, not the history.
- **Apply on the equal-version row**: `updateBake` re-fetches the bundle at the same `n` and
  the bake's name becomes the answer's shared name; the orb is re-installed under the new name
  and the old removed, with the same three-step ordering the existing apply tests assert
  (reuse their rig: the assertions about the wear list and the shelf rename are byte-identical
  to today's — the gate is the only thing that moved).
- **Apply with neither version nor name moved** answers today's "no newer version" sentence
  unchanged.
- A row built from an adopted theme is **keyed by design id**, so the existing skip/keep doors
  and the apply door work on it with no change of their own.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd tools/orb-manager &&
   go vet ./... && go test -run 'TestSite(ResolveName|NameAdopt|NameHomemade|NameNoLink|
   NameGateHost|NameGoneOnce|NameSilent|RenameSameVersionRow|RenameSkip|RenameVersionName|
   RenameApply)' -count=1 -v .` → green with exactly those names covering the bullets above
   (request counts and header/param assertions on the fake).
2. `cd tools/orb-manager && go test -run 'TestSiteUpdates|TestSiteKeep|TestSiteImport|TestSiteInbox' -count=1 .`
   → green, those files unedited (G3's and step 2's behavior is unchanged where it should be).
3. `grep -q 'func (s \*Server) resolveName' tools/orb-manager/siteimport.go && grep -q
   'origin.gone' tools/orb-manager/updates.go && grep -q 'ans.SharedName == ""'
   tools/orb-manager/updates.go && ! grep -q 'ambiguous' tools/orb-manager/updates.go` → exit 0
   (the can't-trace sentence no longer appears in the row builder; `resolveSaysGone` keeps its
   own copy in `siteimport.go`, which the negation deliberately does not scan).
4. `cd tools/orb-manager && go test ./... -count=1 && cd ../.. && make check-apps check-hub
   check-web check-editor check-runner check-migrations check-editor-origin` → green.
5. `root="$(git rev-parse --show-toplevel)"; ! git -C "$root" diff --name-only
   delegate/h3-hub-2-inbox-client | grep -qvE '^tools/orb-manager/' && git -C "$root" diff
   --exit-code delegate/h3-hub-2-inbox-client -- tools/orb-manager/web` → exit 0 — the scope and
   no-UI gates measured against **hub step 2's branch**, run from the repo root (review 15: a
   cwd-relative pathspec inside `tools/orb-manager` would match nothing and pass by accident).

## Out of scope

- No new UI, no new doors, no changes to `/api/site/inbox*` (step 2), no site-side change, no
  orb-only enumeration (step 4). Do not touch `web/`, `state.go`, `sitelink.go` (the link's
  shape is H4's) or the editor import flow; `updates.go`'s `Renamed` computation, the wear-list
  walk and the three gated steps are untouched by construction — the diff here is one parser
  call, one marker check, one sidecar write, one log line, and two relaxed comparisons.

## Notes

Ask-by-name asks the hub's one configured site, and the bearer it sends is the one that site's
link handed it, if any: the hub never discovers a site from a handle, and a name from elsewhere
is a miss rather than a second origin. `checkSharedName`'s first-separator rule is the site's
own writing rule read back, so the hub never invents a split the site would not have made. The
`.gone` marker is deliberately a file beside the sidecar path (same directory, same lifecycle as
the theme): `removeThemeOrigin` takes it with the theme; a state-file entry would be a second
source. Silence on a miss is what makes the by-name path safe to run every minute against a
shelf of strangers' themes — review 5's whole point.

