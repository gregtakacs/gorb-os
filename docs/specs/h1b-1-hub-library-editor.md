# Hub: an app's editor with no orb (`/editor/hub/<slug>/`), and the origin on `/api/library` theme rows

## Goal

Slice H1b of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 32
and §7.1). **Import from orb-os.net needs no orb.** H1 baked an import with the orb's own
editor (`/editor/<ip>/<slug>/?import=<ticket>`), so it needed an orb that runs the app. From
now on Orb Manager bakes with **its own copy** of the app's editor: the editor region inside
the `.orbapp` its app library holds (`s.library().Container(slug)`). That is the same file the
per-orb door already serves when the orb runs the hub's release (`libraryEditorRegion`), and
the same file `orb-manager extract-editors` unpacks.

This step is Go only:

1. a new editor door, `GET /editor/hub/<slug>/<file>`, that serves the hub library's editor
   for an app and never dials an orb;
2. `GET /api/library`'s theme rows carry the theme's origin record, so a list with no orb
   (step h1b-3) can say where a theme came from.

No JavaScript, HTML or CSS changes in this step.

## Read first

- `tools/orb-manager/editor.go`, all of it, especially:
  - `routesEditor` (~line 75) and the memo types `editorKey`, `editorSlot`, `regions` (~80–105);
  - `editorRegionDialed` (~line 210): the memo rule, and how a library region's slot is
    keyed on `ContainerStamp` so a rebuilt `.orbapp` is served at once;
  - `libraryEditorRegion` (~line 293): reading the shelf copy's region;
  - `serveEditor` (~line 380): the path checks, the manifest/entry resolution, hosted
    editors, the runtime check, the launch token on HTML documents only, `no-store`;
  - `editorRefused`, `hostedEditor`, `serveHostedEditor`.
- `tools/orb-manager/runtime.go` lines ~160–200: `editorVersions`, `runtimeRefused`.
- `tools/orb-manager/library.go`: `Library.Container`, `ContainerStamp` (~360–395),
  `LibraryFile` (~141), `h_library` (~221), `listThemeLibrary` (~318).
- `tools/orb-manager/siteimport.go` lines ~455–505: `ThemeOrigin`, `themeOriginPath`,
  `readThemeOrigin`.
- `tools/orb-manager/editor_test.go`: `editorHub`, `editorGet` (~53–80) and
  `TestEditorIsServedFromTheLibraryWhenItIsTheOrbsRelease` (~367), the pattern to imitate.
- `tools/orb-manager/editor_hosted_test.go`: how a hosted (`host@1`) editor page is checked.
- `tools/orb-manager/library_test.go`: `cloneFixture`, `TestLibraryRouteIsWiredAndShaped`.
- `tools/orb-manager/apps/*.orbapp`: the shipped apps. `weather.orbapp` and `flights.orbapp`
  carry hosted (`host@1`) editors; `tests/fixtures/sentinel.orbapp` carries an entry-page one.

## Contract

### The door: `GET /editor/hub/<slug>/<file>`

- Register `mux.Handle("/editor/hub/", s.requireControl(http.HandlerFunc(s.serveHubEditor)))`
  in `routesEditor`, beside `/editor/`. Go's mux picks the longer pattern, so `/editor/hub/…`
  never reaches `serveEditor`. Same guard as `/editor/`.
- `serveHubEditor` takes `<slug>` and `<file>` from the rest of the path exactly as
  `serveEditor` does: `orbnet.AppStemOK(slug)` or 404 `not an app name`; a file containing
  `..` or starting with `/` → 404; the same `path.Clean` normalisation; a bare
  `/editor/hub/<slug>/` (or no trailing file) means the manifest.
- **There is no orb.** No `checkedTarget`, no `installedRow`, no dial, no `editorCache`.
  The region comes from the hub's library only:
  - `body, err := s.library().Container(slug)`. A read error → `editorRefused(w, r, err)`
    (502). `body == nil` (the library has no `<slug>.orbapp`, or there is no library) →
    `editorRefused(w, r, fmt.Errorf("%w: Orb Manager carries no %s app", orbnet.ErrNoEditor, slug))`,
    which answers 404.
  - `orbnet.ParseApp(body)` failure → `editorRefused` with that error (502).
  - `orbnet.AppEditorRegion(body)`: an error → `editorRefused` with it; an empty region →
    the same 404 as above but with the sentence `Orb Manager’s copy of the <slug> app has no editor`
    (wrapping `orbnet.ErrNoEditor`).
  - Not a ZIP → `editorRefused` with `the library editor region for <slug> is not a ZIP archive: <err>`.
  - Ignore `Compiled`/version: whatever release the library holds is the editor Orb Manager
    carries.
