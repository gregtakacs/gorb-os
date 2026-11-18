# L1 hub step 2: Orb Manager's library doors and the GorbHost line when linked

## Goal

Slice L1 (orb-os-net ruling 42; `docs/specs/l1-design.md` §4.5 is this step's contract). Orb
Manager's local editors keep their file pickers and gain "Your library" while the hub is linked
to orb-studio.net (H2). Step 1 gave the editors `OrbTheme.library`, which asks `hostUrl('/assets')`
and `hostUrl('/asset?sha256=…')` — under the hub that is `/api/assets` and `/api/asset`. This
step adds those two doors to the editor port, passing through to the linked site's bearer routes
`/api/hub/assets` and `/api/hub/asset`, and tells an editor document it may offer the library by
writing `window.GorbHost={"kind":"hub","library":true}` into it when a live link exists.

## Read first

- `docs/specs/l1-design.md` §2.3 (the asset shape the site answers), §4.1, §4.2, §4.5.
- `tools/orb-manager/sitelink.go`: `siteMsg*` (~90–100), `liveSiteLink` (~196),
  `disconnectedSiteLink`, `forgetSiteLink` (~741), `fetchSiteDesigns` (~763–800) and
  `handleSiteDesigns` (~816–850) — the pass-through to imitate line for line (timeouts,
  `siteHTTP()`, the headers, the 401 handling, the body cap, `jsonOKRaw` / `jsonErr`).
- `tools/orb-manager/editororigin.go`: `editorShim` / `insertEditorShim` (~190–245) and where
  the editor document is served with it (~215–225); `routesEditorListener` (~548–585).
- `tools/orb-manager/sitelink_test.go` (the fake site and how a linked hub is set up in a test)
  and `tools/orb-manager/editororigin_test.go` (how a request reaches the editor port with a
  token, and the route-table assertions). Any test that lists the editor port's routes or probes
  doors an editor may not reach must be updated to name the two new doors as allowed.
- `tools/orb-manager/web/runtime/_selftest/editor-origin-live.js`: its hostile probe list. The two
  new doors are reachable from any editor (owner question 1 in the design); if a probe targets
  them, move it to the "allowed" side with a comment, never delete the check.

## Contract

New file `tools/orb-manager/siteassets.go` (and `siteassets_test.go`):

- `GET /api/assets` on the editor mux: `editorMethod(http.MethodGet, s.handleSiteAssets)` —
  no app scope (the library is the account's, not an app's).
  - `disconnectedSiteLink() != nil` → `jsonErr(siteMsgDisconnected)` without dialing.
  - no live link → `jsonErr(siteMsgNotConnected)` without dialing.
  - otherwise `GET <site>/api/hub/assets` with `Authorization: Bearer <token>`, `Accept:
    application/json`, `User-Agent: orb-manager/<version>`, `Cache-Control: no-store`, the
    `siteCallTimeout`, the no-redirect `siteHTTP()`.
  - 401 → `forgetSiteLink(l)` and `jsonErr(siteMsgDisconnected)`; any other non-2xx, a body over
    `siteAssetsBodyMax = 4 << 20`, or a body that is not a JSON object whose `assets` is an array
    → `jsonErr(siteMsgNoAnswer)`; 2xx → `jsonOKRaw(w, body)` (verbatim, no re-encoding of the rows).
- `GET /api/asset?sha256=<hex>` on the editor mux: `editorMethod(http.MethodGet, s.handleSiteAsset)`.
  - the parameter must be exactly 64 lower-case hex digits, the only query parameter → else
    HTTP 400 `jsonErr` `"no such asset"` without dialing.
  - the link checks as above.
  - `GET <site>/api/hub/asset?sha256=<hex>` with the bearer (headers as above, `Accept: */*`).
  - 401 → forget + `siteMsgDisconnected`; 404 → HTTP 404 with `{"ok":false,"error":"no such asset"}`;
    other non-2xx → `siteMsgNoAnswer`; a body over `siteAssetBytesMax = 10<<20` bytes (read at
    most that + 1) → `siteMsgNoAnswer`.
  - 2xx: the hub checks that SHA-256 of the body is the hash asked for (else `siteMsgNoAnswer`),
    then answers 200 with the bytes, `Content-Type: application/octet-stream`,
    `X-Content-Type-Options: nosniff`, `Cache-Control: no-store`. Nothing is stored on disk.
- **The GorbHost line.** Where the editor document gets its shim, when `s.liveSiteLink() != nil`
  at that moment, the hub inserts right after the shim:
  `<script>window.GorbHost={"kind":"hub","library":true}</script>` (a constant; no request value
  goes into it). With no live link the document is byte-for-byte today's. Only the editor's own
  entry documents get it (the same documents that get the shim).

## Behaviour and edge cases

- Linked, then the site revokes the link: the next `/api/assets` hears 401, forgets the link,
  answers `disconnected from orb-studio.net`; the next editor document has no GorbHost line.
- A site that redirects (3xx) is not an answer → `orb-studio.net did not answer`.
- The site's asset rows are passed through untouched (the hub does not parse them into structs),
  exactly as `/api/site/designs` passes the designs list.
- The doors exist only on the editor port; the control port gets nothing new.

## Tests (`siteassets_test.go`, plus updates to existing route-table tests)

Go test names (each `t.Run` or `func Test…`; keep these words in the names so a grep finds them):
- `TestSiteAssetsNotLinked` — no link → `not connected to orb-studio.net`, the fake site saw no request.
- `TestSiteAssetsPassThrough` — linked → the fake site saw `GET /api/hub/assets` with the bearer;
  the answer is `{"data":<the site's body>,"ok":true}`, byte for byte.
- `TestSiteAssetsRevoked` — the site answers 401 → `disconnected from orb-studio.net` and the link is
  gone from disk.
- `TestSiteAssetsBadAnswer` — 500, a 302, non-JSON, `{"assets":{}}`, a 5 MB body → `orb-studio.net did not answer`.
- `TestSiteAssetBytes` — the bytes come through with the three headers; a body whose hash is
  wrong → `orb-studio.net did not answer`; 404 → 404 `no such asset`; a bad hash (63 hex, upper case,
  an extra parameter) → 400 with no dial.
- `TestEditorDocumentGorbHostWhenLinked` — the served editor document contains the constant line
  right after the shim when linked, and is unchanged (equal to the unlinked bytes) when not.
- `TestSiteAssetsEditorPortOnly` — the control port answers 404 for both paths; the editor port
  needs the editor token (no token → refused as any editor route is).

## Acceptance criteria

1. `cd tools/orb-manager && go vet ./... && go test ./... 2>&1 | tail -30` → `ok` for the package.
2. `cd tools/orb-manager && go test -run 'SiteAsset|GorbHostWhenLinked' -v ./... 2>&1 | grep -E '^(=== RUN|--- (PASS|FAIL))' ` → every test above PASS.
3. `make check-sandbox` → `check-sandbox: green apart from the named skips`.
4. `make check-editor-origin` → every check passes (the live S2 gate, with the new doors).

## Out of scope

- No change to `apps/`, `tools/orb-manager/web/runtime/` (step 1 owns theme@1/studio@1), the
  site link's state machine, the scopes, or the upload door.
- No caching of assets in the hub, no new settings.
