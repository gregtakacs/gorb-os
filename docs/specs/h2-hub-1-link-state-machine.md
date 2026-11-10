# H2 hub step 1: `sitelink.go` — the connect state machine, its listener, and its token file

> **Read `docs/specs/h2-design.md`'s "Lead review" block first: it is binding and overrides this spec where they disagree.** For this step: points 5, 6 and 9. The file shape is `{site, token?, linkedAt, handle?, disconnected?}`; `GET /api/site/link-status` answers `state` none/pending/linked/disconnected (never the token); a press while pending replaces the attempt; a wrong path/method/Host/state at the callback is a 400 that does NOT end the attempt; after a successful exchange an older token is revoked best effort; the token goes only to the recorded site while it equals `siteReleaseBase()`. Add `TestSiteConnectWrongStateKeepsWaiting`, `TestSiteConnectRevokesTheOldToken`, `TestSiteLinkRoutesRefuseEditorCreds` (editor token + editor Host on every new route → refused) and, in `TestSiteDoorShutWithoutTheFlag`, a fake site that must see zero requests.

## Goal

Slice H2 of the orb-os-net design (`docs/specs/h2-design.md`), hub step 1 of 3.
Orb Manager can start a connect, hear its own loopback callback, exchange the
code for a token against the wire contract (§2.3), and store that token — the
whole RFC 8252 client minus UI, which is step 3. After this step
`TestSiteConnectEndToEnd` drives hub → fake site → hub file → bearer and finds a
live link.

## Read first

- `docs/specs/h2-design.md` §2 (the whole contract — the hub may rely on this
  file alone, never on orb-os-net sources), §5 (the hub surface this step builds).
- `siteimport.go` — `siteHTTP()` (~432, the no-redirects client you reuse for the
  exchange POST), `s.jsonOK`/`s.jsonErr` (firmware.go ~255–277: the
  `{"ok":true,"data":…}` / `{"ok":false,"error":…}` envelope),
  `routesSiteImport` (~636, the registration shape),
  the `siteLinkRefused` one-sentence-error style.
- `state.go` — the `s.statePath` field tests overwrite (~132), `saveState`'s
  MkdirAll-0700 + tmp-and-rename dance, `s.installID`/`mintInstallID`/`armState`,
  and `state_test.go`'s `TestStateFileHoldsNoSecrets` — the test your new file
  must keep green.
- `main.go` — the `-site` flag and `s.siteHidden` (164–167, 575), the
  `if !s.siteHidden` registration block (~1366), `var version` (78).
- `siteimport_test.go` — `siteServer(t, handler)` (~39, the recording fake site)
  and `TestSiteDoorShutWithoutTheFlag` (~828, the closed-door shape to copy).
- `release.go` — `siteReleaseBase()`: the ONLY way the hub names a site origin.

## Contract

