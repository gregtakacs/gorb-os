# Hub: the editor API reaches only its own app, and the control API refuses the editor origin and token

## Goal

Slice **S2** of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 33,
§6.6, §12 S2 row), step 2 of 4. Step s2-1 put every editor on its own origin
(`http://<label>.localhost:<editor port>`), with a per-launch per-app editor token
(`X-Orb-Editor`) and a narrow editor API (`tools/orb-manager/editororigin.go`). Read that file
first: it is the base of this step.

This step (Go only) finishes the server side of the gate:

1. **Per-app scope.** On the editor listener, a request can only read or write the app whose
   origin and token it carries (the "scope slug"): another app's library entries, designs,
   bakes, import tickets and theme uploads are refused. Origins already stop one editor from
   reading another's *pages*; this stops it from using the shared hub *data* through its own API.
2. **Control hardening.** The control API refuses anything carrying the editor token, stops
   treating `Sec-Fetch-Site: same-site` as its own origin, answers only loopback Host names, and
   its page cannot be framed.
3. **Gate tests** that prove §12's S2 gate in Go.
4. A short README section.

## Read first

- `tools/orb-manager/editororigin.go` and `editororigin_test.go` (from s2-1): the guard, the
  scope slug on the request context, the route table, `editorToken`, `editorOrigin`, `editorHandler`.
- `tools/orb-manager/security.go` lines 1–170: `requireControl`, `tokenOK`, `originOK`, `reject`.
- `tools/orb-manager/libentries.go` (`handleLibEntries`, `handleLibDesign`, `handleLibKeep`,
  `handleLibForget`), `designs.go` (`handleDesignSave`, `designAppRe`), `theme.go`
  (`handleThemeDesign` ~791, `handleThemeStem` ~886, `handleThemeUpload` ~924),
  `siteimport.go` (`siteTicketStore.get` ~330, `handleSiteBundle`, `handleSiteKeep` — note how
  keep checks `c.Sections[0] != t.app`), `editor.go` `handleEditorSource`.
- `tools/orb-manager/internal/orbnet/themecontainer.go` — `ContainerName`, `ThemeContainer.Sections`
  (`Sections[0]` is the app the theme belongs to).
- `tools/orb-manager/main.go` — `handleIndex`, the `allowRemote` flag, where the control server's
  handler is built (`s.log(mux)`).
- `tools/orb-manager/security_test.go` — how the existing control checks are tested (imitate).

## Contract

### 1. Scope checks on the editor listener (in `editororigin.go`)

Applied after the guard, before the shared handler. `S` = the scope slug. All refusals are
**403** in the `reject` JSON envelope `{"ok":false,"error":...}`. Texts verbatim:
- other app: `this editor is the S app's: it cannot reach the X app's themes`
- missing: `this editor is the S app's: name its app (?app=S)`

| Route | Rule |
|---|---|
| `GET /api/library/entries`, `GET /api/library/design`, `POST /api/design/save`, `POST /api/library/keep`, `POST /api/library/forget` | `?app=` required and must equal `S` |
| `POST /api/design/save` | additionally: if the JSON body's top-level `"app"` is a string, it must equal `S`. Read the body (cap it at the size `handleDesignSave` already allows, or 16 MiB if none), then restore `r.Body` for the handler |
| `POST /api/library/keep`, `POST /api/theme/upload` (with and without `precheck=1`) | the body must parse with `orbnet.ContainerName` and `Sections[0]` must equal `S` (cap the read at `orbnet.MaxThemeUpload`+1, restore `r.Body`). An unparsable body is passed through to the handler so its own error text stays as it is |
| `GET /api/theme/design` | buffer the handler's answer; if it is 200 JSON whose top-level `"app"` is a string other than `S`, replace it with the 403. Otherwise copy the buffered status, headers and body through unchanged |
| `GET /api/editor/source` | `?slug=` required and must equal `S` |
| `GET /api/theme/stem` | `?slug=` if present must equal `S`; if absent, add `slug=S` to the query before the handler runs (the handler defaults to `clock`) |
| `GET /api/site/bundle`, `POST /api/site/keep` | look the ticket up with the store's existing non-consuming `get` (add a `peek` if `get` consumes); a ticket whose `app` is not `S` → 403 `this editor is the S app's: that import is for the X app`. An unknown ticket is passed through (the handler answers its own "gone" text) |
| `GET /api/orb/known`, `POST /api/theme/commit`, `GET /api/theme/upload-status`, `GET /api/maps/source`, `GET /api/maps/fetch`, `GET /orb/<ip>/api/v1/{home,places,units}` | no app parameter; unchanged |

The same handlers on the **control** mux are unaffected (the hub page is trusted with every app).

### 2. Control-side hardening (`security.go`, `main.go`)

- `originOK`: `Sec-Fetch-Site: same-site` **no longer passes**; only `same-origin` (and the
  absent-header path that already exists). Comment why: `<app>.localhost` editors and any other
  loopback origin are "same-site" to some browsers' eyes only by port-blind rules, and the hub
  page is the only page that may drive the control API. Update any existing test that relied on
  `same-site` passing so it now expects 403, and say so in the test's comment.
- `requireControl`: before the existing checks, a request carrying an `X-Orb-Editor` header or
  an `et` query parameter → **403** `the editor token opens only the editor API, never Orb Manager's controls`.
  (The control token check would refuse it anyway; this makes the refusal explicit and logged.)
