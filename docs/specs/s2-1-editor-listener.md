# Hub: app editors on their own origin (`<app>.localhost`, a second loopback port) with a per-launch editor token

## Goal

Slice **S2** of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, ruling 33,
§6.6 and the S2 row of §12). Today Orb Manager serves every app editor at `/editor/<ip>/<slug>/`
and `/editor/hub/<slug>/` on the **same origin as its control API** (`127.0.0.1:3779`). A page
on that origin passes both control checks (the `orb_token` SameSite=Strict cookie and the origin
check), so an editor's JavaScript could call `/api/flash`, provision, erase, install apps or
use the LAN proxy. A1 will run outside developers' editors, so this must end.

This step (Go only) moves every editor onto **its own origin**:

- a **second HTTP listener** on loopback (default `127.0.0.1:<control port + 1>`, i.e. 3780);
- **one origin per app**: `http://<label>.localhost:<editor port>`, where `<label>` is the app
  slug with `_` replaced by `-` (`clock` → `clock.localhost`, `color_picker` → `color-picker.localhost`).
  Different hosts are different origins and different *sites*, so one app's editor cannot read
  another's pages or storage, and the control cookie (host `127.0.0.1`) is never sent there;
- a **per-launch, per-app editor token**, distinct from the control token;
- on that listener, **only** the editor files, the runtime, and the narrow **editor API**:
  the routes an editor genuinely calls (list below). Everything else 404s there;
- the control listener **stops serving** `/editor/...` entirely.

Steps 2–4 follow: s2-2 adds per-app scope checks on the editor API and hardens the control
side; s2-3 moves the browser code (app.js iframe, postMessage) to the new origin; s2-4 is a
live Chromium test. Do not do their work here.

## Read first

- `~/docker/Projects/orb-os-net/docs/design.md` §6.6 (the Orb Manager row) and ruling 33 — why.
- `tools/orb-manager/security.go` lines 1–170: `tokenHeader`, `tokenCookie`, `newLaunchToken`,
  `setLaunchToken`, `requireControl`, `tokenOK`, `originOK`, `reject`. Imitate their style
  (long explanatory comments, `reject` envelope).
- `tools/orb-manager/editor.go`, all of it: `routesEditor`, `serveEditor`, `serveHubEditor`,
  `serveEditorFile` (sets the launch cookie on HTML docs today), `editorRefused`,
  `hostedEditor`, `serveHostedEditor`, `handleEditorSource`.
- `tools/orb-manager/runtime.go` lines 90–140: `routesRuntime`, `serveRuntime`.
- `tools/orb-manager/main.go`: flags (~460–520), `bindHub` (~985), the server start and
  shutdown (~800–920), `routes` (~1105–1290), `handleIndex`, `Server` struct (~81), and
  `identity.go` `newHub` (~105).
- `tools/orb-manager/web/runtime/host@1/page.html` — the hosted editor page template.
- `tools/orb-manager/proxy.go` `proxyOrb`, `security.go` `routeAllowed`/`mayDial`.
- Existing tests that request editors: `editor_test.go` (helpers `editorHub`, `editorGet`,
  `cookieNamed`), `editor_hub_test.go` (`editorAcceptGet`), `editor_hosted_test.go`,
  `runtime_test.go`, `themeapps_test.go`, `contract_test.go`, `diskcache_test.go`,
  `extract_test.go`, `ui_test.go`, `window_test.go`. Find them all with
  `grep -ln '/editor/' tools/orb-manager/*_test.go`.

## Contract

### New file `tools/orb-manager/editororigin.go`

Constants:

```go
const (
	editorTokenHeader = "X-Orb-Editor" // the editor API credential, sent by the page's fetch shim
	editorTokenQuery  = "et"           // the same token on the editor document's URL (an iframe src cannot carry a header)
)
```