- New file `sitelink.go` (+ `sitelink_test.go`):
  - `func (s *Server) siteLinkPath() string` —
    `filepath.Join(filepath.Dir(s.statePath), "site-link.json")`: a test that
    sets `s.statePath` moves it, and `TestStateFileHoldsNoSecrets` never sees
    the new file because it is a sibling of `state.json`, not inside it.
  - `type siteLink struct { Token string; Site string; LinkedAt int64 }` with
    JSON tags `token`, `site`, `linkedAt`; `loadSiteLink() *siteLink` (missing or
    unparseable → nil, never fatal); `saveSiteLink` whole-file 0600, tmp-and-
    rename like `saveState`; `clearSiteLink` (missing file counts as success).
    The token lives in this file and nowhere else — not `state.json`, not the
    log, not any `/api` answer (later routes answer `linked: true`, never the
    credential).
  - `func (s *Server) siteConnectURL(port int, verifier, state string) string`
    — pure; exactly h2-design.md §5's URL: base `siteReleaseBase()`, params in
    the order install,label,platform,version,redirect,challenge,state;
    `platform` from `runtime.GOOS` (windows→windows, darwin→macos, else linux);
    label from `os.Hostname()` trimmed to 64 bytes on a rune boundary (an empty
    or failing hostname labels itself `this computer`); version = `version`;
    challenge = base64url of the SHA-256 of the verifier's ASCII bytes;
    redirect = `http://127.0.0.1:<port>/callback`. Build the query by hand so
    the order is code, not map luck, and pin it in a test.
  - `startSiteConnect` (POST /api/site/connect): a connect already open is closed and replaced; then verifier =
    32 random bytes base64url, state = 16 random bytes hex, `net.Listen` on
    `127.0.0.1:0`, one handler serving ONLY `GET /callback` (everything else
    404 plain text), a 10-minute idle death, single-use; `go http.Serve(...)`;
    answer `{"ok":true,"data":{"url":…}}` and hand the same URL to
    `openBrowser` (a browser that will not open is not this call's failure).

  - The callback handler: `error=access_denied` → a `not connected` page,
    attempt ended, nothing stored; `state` mismatch (or absent), another path, method or Host → 400, the attempt keeps waiting; `code` → POST `{"code":…,"verifier":…}` to
    `<siteReleaseBase()>/api/hub/token` with `siteHTTP()`, content-type JSON,
    10-second timeout, body read capped at 8 KB; 200 with a `token` field →
    `saveSiteLink` (with the site origin and `time.Now()`) and a plain
    `Orb Manager is connected` page that invites the operator to close the tab;
    401 (or anything else) → `that sign-in did not go through: press Connect
    again` page. One listener, one attempt, dead afterwards either way.
  - `GET /api/site/link-status` → `{"ok":true,"data":{"linked": …}}`, the bool
    from `loadSiteLink() != nil`, and nothing else about it.
- Registration inside the existing `if !s.siteHidden` block (main.go ~1366):
  `s.routesSiteLink(mux)` in a new `routesSiteLink`, both routes
  `s.requireControl(...)`. The editor-API listener (`routesEditorListener` in
  `editororigin.go` ~512) gains NOTHING: the extend-`TestEditorAPIOnlyTheListedRoutes`
  criterion below pins that.
- A `siteLinkRand`-shaped seam is NOT wanted: the flow is already injectable at
  its real seams (fake site via `GORB_SITE` at an httptest loopback origin,
  browser via never calling `openBrowser` — take it out of the test path by
  having the handler return the URL it built; the test drives the callback URL
  itself).

## Behaviour and edge cases

- `GORB_SITE=http://127.0.0.1:<fake>` (the rule release.go already implements for
  loopback http) is the whole hook for end-to-end tests; no new env, no flag.
- Relink replaces on the SITE; the hub just overwrites its one credential file —
  and the old bearer dying is exactly why step 2's routes must re-read the file
  per call instead of caching a copy in the `Server`.
- Two callbacks arriving (browser refresh): the second finds no live attempt and
  gets the plain 404; the page text is never "connected" twice.
- A hub with `statePath` under a fresh temp dir creates the dir (0700) before
  writing 0600, exactly as `saveState` learned to.
- The exchange POST follows no redirects (`siteHTTP` refuses them; a 302 from a
  hostile "site" is a failed sign-in, page says so, nothing stored).
- `TestSiteDoorShutWithoutTheFlag` grows: with the flag off, `/api/site/connect`
  and `/api/site/link-status` answer like every other never-registered route,
  and `siteHidden`'s UI signal is unchanged.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd
   tools/orb-manager && go vet ./... && go test -run
   'TestSiteLink|TestSiteConnect|TestStateFileHoldsNoSecrets|TestSiteDoorShut'
   -v .` → every named test PASSes (new tests: `TestSiteLinkFileRoundTrip`
   (0600, dir 0700, unreadable file → nil, clear is idempotent),
   `TestSiteConnectURLIsTheContract` (fixed state/verifier inputs, exact param
   order, platform map, label truncation on a multi-byte hostname),
   `TestSiteConnectEndToEnd` (fake site implements §2.1–2.3 including the S256
   check of the real verifier; two installs → two links, each replacement seen),
   `TestSiteConnectStateMismatchStoresNothing`, `TestSiteConnectSingleUse`,
   `TestSiteDoorShutWithoutTheFlag` extended).
2. `go test ./... -count=1` (or `make check` if the toolchain probe prefers it)
   → whole suite green, including every H1/G3 site-import test.
3. `grep -q 'site-link.json' sitelink.go && git diff --exit-code h2 -- state.go
   siteimport.go web internal` → exit 0: the credential has one home, and the
   state file, the H1 importer and the web dir did not move.

## Out of scope

- No designs/disconnect/import-by-id routes (step 2), no HTML/JS (step 3). Do
  not touch `siteimport.go`, `web/**`, or the editor listener.
- No token refresh, no expiry logic in the hub: the site owns the 90 days, and
  the hub learns by being 401'd.
- Do not start the callback listener on boot: it exists between the press and
  the death of an attempt, and not one second longer.

## Notes

- The fake site in tests must verify `sha256(verifier) == challenge` before it
  hands out a token — that is the wire contract doing its PKCE job in CI, and
  the whole reason the hub's fake site is written from h2-design.md §2 alone.
- h2-design.md is the orb-os-net repo's file; the lead copies it into this repo
  under `docs/specs/` before this chain runs, which is why it is citable here.
- Keep `openBrowser` out of tests: the URL the handler RETURNS is the tested
  artifact; a test that opened a browser would be a test that ends the world's
  patience.