- New `func (s *Server) controlHostGuard(h http.Handler) http.Handler`: when the hub was NOT
  started with `-allow-remote` (add `allowRemote bool` to `Server`, set in `main`), the request's
  Host, port stripped, must be `127.0.0.1`, `localhost` or `::1` (bracketed form `[::1]`),
  case-insensitive. Anything else (including `clock.localhost`) → **421** with the `reject`
  envelope `this hub answers only http://127.0.0.1:<port> or http://localhost:<port>`. With
  `allowRemote` it passes everything through. Wire it in `main` around the control mux
  (`s.log(s.controlHostGuard(mux))`). Unit-test it directly; do NOT add it inside `routes()`
  (hundreds of tests use `httptest`'s default Host `example.com`).
- `handleIndex` adds `Content-Security-Policy: frame-ancestors 'self'` and
  `X-Frame-Options: SAMEORIGIN` (an editor must not be able to frame the hub's page and trick a
  click on Flash).

### 3. Gate tests — new file `tools/orb-manager/editor_gate_test.go` (names exact)

Shared setup: a `newHub()` with a granted orb IP (use `s.grants.learn`), an `orbfake` double
behind it where a test needs a device, and a helper that builds a request the way a browser on
the editor origin would send it to the **control** port: `Origin: http://clock.localhost:<editorPort>`,
`Sec-Fetch-Site: cross-site`, `X-Orb-Editor: <clock token>`, no cookie.

- `TestEditorTokenOpensNoControlRoute` — a table of EVERY route registered with `requireControl`
  in `routes()` and the `routesX` functions it calls (grep them; at least: `/api/flash`,
  `/api/flash/cancel`, `/api/provision`, `/api/orb/cmd`, `/api/lonely`, `/api/watch`,
  `/api/console`, `/api/firmware`, `/api/firmware/reset`, `/api/library/install`,
  `/api/library/rm`, `/api/library/restore`, `/api/library/push`, `/api/apps/import`,
  `/api/orb/allow`, `/api/orb/forget`, `/api/orb/add`, `/api/orb/update`, `/api/fleet/set`,
  `/api/fleet/forget`, `/api/theme/install`, `/api/theme/delete`, `/api/theme/upload`,
  `/api/site/import`, `/api/updates/apply`, `/api/editor/origin`, `/api/library/entries`,
  `/api/design/save`, `/orb/<ip>/api/v1/status`, `/orb/<ip>/api/v1/sd/delete`). Each is sent
  to the control mux three ways and must answer 403 every time, and the orbfake double must
  record **no** call: (a) the editor-origin request above; (b) the same with the clock token in
  `X-Orb-Token` instead (an editor copying its token into the control header); (c) the same
  with `?et=<token>`. Use POST for mutating routes and GET for the proxy ones.
- `TestEditorOriginRefusedEvenWithTheControlCookie` — the control cookie AND the right control
  token, but `Sec-Fetch-Site: same-site` + `Origin: http://clock.localhost:<p>` → 403 on
  `/api/flash`; with `Sec-Fetch-Site: same-origin` and `Origin` equal to the request Host → passes the check (not 403).
- `TestEditorListenerServesNoControlRoute` — the same route list on `s.editorHandler()` with the
  right Host and token → 404 or 405, never 2xx, double records no call.
- `TestEditorAPIScopeIsItsApp` — every row of the scope table: the other-app and missing cases
  → 403 with the exact text; the right-app case reaches the handler (not 403). Use real
  containers where needed: `orbfake.Container(name)` gives a theme container (check which app
  its `Sections[0]` names and build the other-app case accordingly), or the repo's shelf
  `.orbtheme` fixtures; create an import ticket through the store directly.
- `TestControlHostGuard` — `127.0.0.1:3779`, `localhost:3779`, `[::1]:3779` pass;
  `clock.localhost:3779`, `evil.example:3779`, `192.168.1.5:3779` → 421; with `allowRemote` all pass.
- `TestHubIndexRefusesFraming` — `GET /` carries both headers.

### 4. Docs

Add a section `## Editors run on their own origin` to `tools/orb-manager/README.md` (6–12 lines):
the editor port (`-editor-addr`, default control port + 1), the `<app>.localhost` origin per app,
the per-launch per-app token, what the editor API is (list the routes), that the control API
refuses the editor origin and token, and that editors need a browser that resolves
`*.localhost` (Chromium, Edge, Firefox — the app window is Chromium).

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./...` → clean.
2. `go test -run 'EditorTokenOpensNoControlRoute|EditorOriginRefusedEvenWithTheControlCookie|EditorListenerServesNoControlRoute|EditorAPIScopeIsItsApp|ControlHostGuard|HubIndexRefusesFraming' -v .` → 6 PASS.
3. `go test ./...` → all green (re-run `TestThePassParksForADemandAndResumesItsOwnQueue` alone if it is the only failure: known flaky).
4. `grep -n "same-site" tools/orb-manager/security.go` → only in comments, never as an accepted case.
5. `git diff --stat s2 -- tools/orb-manager/contract apps tools/orb-manager/web` → empty.

## Out of scope

- JavaScript, HTML, CSS (s2-3), the live browser test (s2-4).
- No CORS headers on either listener.
- Do not modify: `tools/orb-manager/contract/`, `apps/`, `tools/orb-manager/web/`.

## Notes

- Go is `/usr/local/go/bin/go`; export `GOFLAGS=-buildvcs=false`.
- Keep the shared handlers shared; the scope checks are wrappers in `editororigin.go`.
- If an existing test breaks because of the `same-site` change, fix the test, not the rule.