`Server` gains fields (in `main.go`'s struct): `editorKey []byte` (32 random bytes),
`editorPort int`, `controlPort int`. `newHub()` fills `editorKey` from `crypto/rand` and sets
`controlPort = listenPort`, `editorPort = listenPort + 1`, so tests get sane values. `main()`
overwrites both ports with the real bound ports.

Functions (exact names; tests in later steps use them):

- `func editorHostLabel(slug string) string` — `strings.ReplaceAll(slug, "_", "-")`.
- `func (s *Server) editorOrigin(slug string) string` — `"http://" + editorHostLabel(slug) + ".localhost:" + strconv.Itoa(s.editorPort)`.
- `func (s *Server) editorToken(slug string) string` — `slug + "." + hex(HMAC-SHA256(s.editorKey, "orb-editor\x00"+slug))[:32]`.
  Per launch (the key is per process) and per app. It never equals `s.token`.
- `func (s *Server) editorTokenSlug(tok string) (string, bool)` — splits at the LAST `.`,
  checks the slug with `orbnet.AppStemOK`, recomputes and `subtle.ConstantTimeCompare`s.
  Empty key or empty token → false.
- `func (s *Server) editorHandler() http.Handler` — the whole editor listener: a guard
  (below) in front of a private `*http.ServeMux` holding only the editor routes. Tests call
  `s.editorHandler().ServeHTTP(rec, req)`.

### The editor listener's guard (every request)

1. **Host.** `r.Host` must be exactly `<label>.localhost:<s.editorPort>` (case-insensitive) for
   some label made only of `[a-z0-9-]` (1–32 chars). Anything else (an IP, `localhost:3780`,
   `evil.example:3780`, a wrong port) → **421** with the `reject` envelope and text
   `this is Orb Manager's editor port: it answers only <app>.localhost:<port>` (DNS-rebinding
   defence). The label is the request's **scope**.
2. **Never the control credential.** The guard never reads `orb_token` or `X-Orb-Token`, and
   no response on this listener ever sets a cookie.
3. **Headers on every response** (set before the route runs, including refusals):
   - `Content-Security-Policy: default-src 'self'; script-src 'self' 'unsafe-inline' 'unsafe-eval' blob:; style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; media-src 'self' data: blob:; connect-src 'self' data: blob:; worker-src 'self' blob:; frame-src 'self' blob: data:; object-src 'none'; base-uri 'self'; form-action 'self'; frame-ancestors 'self' http://127.0.0.1:<controlPort> http://localhost:<controlPort>`
   - `Referrer-Policy: no-referrer`
   - `X-Content-Type-Options: nosniff`
4. **Credential**, by route class:
   - **Static** (`/editor/...`, `/runtime/...`): method must be GET or HEAD (else 405).
     Allowed when ANY of: (a) the `et` query parameter is a valid editor token; (b) the
     `X-Orb-Editor` header is a valid editor token; (c) `Sec-Fetch-Site: same-origin`;
     (d) no `Sec-Fetch-Site` header and the `Referer` header's scheme+host+port equals this
     request's editor origin. Otherwise **403** `the editor's files load only inside its own page`.
     (Sub-resources of the editor page are same-origin requests; the document itself is a
     cross-site iframe load from the hub page, so it carries the token in its URL.)
   - **API** (everything under `/api/` and `/orb/`): the `X-Orb-Editor` header must be a valid
     editor token (the `et` query is NOT accepted for API calls). If an `Origin` header is
     present it must equal the request's editor origin exactly; if `Sec-Fetch-Site` is present
     it must be `same-origin`. Failures → **403** with texts
     `missing or wrong X-Orb-Editor: this route answers only the editor page this hub opened`
     and `cross-origin request refused: the editor API answers only its own editor page`.
   - In both classes, a valid token whose `editorHostLabel(slug)` differs from the Host's label
     is **wrong** (403, same text as a missing token). The token's slug is the request's
     **scope slug** (attach it to the request context; s2-2 uses it).
5. **Path slug.** For `/editor/<ipv4>/<slug>/...` and `/editor/hub/<slug>/...` the path's slug
   must equal the scope slug → else 404 `not this app's editor`.

### The editor listener's routes (the editor API, exactly these)

Static:
- `GET /editor/<ipv4>/<slug>/<file>` → existing `serveEditor` logic (orb's editor, or the hub's copy per `editorFromLibrary`)
- `GET /editor/hub/<slug>/<file>` → existing `serveHubEditor` logic
- `GET /runtime/<id>/<file>` → existing `serveRuntime`

API (each calls the SAME handler function the control mux uses, but WITHOUT `requireControl`;
the guard above replaces it):
- `GET /api/library/entries`, `GET /api/library/design`, `POST /api/design/save`,
  `POST /api/library/keep`, `POST /api/library/forget`
- `GET /api/theme/design`, `GET /api/theme/stem`, `GET /api/editor/source`
- `GET /api/orb/known`, `POST /api/theme/upload`, `POST /api/theme/commit`, `GET /api/theme/upload-status`
- `GET /api/site/bundle`, `POST /api/site/keep`
- `GET /api/maps/source` (GET only here; POST, which sets the source, stays control-only → 405), `GET /api/maps/fetch`
- `GET /orb/<ipv4>/api/v1/home`, `GET /orb/<ipv4>/api/v1/places`, `GET /orb/<ipv4>/api/v1/units`
  — and NO other `/orb/` path or method (→ 404). Exact paths, no extra segments. After the
  check, hand the request to the existing `proxyOrb` (which still applies `mayDial`).

Anything else on the editor listener → 404 (plain `http.NotFound`, or the `reject` envelope with 404 for `/api/`).

Enforce the method per route on the editor listener (GET routes refuse POST with 405 and
vice versa), even where the shared handler is lenient.

### HTML documents: the fetch shim, and no cookie

`serveEditorFile` and `serveHostedEditor` stop calling `setLaunchToken` (delete those calls).
Instead every HTML document served from the editor listener gets a **fetch shim** as its first
script, so the editor's same-origin `fetch` calls carry the token without any editor changing:

```html
<script>(function(){var T=<TOKEN as a JSON string>,O=location.origin,F=window.fetch;window.fetch=function(i,o){var q=new Request(i,o);try{if(new URL(q.url,location.href).origin===O)q.headers.set('X-Orb-Editor',T);}catch(e){}return F.call(this,q);};})();</script>
```

- **Do not edit `host@1/page.html`.** It ships to the site and to the migration runner
  (`web/runtime/_runner/migrate.js` `hostedPageHtml` refuses any template action it does not
  know, and orb-os-net's checker runs that runner). Insert the shim into the **rendered** bytes
  instead, exactly as for any other HTML document below.
- Every HTML document the editor listener serves (the hosted page after `hostedTmpl.Execute`, a
  legacy `entry` editor, an `index.html`): insert the `<script>` immediately after the first
  `<head ...>` tag (case-insensitive); if there is none, after the `<!doctype ...>` declaration;
  if neither, at the very start. Serve the modified bytes. Build the token literal with
  `json.Marshal` (the token is `[a-z0-9_-]+\.[0-9a-f]{32}`, so it is safe in a script).
- The hub's own error pages (`editorRefused`, `runtimeRefused`) need no shim.

The token is the editor's own; the page may read it. It opens only that app's editor API.

### The control listener

- `routesEditor` registers only `GET /api/editor/source` (still `requireControl`; app.js uses it).
  The `/editor/` and `/editor/hub/` patterns are **removed** from the control mux, so
  `GET /editor/...` on the control port now 404s (the `/` handler already 404s non-root paths).
- New control route `GET /api/editor/origin?app=<slug>` (`requireControl`): slug checked with
  `orbnet.AppStemOK` (else `s.jsonErr` `expected ?app=<app>`), answers
  `s.jsonOK(w, map[string]any{"origin": s.editorOrigin(slug), "token": s.editorToken(slug)})`.
  This is how the hub page (s2-3) learns where to point the editor iframe.
- `/runtime/` stays on the control mux too (tabs.js loads `/runtime/maps@1/orbmap.js`).
- Nothing else about the control side changes in this step.

### `main.go` wiring

- New flag `-editor-addr` (string, default empty = `127.0.0.1:<bound control port + 1>`),
  usage: `host:port for the app editors' own origin (<app>.localhost); loopback only`.
- After the control listener is bound: if the editor address's host is not loopback
  (`isLoopbackHost`) → `log.Fatalf` (editors are served to this machine only, even with
  `-allow-remote`). Bind it; if the bind fails, log
  `orb-manager: editor port <addr> is taken (<err>); editors use an ephemeral port this launch`
  and bind `127.0.0.1:0` instead. Set `s.controlPort` and `s.editorPort` from the real
  listeners. Log `orb-manager: app editors on http://<app>.localhost:<port>`.
- Serve it with `&http.Server{Handler: s.log(s.editorHandler())}` in a goroutine; shut it down
  wherever the control server is shut down (same bounded context).

## Behaviour and edge cases

- `GET http://clock.localhost:3780/editor/hub/clock/?app=clock&et=<clock token>` → 200 hosted
  page with the shim first in `<head>`, no `Set-Cookie`, the CSP header.
- Same URL with the `color_picker` token → 403. With no `et` and `Sec-Fetch-Site: cross-site` → 403.
- `GET http://clock.localhost:3780/runtime/theme@1/session.js` with `Sec-Fetch-Site: same-origin` → 200.
- `GET /editor/hub/weather/` on host `clock.localhost:3780` with the clock token → 404.
- `POST http://clock.localhost:3780/api/flash` with the clock token → 404 (not an editor route).
- `GET http://clock.localhost:3780/orb/192.0.2.7/api/v1/sd/ls` → 404; `POST .../api/v1/home` → 405 or 404.
- `GET http://127.0.0.1:3779/editor/hub/clock/` with the control token → 404.
- Request to the editor handler with Host `127.0.0.1:3780` → 421.
- A request with a valid token in `X-Orb-Token` (the control header) and nothing else → 403
  on the editor API (control credentials open nothing here).

## Tests

- **Migrate** every existing test that requests `/editor/...` to the editor listener. Change the
  shared helpers (`editorGet`, `editorAcceptGet`, any others) to build the request with
  `req.Host = editorHostLabel(slug)+".localhost:"+strconv.Itoa(s.editorPort)` (derive the slug
  from the path), add `et=<s.editorToken(slug)>` when the old helper sent a token, and serve it
  with `s.editorHandler()`. Tests that asserted the `orb_token` cookie on an editor document
  now assert the opposite: **no** `Set-Cookie` header at all, and the shim present in the HTML
  with that app's token. `TestEditorRequiresToken`-style tests now assert the 403 without `et`.
  Keep each test's intent; do not delete tests.
- **New** `tools/orb-manager/editororigin_test.go` with at least these (names exact):
  - `TestEditorOriginIsPerAppAndLoopback` — `editorOrigin("clock")`, `editorOrigin("color_picker")`; `/api/editor/origin` needs the control token and answers origin+token; the token differs from `s.token` and between apps.
  - `TestEditorTokenIsPerAppAndPerLaunch` — two `newHub()`s give different tokens for one slug; `editorTokenSlug` rejects a token from another hub, a tampered hex, a swapped slug.
  - `TestEditorListenerRefusesForeignHosts` — 421 for `127.0.0.1:<p>`, `localhost:<p>`, `evil.example:<p>`, `clock.localhost:<wrong port>`.
  - `TestEditorDocumentNeedsItsOwnToken` — et ok / missing / other app's / control token; no `Set-Cookie` on any; shim present with the right token; CSP and Referrer-Policy present.
  - `TestEditorSubresourcesLoadSameOriginOnly` — `/runtime/...` with `Sec-Fetch-Site: same-origin` 200, `cross-site` 403, no header + matching Referer 200, no header + other Referer 403.
  - `TestEditorAPIOnlyTheListedRoutes` — table of every listed API route answers non-404 with the right token+method; a table of control routes (`/api/flash`, `/api/provision`, `/api/orb/cmd`, `/api/lonely`, `/api/library/install`, `/api/apps/import`, `/api/orb/update`, `/api/library/push`, `/api/firmware`, `/api/orb/allow`, `/api/theme/install`, `/api/theme/delete`, `/api/fleet/set`, `/api/updates/apply`, `/api/site/import`, `/api/editor/origin`, `/api/status`) answers 404 on the editor listener even with the right token; `/orb/<ip>/api/v1/sd/ls` and `/orb/<ip>/api/v1/ota` 404; wrong method 405.
  - `TestEditorAPIRefusesWithoutItsHeader` — no header / `et` query only / control `X-Orb-Token` / wrong Origin / `Sec-Fetch-Site: cross-site` → 403.
  - `TestControlNoLongerServesEditors` — `/editor/<ip>/clock/` and `/editor/hub/clock/` on the control mux → 404 with the control token.
  - `TestLegacyEditorDocumentGetsTheShim` — an `index.html` with `<HEAD>` gets the script right after it; one with only a doctype gets it after the doctype.

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./...` → clean.
2. `go test -run 'EditorOrigin|EditorToken|EditorListener|EditorDocument|EditorSubresources|EditorAPI|ControlNoLonger|LegacyEditor' -v .` → the 9 tests above PASS.
3. `go test ./...` → all green (re-run `TestThePassParksForADemandAndResumesItsOwnQueue` alone if it is the only failure: it is a known flaky test).
4. `grep -n 'setLaunchToken' tools/orb-manager/editor.go` → no matches.
5. `git diff --stat s2 -- tools/orb-manager/contract apps tools/orb-manager/web/app.js tools/orb-manager/web/runtime/theme@1` → empty (nothing under those paths changes in this step; `s2` is the branch the chain starts from).

## Out of scope

- Per-app scope checks on query parameters, tickets and bodies (s2-2), control-side hardening
  (s2-2), app.js / theme@1 / host@1 JS changes (s2-3), the live browser test (s2-4).
- Do not modify: `tools/orb-manager/web/runtime/host@1/page.html`, `tools/orb-manager/contract/`, anything under `apps/`, `tools/orb-manager/web/app.js`,
  `tools/orb-manager/web/runtime/theme@1/`, `tools/orb-manager/web/runtime/host@1/host.js`.
- No CORS headers anywhere: the editor API is same-origin to its editor; nothing cross-origin is granted.

## Notes

- Go is `/usr/local/go/bin/go`; export `GOFLAGS=-buildvcs=false`.
- `*.localhost` resolves to loopback inside Chromium (and Firefox) without DNS; tests don't need resolution, they set `req.Host`.
- `httptest.NewRequest` sets Host `example.com`; editor-listener tests must set `req.Host` explicitly.
- Keep handlers shared: register the editor routes by calling the existing `s.handleX` methods; do not copy handler bodies.
