# H4 hub step 2: the upload doors — `POST /api/site/upload` and `GET /api/site/local-designs`

## Goal

Slice H4, step 2 of 3 on Orb Manager (docs/specs/h4-design.md §6's second and third
paragraphs). The hub can put one of its own designs on the site, and the page can list what it
would upload. One door dials out with the bearer and reports the answer's meaning; the other
reads the local library. No HTML and no button in this step — step 3 wires them.

## Read first

- `docs/specs/h4-design.md` §2 (what the site says back), §6's `POST /api/site/upload` and
  `GET /api/site/local-designs` paragraphs, §7's hub rows, §10's 403-vs-401 answer.
- `tools/orb-manager/sitelink.go` — `fetchSiteDesigns` (line 691: the header set, the
  no-redirect client, the body cap and the one-failure wording you are cloning for a POST),
  `handleSiteDesigns` (line 750: the disconnected/not-connected/401-other-refusal shape),
  `forgetSiteLink`, `liveSiteLink`, `disconnectedSiteLink`, `siteCallTimeout` and
  `siteDesignsBodyMax`, `routesSiteLink` (line 814), and step 1's `siteLinkScopes`.
- `tools/orb-manager/designs.go` — `designsDir`, `designName`, `designFile`, `listDesigns`,
  `DesignRow`, `designMaxBytes` (64 MB: the hub's own save bound, not the site's 10 MB).
- `tools/orb-manager/libentries.go` — `entry` (its `Draft`/`Theme`/`State`/`path`), `LibTheme`'s
  `CanReopen` and `MadeWith`, `Library.entries` (line 100) and `entryFold` (line 190),
  `shelfApp` (line 57) and `shelfThemes` (line 65), and `handleLibDesign` (line 250: the exact
  default-branch read of a draft's or a theme's bytes that the new door replicates, and the
  trimmed-`ErrNoDesign` sentence that is this door's neighbour).
- `tools/orb-manager/main.go` — `s.siteHidden` and where `routesSiteLink` is called; and
  `tools/orb-manager/sitelink_test.go`'s fake site, its `siteCallTimeout` test hook if present,
  and `TestSiteDoorShutWithoutTheFlag` / the editor-API-listener test the new doors must join.

## Contract

- `var siteUploadTimeout = 120 * time.Second`, a `var` for `siteCallTimeout`'s reason (the
  comment says so), and a new `const siteUploadBodyMax = 8 << 10` cap on the answer.
