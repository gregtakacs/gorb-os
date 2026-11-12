# H3 hub step 2: the inbox client — a hold on disk that survives a restart

## Goal

Slice H3, hub step 2 (docs/specs/h3-design.md §7.1, §3). Once a minute the hub asks its site
link's inbox door, and for each item it does **not** already hold it writes a hold file under
the config dir containing the **pinned version's bytes** (fetched from the bundle door, not
the theme door), fsyncs, and only then claims the item. A restart between fetch and import
loses nothing: the claim is on disk, the bytes are on disk, and the next boot re-drives the
same files. The Themes tab grows a **From orb-os.net** block with Add (the existing
import→editor→keep flow, unchanged downstream) and Dismiss. No new shelf code, no new bake code,
no new keep code.

## Read first

- `docs/specs/h3-design.md` §3 (all three doors — the item shape, the claim matrix, the pinned
  bundle door), §7.1, §8's tray rows.
- `docs/specs/h3-site-1-inbox-doors.md` — the contract the fake site in these tests answers
  like (the chains run in parallel; the fake site is built from the design doc and that file,
  never from a live site).
- `tools/orb-manager/siteimport.go` — `fetchSiteBundle(r, site, id, bearer)` and its
  `siteFetchError` (status field), `checkBundle`, the `siteTicket`/`siteTicketStore` add/get
  shapes, `handleSiteImport`'s success JSON keys (`ticket,site,id,version,app,name,sharedName,
  handle,schema`), `handleSiteKeep`'s sidecar write (`writeThemeOrigin`, line ~570),
  `siteLinkMax`, `siteLinkRefused`, `siteMsgNotConnected`, `forgetSiteLink`.
- `tools/orb-manager/sitelink.go` — `liveSiteLink()` (:196), `siteAllowed`, `own.Token`, the
  401→`forgetSiteLink` arm at siteimport.go's own-design branch, `jsonOK`/`jsonErr`,
  `requireControl`, the `mux.Handle("/api/site/…")` registration block (:888).
- `tools/orb-manager/release.go:70` — `siteReleaseBase()` (the one configured site).
- `tools/orb-manager/updates.go` — `themeUpdateRows` (:218) and `ThemeEntry` cards: how a row
  and its `Choices` are drawn, and how `web/app.js` renders a row-with-buttons.
- `tools/orb-manager/liveness.go:529`, `release.go:254`, `main.go:868` — the ticker goroutine
  this loop copies and where it is started.
- `tools/orb-manager/config.go` (or wherever `configDir` lives — read it; the hold directory
  goes under it), `siteimport_test.go` — the fake-site rig style every test here uses.

## Contract

- One new file, `tools/orb-manager/siteinbox.go`, plus one `go s.siteInboxLoop(cableCtx)` line
  in `main.go` beside the liveness goroutine, plus two mux registrations
  (`/api/site/inbox`, `/api/site/inbox/add`, `requireControl`-wrapped) and one `web/app.js`
  block. Constants: `siteInboxEvery = 60 * time.Second`, `siteInboxMax = 32`,
  `siteInboxBodyMax` = the bundle door's max (siteimport.go's existing bundle cap — reuse it,
  do not re-declare a number), timeout = the existing site fetch timeout.
- **The hold.** Directory `<configDir>/site-inbox`, created `0700` on first use; one file per
  item, `<itemId>.json`, mode `0600`, written temp+rename+fsync (same atomic style the state
  files use). Envelope, exactly these keys:

  ```json
  {"itemId":"…","designId":"…","name":"…","app":"…","version":4,
   "sentAt":"…","claimed":false,"imported":false,"raw":"<base64 bundle>"}
  ```

  `raw` is the bundle door's bytes; `name`/`app`/`version` are the **pinned** identity from
  the list door. Filenames are the item ids (id grammar: `[A-Za-z0-9_-]{22}` — a file whose
  name does not match is skipped and logged, never opened: the directory is the hub's own, but
  a corrupt one is not a puzzle to be solved at 3am).
- `func (s *Server) siteInboxLoop(ctx context.Context)`: `pollSiteInbox` once at boot (after a
  short delay, as `probeLiveness` does), then every `siteInboxEvery`; a pass that outlives the
  interval drops ticks rather than queueing. `pollSiteInbox(ctx)` returns nil when
  `liveSiteLink()` is nil or has no token, and **re-drives the disk before the network**:
  1. **Settle.** For each held file with `imported:true`: if the library now holds a theme
     whose origin record (`readThemeOrigin`/the sidecar the keep path writes) names this
     envelope's `designId` at a version ≥ the envelope's — delete the file. Otherwise leave it
     (the tray still offers Add; a bake that never came back is a retriable state).
  2. **Re-claim.** For each file with `claimed:false`: `POST <siteBase>/api/hub/inbox/deliver`
     `{"ids":[id]}` with the link bearer. `delivered` → rewrite the envelope with
     `claimed:true`. `missing`, 4xx, 5xx, transport error → **delete the file** (the site says
     this is not ours to deliver; the next poll re-lists the truth). A file whose `claimed` is
     true is skipped here and offered by the tray.
  3. **Fetch.** `GET <siteBase>/api/hub/inbox` with the bearer. 200 → for each item, oldest
     first, skipping ids already held, while held-count < `siteInboxMax`: fetch
     `GET <siteBase>/api/hub/inbox/<itemId>/bundle`; non-200 or oversize → no file, ask again
     next minute; 200 → write the envelope (`claimed:false`) and count it. The count is
     re-read from the directory each iteration, so a full hold stops the pass mid-list without
     a special case. Non-200 list / transport error → **one log line** (`orb-manager: site
     inbox: …`) and the pass ends; 401 → `forgetSiteLink` as the import doors do.
  4. **Claim the new files** in one `deliver` call per pass (the ids written this pass), with
     step 2's per-id outcomes.

- **Tray doors.** `GET /api/site/inbox` → `{"items":[{"itemId","designId","name","app","version",
  "sentAt","claimed","imported"}…]}` newest-held-first, read from the directory only (never the
  network; an unreadable file is skipped, a missing directory is `{"items":[]}`). `POST
  /api/site/inbox/add` `{"itemId":…}`: read that file, `checkBundle` the decoded `raw`, put it
  in the `siteTicketStore`, set `imported:true` in the envelope, and answer **`handleSiteImport`'s
  own success object** (same keys, same order of construction, `site` = `siteReleaseBase()`) so
  the front-end calls the same editor path it already calls. A corrupt/absent `raw`, an oversize
  body, a bad `checkBundle` → the existing refuse sentence; an unknown `itemId` → the same
  sentence, not a 404 (the tray cannot tell the difference and neither can the user).
  `POST /api/site/inbox/dismiss` `{"itemId":…}`: delete the file, then `deliver` that id (its
  answer is ignored — a `missing` is precisely the agreement), answer `{"ok":true}`.
- `web/app.js`: a **From orb-os.net** block in the Themes tab beside the `/api/updates` section
  (`hubFetch("/api/site/inbox")` next to the existing `hubFetch("/api/updates")` call), one row
  per item — §8's `<name> · from <site> · waiting` and `<name> · from <site> · delivered
  <relative time>` — with
  `Add to library` (posts `/api/site/inbox/add`, then the same editor hand-off the paste import
  performs with its answer) and `Dismiss` (posts `/api/site/inbox/dismiss`, re-reads the block,
  and leaves §8's `Dismissed.` in its place until the row repaints). Empty: `Nothing waiting.` No
  new polling: the loop's minute is the poll, the block refreshes with the tab's existing refresh
  idiom.

## Behaviour and edge cases (the tests)

`tools/orb-manager/siteinbox_test.go`, fake site built with `httptest` as `siteimport_test.go`
does (it serves `/api/hub/inbox`, `/api/hub/inbox/<id>/bundle`, `/api/hub/inbox/deliver` with
counters the test can read):

1. `TestSiteInboxHoldThenClaim` — one waiting item: the file appears with `claimed:true` and
   base64 `raw` equal to the served bytes, and the deliver counter saw exactly one call, after
   the file existed (the fake asserts the ordering by checking the directory mid-claim).
2. `TestSiteInboxServesPinnedVersion` — the fake's bundle door answers version 4 while the
   design sits at 7: the envelope says `version:4` and its `raw` is v4's bytes (review 8).
3. `TestSiteInboxRestartReclaimsAndKeepsHolds` — write envelopes by hand (one unclaimed, one
   claimed-unimported, one with an origin record already in the library): one pass → the
   unclaimed one was claimed, the claimed one untouched, the settled one deleted. This is the
   restart test: the hub process is never restarted, the *disk state after a crash* is the
   input (review 11's real requirement).
4. `TestSiteInboxClaimMissingDeletesFile` — deliver answers `missing` → no file, log line.
5. `TestSiteInboxFullHoldStopsFetching` — 32 files exist, the door lists 34 items: two fetches
   happened (or zero, given what was already held) and the rest of the list is untouched — no
   eviction, no error (review 11's "stop fetching").
6. `TestSiteInboxListDoorServesDiskOnly` — `/api/site/inbox` answers with no network (the fake's
   counters stay at zero), newest-held-first, and skips an unparseable file.
7. `TestSiteInboxAddAnswersLikeImport` — the add door's keys equal `handleSiteImport`'s for the
   same bytes, the editor's keep path then writes the sidecar, and the next pass settles the
   file away.
8. `TestSiteInboxDismiss` — file gone, deliver saw the id, `{"ok":true}`.
9. `TestSiteInboxLinkDeadAndIdle` — 401 from the list door forgets the link; no link at all
   makes the pass a no-op that writes nothing.

## Verify

```bash
set -e; root="$(git rev-parse --show-toplevel)"; cd "$root"
! git -C "$root" diff --name-only delegate/h3-hub-1-editor-commit-scope | grep -qvE '^tools/orb-manager/'
test -f tools/orb-manager/siteinbox.go
grep -q 'site-inbox' tools/orb-manager/siteinbox.go
grep -q 'siteInboxLoop' tools/orb-manager/main.go
cd tools/orb-manager && go vet ./... && go test -run 'TestSiteInbox' -v ./... 2>&1 | tail -40
cd tools/orb-manager && go test ./... 2>&1 | tail -20
```

## Done

All nine `TestSiteInbox*` names appear as `--- PASS` in the verbose run; `go test ./...` and
`go vet ./...` are clean; the scope gate (against **hub step 1's branch**, run from the repo
root so the pathspec means what it says) is clean; the envelope's mode is asserted by
`os.Stat` in test 1 (0600) and the directory's (0700).