- **Memo.** Keep the parsed reader in the existing `regions` memo under the key
  `editorKey{ip: "hub", slug: slug}`, with `lib: s.library().ContainerStamp(slug)`. A slot is
  reused only while its stamp equals the current `ContainerStamp(slug)` and it is younger than
  `editorFetchTTL`; an error slot is never reused (re-read every time: it's a local disk read).
  `editorInvalidate()` drops it like every other slot. `dropEditorRegions(ip)` doesn't need to
  know about it (`"hub"` is never an orb address).
- **Serving.** From the reader on, serve exactly as `serveEditor` does: the manifest/entry
  resolution, `hostedEditor` / `serveHostedEditor`, `editorRuntimeMissing` → `runtimeRefused`
  for HTML documents, the launch token on HTML documents only, `Cache-Control: no-store`,
  the content type, `http.ServeContent`. **Factor that tail out** of `serveEditor` into one
  function both doors call, for example
  `func (s *Server) serveEditorFile(w http.ResponseWriter, r *http.Request, ip, slug, file string, zr *zip.Reader)`.
  `serveEditor`'s behaviour must not change at all; every existing editor test must pass
  unedited.
- **Versions in refusals.** `serveHostedEditor` and `runtimeRefused` take the version pair
  from `s.editorVersions(ip, slug)`. Make `editorVersions` skip `installedRow` when `ip` is
  `""` or `"hub"` (it must not call `s.device` at all then), so a hub with no orbs never
  dials anything from this door. The hub door passes `"hub"`.

### `GET /api/library`: the origin on theme rows

- `LibraryFile` gains `Origin *ThemeOrigin \`json:"origin,omitempty"\``.
- `listThemeLibrary` sets it for each `.orbtheme` row from
  `readThemeOrigin(filepath.Join(dir, e.Name()))`. App rows never carry it. A theme with no
  (or an unreadable) sidecar has no `origin` key at all.
- The sidecar files (`*.orbtheme.origin.json`) must not show up as rows or as refusals in
  either listing (check: `listThemeLibrary` filters on the `.orbtheme` extension; keep it so).

### README

`tools/orb-manager/README.md`, in the editor section that describes `/editor/<ip>/<slug>/`
(search for `/editor/`): add 2–4 lines saying that `/editor/hub/<slug>/` serves Orb Manager's
own copy of an app's editor, from the `.orbapp` in its app library, with no orb involved, and
that Import from orb-os.net bakes with it. Match the README's tone. Don't edit the
"Import from orb-os.net" section yet (step h1b-3 rewrites it).

## Behaviour and edge cases

- A hub with **no orbs at all** (no `s.device` dial ever succeeds, the fleet is empty) serves
  `/editor/hub/weather/` as a 200 HTML page with the launch token, and every module of it.
- `/editor/hub/nosuchapp/` → 404 with `Orb Manager carries no nosuchapp app`; for
  `Accept: text/html` the existing `editorRefused` HTML page with that sentence.
- `/editor/hub/` with no slug, or `/editor/hub/Bad Name/` → 404 (`not an app name`).
- `/editor/hub/weather/../../x` → 404.
- No token → the same refusal `requireControl` gives `/editor/`.
- Replacing `apps/<slug>.orbapp` on disk is served on the next request (the stamp rule).
- `/editor/<ip>/<slug>/…` is untouched: same answers, same dials, same memo slots.

## Acceptance criteria

From `tools/orb-manager`, with `GOFLAGS=-buildvcs=false` and Go at `/usr/local/go/bin/go`:

1. `go vet ./...` clean.
2. `go test -run 'HubEditor|LibraryOrigin' -v .` passes, with new tests in a new file
   `tools/orb-manager/editor_hub_test.go`:
   - `TestHubEditorServesTheLibrarysEditorWithNoOrb`: a `Server` with **no orbfake** (build
     it the way `libHub` does but without `fakeOrbHub`; give it a token and `s.lib` with an
     `AppsDir` holding a copy of `apps/weather.orbapp` and `tests/fixtures/sentinel.orbapp`).
     `/editor/hub/weather/` → 200, `text/html`, the launch token set, the page names the
     app's module; one module file → 200 without the token; `/editor/hub/sentinel/` → the
     sentinel's entry document, byte-equal to its member in the region. If `s.device` is nil
     here, nothing panics; if the constructor gives a device, assert it was never dialed.
   - `TestHubEditorRefusesWhatItDoesNotCarry`: unknown slug → 404 with the sentence; an HTML
     Accept → the refusal page containing the sentence; a bad name → 404; `..` → 404; no
     token → refused.
   - `TestHubEditorLetsGoWhenTheFileChanges`: serve, rewrite the `.orbapp` with another
     app's bytes (or a changed mtime and size), serve again → the new bytes.
   - `TestLibraryOriginOnThemeRows`: a themes dir with two `.orbtheme` files (use any theme
     the existing tests already build or ship), one with a `.origin.json` sidecar written by
     `writeThemeOrigin`; `GET /api/library` → that row has `origin` with
     `site`, `designId`, `version`, `importedAt`; the other row has no `origin` key; the
     sidecar is neither a row nor a refusal.
3. `go test ./...` passes (every existing test unedited).
4. `git diff h1 -- tools/orb-manager/contract/` is empty.

## Out of scope

- Any JavaScript, HTML, CSS (steps h1b-2 and h1b-3).
- `siteimport.go`'s doors: `/api/site/import`, `/api/site/bundle`, `/api/site/keep` already
  need no orb. Don't change them.
- `/api/editor/source`, Rebake all, Edit, the per-orb door's rules.
- Uploading to the site (it needs H2's linking; not built).