- New file `tools/orb-manager/siteupload.go` holding both handlers, their one shared refusal
  helper, and `designsRoot()`:
  - `func (l *Library) designsRoot() string` — sibling of `designsDir`, the same
    `filepath.Dir(writableThemesDir())` + `libDesignsSubdir`, empty when there is no writable
    library. Used only by the listing door.
  - `func (s *Server) handleSiteUpload` — `POST` only (`http.Error(w, "POST only", 405)` as
    its siblings do). Body `{"app","name","siteName"?,"visibility"?,"replaceId"?}` naming an
    **entry** in this hub's library, read through the hub's
    existing small-body bound. `name` names that entry and is what the library lookup reads;
    `siteName` (step 4's separation of the two names) is the name the site is dialed with when
    the body carries one, and is held to the same `designName` grammar as `name` — an explicit
    empty or blank one is the same refusal, not a silent fall back to the entry's name. Order of
    local refusals, each one `{"ok":false,"error":<one
    sentence>}` at HTTP 200 as `jsonErr`'s shape already is: app failing `designAppRe` →
    `expected an app slug`; a name `designName` refuses → its sentence, and a carried `siteName`
    `designName` refuses → that same sentence before anything is dialed; no writable library dir
    → the same sentence `designApp` uses; no such entry (`s.library().entryFold(app, name)`, the
    same fold the picker's rows and `handleLibDesign` match on) → `no “<name>” in
    the <app> library on this hub` (`handleLibDesign`'s words); an entry with a draft **and** no
    draft file, or a theme-only entry whose bake carries no design (`CanReopen` false, so
    `orbnet.ShelfTheme(...)` gives a nil `Design` or `ErrNoDesign`) → `that theme carries no
    design, so it can't be uploaded`, this door's own wording for its own refusal (the neighbour
    of `handleLibDesign`'s trimmed `ErrNoDesign` sentence, which is about reopening); the bytes
    that came out of that read over
    `siteUploadMaxBytes` (a new `const` of `10 << 20`, the hub's copy of the site's cap —
    `designMaxBytes` is 64 MB and is the hub's own save bound, not this rule) → `that design is
    over the 10 MB a saved design may be`; and a link whose `siteLinkScopes` lacks
    `designs:write` → `Reconnect to allow uploads`, dialing nobody. A disconnected file and a
    missing file answer the two existing sentences, `siteMsgDisconnected` and
    `siteMsgNotConnected`, before any of that.
  - **The read is `handleLibDesign`'s default branch, replicated, not called**: draft bytes
    (`os.ReadFile` of `designFile(name)` under `l.designsDir(app)`) when `e.Draft != nil`, else
    `os.ReadFile` of `filepath.Join(<writable themes dir>, e.Theme.File)` through
    `orbnet.ShelfTheme` and then `t.Design`. Never `&from=theme` and never the bake's own file
    verbatim: an unsent draft uploads as the human sees it (ruling 43), and a bake's bytes are
    not a design document. Replicating the branch is why this step's Go diff touches no existing
    file: `handleLibDesign` writes to a `ResponseWriter` and takes its name from the query, so
    sharing it would mean changing it, and changing the editor's door for an upload door is
    h4-design.md §11's "one door, one job" inverted. Keep the two readings side by side in the
    new file with a comment naming `libentries.go`'s `handleLibDesign` as the shape.
  - The dial: `POST <l.Site>/api/hub/designs?app=<app>&name=<name>` — both always, through
    `url.QueryEscape`, with the body's `app` forwarded verbatim and the query's `name` the body's
    `siteName` when the body carried one and its `name` otherwise, in both cases forwarded
    **verbatim**: this door adds no name of its own and rewrites none, so a Replace's spelling is
    whatever the dialog put in the body (§6: on a Replace the dialog sends the clash answer's
    `existing.name`, in `siteName`, while `name` still names the entry it reads), and a body whose
    name is not the site's spelling comes back as the site's `replace_mismatch` sentence and is
    passed through like any other refusal. Plus
    `?visibility=` when the body named one and
    `?replaceId=` when it named one (both through `url.QueryEscape`, absent when absent), the
    body the design bytes the read returned verbatim, and headers `Authorization: Bearer`, `Accept:
    application/json`, `User-Agent: orb-manager/<version>`, `Content-Type: application/json`,
    `Cache-Control: no-store` — the same five `fetchSiteDesigns` sets, plus the content type a
    POST needs. `siteHTTP()` (no redirects), `siteUploadTimeout`, one attempt, no retry.
  - The answer, always through one helper so no path can leak a raw site body:

| Site answer | Hub answer |
|---|---|
| 201 object | `{"ok":true,"data":<the site's object, verbatim>}` — `url` untouched |
| 403 `insufficient_scope` | `{"ok":false,"error":"Reconnect to allow uploads"}`, credential kept |
| 409 `name_taken` | `{"ok":false,"error":"name_taken","message":<site's message>,"nameTaken":<site's existing object>}` |
| 401 | `forgetSiteLink(l)` then `{"ok":false,"error":"disconnected from orb-os.net"}` |
| Any other status | `{"ok":false,"error":<the site's `message` when its body is a §2 envelope carrying one, else `orb-os.net did not answer`>}`, credential kept |
| Timeout, connection refused, oversize or unparsable answer | `{"ok":false,"error":"orb-os.net did not answer"}`, credential kept |

  The `data` object passes an envelope check (an object with a string `id` and a string `url`)
  and is forwarded as `json.RawMessage`, never into a hub struct — the reason
  `siteDesignsObject` checks an envelope rather than a schema. A 201 failing that check is
  `orb-os.net did not answer`. `name_taken` is matched on the site's `error` **word** (the two
  credential answers above are matched on status, because status is the only thing they say): a
  site that answers the clash at another status still reaches the human's confirm. When the
  clash's body carries no `existing` object — the race h4-design.md §3.2 names, where the site's
  in-lock refusal fires after the door's advisory read — the hub's answer is the same shape with
  `nameTaken` absent, and step 3's dialog then shows the sentence and no Replace button.
  - `func (s *Server) handleSiteLocalDesigns` — `GET` only, no bearer and no dial: the picker of
    what **can be uploaded**, which is not the same list as "files in `designs/`". For each app
    that has a `designs/<app>/` folder **or** a shelf theme (`shelfThemes` over the writable
    themes dir with `shelfApp` naming each one's app — a theme directory with no designs folder
    is a library too), run `s.library().entries(app)` and keep the entries with a `Draft` or a
    `Theme` whose `CanReopen` is true: the same flag `/api/library/entries` answers and the same
    flag `handleLibDesign`'s theme branch depends on, so the picker never offers a bake with
    nothing inside. Emit `{app, name, bytes, at, appVersion}` per entry, where `bytes` and
    `appVersion` come from **whichever half would actually upload** — the draft's `DesignRow`
    (`Bytes`, `AppVersion`, `At`) when there is a draft, else the **carried** design's size and
    version — `e.Theme.MadeWith` (`entries` already read the design to stamp it) and
    `len(t.Design)` from one fresh `orbnet.ShelfTheme` read of the shelf file, since
    `LibTheme.Bytes` is the bake's size and `CanReopen` says only that a design is inside, never
    how big it is — never the bake's own file size, which is what the door's 10 MB pre-check then
    weighs honestly. Sort by `at` newest first, then app, then name;
    truncate at 500; answer `{"designs":[…],"truncated":<bool>}`. An absent or unreadable
    designs root, a shelf that cannot be read, or an app dir that cannot be read contributes
    nothing rather than failing the
    door: a hub with no designs answers `{"designs":[]}`. No existing door does this:
    `/api/library/entries` is per app, and `designs.go` lists one folder.
- Both handlers join `routesSiteLink`, so both are behind `requireControl`, both inside main.go's
  `!s.siteHidden` block, and both absent from the editor-API listener.

## Behaviour and edge cases

- `TestSiteUploadSendsTheDocument`: the fake site asserts method, path, the
  `?app=`/`?name=`/`?visibility=`/`?replaceId=` presence (the first two always, escaped: a name
  of `Big Ben` goes out as `name=Big+Ben` or `name=Big%20Ben`, never raw), the five headers, and
  that the body is the draft file's bytes; the hub's answer is `{"ok":true,"data":…}` with the
  site's `url` byte-identical.
- `TestSiteUploadAsNewUsesTheTypedName` (step 4): an entry `Aurora` uploaded with
  `siteName: "Aurora 2"` reaches the fake site as `?name=Aurora+2` carrying Aurora's own design
  bytes, a case-folded `name` with a typed `siteName` still reads the file the hub wrote, a body
  with no `siteName` still dials the entry's own name, and a `siteName` `designName` refuses —
  empty, blank, past 64 characters, or carried on a body naming no entry at all — is refused with
  that rule's sentence and zero requests. A Replace is the same field with the clash answer's
  stored name, beside its `replaceId`.
- `TestSiteUploadSendsAThemesDesign`: an entry with **no draft** — only a shelf `.orbtheme` whose
  bake carries a design — uploads `t.Design`, not the bake: assert the body the fake site got
  parses as a design document, is shorter than the shelf file, and equals what
  `GET /api/library/design?app=&name=` answers for the same entry with no `&from=` (that door's
  default branch is the thing being replicated, so it is the oracle). A second row in the same
  test, an entry with **both** halves, asserts the draft's bytes are what went out — the unsent
  change uploads, not the older bake.
- `TestSiteUploadThemeWithNoDesign`: an entry whose theme is a bake with no design inside
  (`CanReopen` false) is refused locally, `that theme carries no design, so it can't be uploaded`,
  zero requests to the fake site, and the same entry is **absent** from
  `GET /api/site/local-designs` — the picker and the door refuse it for the same reason.
- `TestSiteUploadAnswerMatrix` drives every row of the table above from the fake site and, after
  each case except the 401, asserts `site-link.json` still holds the token; after the 401 it
  asserts the file reads disconnected and the answer is the one sentence. Include the
  `replace_mismatch` and `invalid_request` rows here as pass-through cases: the hub repeats the
  site's sentence and keeps the credential.
- `TestSiteUploadScopeRefusalKeepsTheLink`: a read-only link is refused locally — the fake site
  records zero requests — answering `Reconnect to allow uploads`.
- `TestSiteUploadTooLargeLocal`: an 11 MB local draft file is refused without a request, with the
  site's own sentence; a theme whose carried design is 11 MB inside a 12 MB bake is refused too,
  which is the row that proves the door weighs the design and not the bake.
- `TestSiteUploadOnceOnly`: the fake site answers 500 first and would answer 201 second; the hub
  reports the site's message and the fake records exactly one request — no retry, because a
  retry could turn one confirmed Replace into two versions.
- `TestSiteUploadBodyShape`: `{"app":"clock"}` with no name, an app of `../etc`, a 65-character
  name, a name `designName` refuses, a valid body naming no such entry, and a body that is not
  JSON — each gets its sentence above, with no request and the file unread.
- `TestSiteLocalDesignsListsAcrossApps`: three apps and ten drafts, one file with no
  `saved.name` (unlisted, as `listDesigns` already treats it) and one directory named `not an
  app` (skipped by the slug rule); newest first; 501 rows answer 500 with `truncated:true`.
- `TestSiteLocalDesignsListsThemesToo`: an app with **no** `designs/` folder and two shelf themes,
  one baked from a design and one baked without one, answers exactly one row for the app, whose
  `bytes` is the carried design's length (assert it against the design document's size, not the
  `.orbtheme` file's) and whose `appVersion` is the carried design's; an app that has both a
  draft and a bake answers **one** row for that name, at the draft's size — the picker offers an
  entry, not a file.
- `TestSiteDoorShutWithoutTheFlag` and the editor-listener test grow to cover both new paths:
  with `-site` off they are 404 on the control listener, and on the editor listener they refuse
  editor credentials.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd tools/orb-manager && go vet ./... && go test -run 'TestSiteUpload|TestSiteLocalDesigns|TestSiteDoorShut|TestSiteLink|TestEditorAPI' -count=1 -v . 2>&1 | tee "$DELEGATE_EVIDENCE_DIR/h4-hub-2.log"` → every listed test PASS, no `--- FAIL`.
2. `grep -q 'siteUploadTimeout' sitelink.go && grep -q '/api/site/upload' sitelink.go && grep -q '/api/site/local-designs' sitelink.go` → exit 0 — `routesSiteLink` is the only place a site door is mounted.
3. `! grep -n 'http.DefaultClient\|http.Get(' siteupload.go` → exit 0: the no-redirect client is the only way out.
4. `git diff --exit-code h4 -- web internal state.go libentries.go designs.go` → exit 0 (`siteupload.go` and the two registration/timeout lines are this step's whole Go diff).
5. `cd tools/orb-manager && go test ./... -count=1` → green.

## Out of scope

- No HTML, CSS or JS here, and no `#mn-lib-upload` state change: step 3. No download, delete,
  rebake, batch upload, theme-file upload or import-from-site (H2's Import already exists).
- No chunking, progress reporting or resume, and no change to the 10 MB bound: the dialog waits.
- Do not modify `designs.go` to add `designsRoot()` — the listing door derives the root from
  `writableThemesDir()` itself, in `siteupload.go`, where the only caller of it lives.

## Notes

- `handleSiteUpload` answers HTTP 200 with `ok:false` for every refusal, site or local, the way
  H2's site doors do: the dialog then has one failure path and one `data` path. The site's status
  goes to the log, not to the browser.
- Log one line per upload — `log.Printf("orb-manager: site upload: %s/%s: %s", app, name,
  <outcome>)`, the shape `handleSiteDisconnect` uses for its revoke — reporting the sentence the
  owner saw and never the request, which is all bearer.
- Read the draft with `os.ReadFile(filepath.Join(dir, designFile(name)))` exactly as
  `handleLibDesign` does, and do not dial `/api/library/design` from the door: it is the same
  process, the same library and the same bytes, and an HTTP hop between two handlers that share a
  struct buys a serialisation, a status-code path and a second place for the no-design sentence
  to live. Replicating its default branch (draft first, then the theme's design) is two `if`s and
  keeps this door's power to say which bytes it weighed.
- `entryFold`, not `entry`: the picker's rows come from `entries()` and carry the name the file
  holds, so a dialog that posts what it was shown must match case-insensitively the way
  `/api/library/design`'s own lookup does — otherwise the row the human clicked answers
  `no “…” in the … library`.
- Forward `?visibility=` only when the body carried one, so the site's default stays the site's
  rule and the wire shows a choice the human made.
