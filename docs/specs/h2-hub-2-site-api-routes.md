# H2 hub step 2: `/api/site/designs`, disconnect, and Import by own-design id

> **Read `docs/specs/h2-design.md`'s "Lead review" block first: it is binding and overrides this spec where they disagree.** For this step: point 5. A 401 does not delete the file: it rewrites it as `{site, handle, disconnected: true}` (no token), so `link-status` says `disconnected` and the page can offer Reconnect; the error sentence stays `disconnected from orb-os.net`. Disconnect in Orb Manager deletes the file. Every outbound bearer call goes only to the recorded site while it equals `siteReleaseBase()`.

## Goal

Hub step 2 of 3. A linked hub proxies its owner's design list, imports any of
them through the H1 ticket pipeline by design id (the site's §2.6 own-bundle
door, bearer-authenticated), and forgets its token on demand or the moment the
site stops accepting it. This is everything the step-3 UI will call:
My orb-os.net could be driven by curl alone.

## Read first

- `docs/specs/h2-design.md` §2.5, §2.6, §5 — the calls, the bearer matrix, and
  the clear-on-401 rule.
- `siteimport.go` — `handleSiteImport` (~645: body read, cap, `siteLinkRefused`
  for junk), `fetchSiteBundle` (~451) and `checkBundle` (~153: already strict
  about id/format — import-by-id reuses both untouched), `siteTicketStore.add`,
  `ThemeOrigin`/`writeThemeOrigin` (~510–537), `siteHTTP` (~432).
- `sitelink.go` + `sitelink_test.go` as step 1 left them — the fake-site helper
  they grew is your test rig.
- `firmware.go` ~255–278 — the `s.jsonOK`/`s.jsonErr` envelope; passthrough
  answers go through the same encoder, never a hand-written `w.Write`.
- `editororigin.go` `routesEditorListener` (~512) — the whitelist these routes
  must stay OFF, and `editororigin_test.go:464` the test that proves it.

## Contract

- `GET /api/site/designs` (`s.requireControl`, only-when-`!siteHidden`): no
  stored token → `s.jsonErr("not connected to orb-os.net")`. GET
  `<siteReleaseBase()>/api/hub/designs` with `siteHTTP()`, `Authorization:
  Bearer <token>`, 10 s timeout, body capped 64 KB; 200 whose body parses as the
  §2.5 object → re-encode that object as `data` verbatim; **401 →
  `clearSiteLink()` first, then `s.jsonErr("disconnected from orb-os.net")`**;
  anything else (including unparseable) → `s.jsonErr("orb-os.net did not
  answer")`, token kept — a stall is not a revocation.
- `POST /api/site/disconnect`: best-effort `DELETE /api/hub/token` with the
  bearer and a 10 s timeout — its outcome is logged at debug level at most —
  then `clearSiteLink()`; `{"ok":true,"data":{"disconnected":true}}` whether the
  DELETE saw 200, 401 (already gone site-side, the §2.4 rule) or nothing at all.
- `POST /api/site/import` grows a second body shape: `{"designId": "<id>"}`
  (`link` and `designId` together or apart-but-empty → `siteLinkRefused`):
  `loadSiteLink` required (`s.jsonErr("not connected to orb-os.net")` without);
  id must match the same 22-char grammar `parseSiteLink` enforces; fetches
  `<siteReleaseBase()>/api/hub/theme/<id>` **with the bearer**, through a
  bearer-parameter on the existing fetch path — one added parameter or sibling
  function, not a second copy of the client — then the untouched
  `checkBundle`/`checkBundleArt`/ticket/`ThemeOrigin` pipeline. 401 on that fetch
  clears the link and answers `disconnected from orb-os.net`, as above. The
  `ThemeOrigin` records `siteReleaseBase()` and the id: an own design imports
  exactly like a shared one and tags "from <host>" the same; §2.6 already made
  the bytes uncredited.

## Behaviour and edge cases

- `loadSiteLink()` runs per call everywhere; a step-1 relink (or a site-side
  disconnect landing between two clicks) is felt on the NEXT request, no cache
  to invalidate.
- `designs` twice in one minute against the fake site: two upstream GETs — the
  hub caches nothing, freshness is the point.
- A 502, a timeout, and a 200-that-is-not-JSON all leave the token in place;
  only a 401 clears it. Pin all four.
- Import by id for a design that answers 404 (§2.6's stranger/private case, or
  plain nonsense): the importer's own `siteLinkRefused` sentence — a wrong id
  and a wrong paste are the same user mistake. A 403 never appears: §2.6 answers
  404 for exactly that reason.
- Import by id while linked to user A, ticket consumed by the editor: the keep
  path is H1's unchanged — test only up to the ticket, as step 1 of H1 did.
- Editor-token requests to all three routes: refused at the editor listener the
  way `/api/flash` is (`TestEditorAPIOnlyTheListedRoutes` extended with the new
  paths).
- `-site` off: nothing registers; the step-1 door-shut test grows to name
  `/api/site/designs` and `/api/site/disconnect` alongside connect.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd
   tools/orb-manager && go vet ./... && go test -run
   'TestSiteDesigns|TestSiteDisconnect|TestSiteImportId|TestSiteDoorShut|TestEditorAPI'
   -v .` → PASSes, with new tests `TestSiteDesignsProxiesAndSendsBearer` (fake
   site asserts the exact header and the list arrives byte-equal),
   `TestSiteDesigns401ClearsTheLink`, `TestSiteDesignsKeepsTheTokenOnAnyOtherFailure`,
   `TestSiteDisconnectForgetsTheToken` (site sees the DELETE with bearer — and a
   variant where the site is unreachable — file gone either way),
   `TestSiteImportIdUsesTheBearerAndTheTicket` (bearer asserted at the theme
   door; ticket consumes into `checkBundle`'s hands; origin sidecar names the
   configured site and the id).
2. `go test ./... -count=1` → suite green; `TestSiteDoorShutWithoutTheFlag` and
   `TestEditorAPIOnlyTheListedRoutes` extended, both green.
3. `git diff --exit-code h2 -- web internal state.go` → exit 0 (UI is step 3;
   state and internals unmoved).

## Out of scope

- No HTML/JS: step 3. No update-check changes (`resolveOrigin` stays H3's
  problem), no retry/backoff — one call, one answer.
- Do not modify `release.go`'s allow rules: `siteAllowed` already blesses the
  configured origin and `orb-os.net`, and every dial here goes to
  `siteReleaseBase()`, which is checked by construction.

## Notes

- The bearer is never logged: the debug log gets the STATUS LINE of the
  disconnect call, not the request.
- The fetch path's no-redirect rule already covers these doors; do not add a
  second client.
- If `fetchSiteBundle`'s signature fights you, the honest seam is a
  `bearer string` last parameter on the low-level fetch with the two callers
  passing `""`/token — everything above it stays untouched.

