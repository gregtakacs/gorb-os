# H4 hub step 3: the upload dialog — `#mn-lib-upload`, the picker, and the link back

## Goal

Slice H4, step 3 of 3 on Orb Manager (docs/specs/h4-design.md §6's last paragraph and §7's hub
rows). The button the H2 UI left disabled becomes the upload dialog: it picks one of this hub's
designs, asks for a visibility, dials step 2's door, and reports the answer with a link back to
the site. A read-only link is the one sentence the ruling gave; a name clash is a confirm with
three answers.

## Read first

- `docs/specs/h4-design.md` §6's dialog paragraph, §4's Replace rules, §7's hub rows verbatim.
- `tools/orb-manager/web/index.html` — the Theme library toolbar (line 49 onward):
  `#mn-site-btn`, `#mn-lib-upload` (disabled, hidden, with the "later release" title to
  replace), `#mn-lib-state`, and `#mn-site-row` with its two blocks; the `#cf-mask` dialog
  (line 761) and the `class="mask" id="ob-flashmask"` panel (the second-mask precedent, whose
  rule jscheck_test.go:968 pins: the mask wraps, the panel is `role="dialog" aria-modal="true"`,
  and the panel's CSS must not re-position itself).
- `tools/orb-manager/web/app.js` — line 5143's `for (const id of ["#mn-site-btn",
  "#mn-lib-upload"]) { … b.hidden = !site }` (the -site show/hide already exists), `loadSiteLink`
  / `drawSiteLink` (5691, 5710: where scopes arrive and where the button's state must be set),
  `loadSiteDesigns`, `siteDesignRow` (5821: the safe-insert idiom, `textContent` and `href`
  properties only), `askConfirm` (6252: two buttons, which is why the clash confirm lives in the
  dialog rather than in it), and `hubFetch`.
- `tools/orb-manager/ui_test.go` — `TestSiteUISectionIsThereAndShut` (line 1896) and
  `TestSiteImportDoorInTheLibrary` (1735) for the assertion idioms: `readWebFile`, `readAppJS`,
  `regionOf`, `blockOf`, `blockOfLine`.
- `tools/orb-manager/jscheck_test.go` — the ASCII-in-console rule and the mask rule, and the
  `jsFuncBody` helper the new pins should use.

## Contract

- `index.html`: `#mn-lib-upload` loses `disabled` and its placeholder title, keeping `hidden`
  (app.js shows it for `-site` hubs and nothing else changes its visibility); its title becomes
  `Send a design from this hub's library to your orb-os.net account. It lands in your library
  there under the same name; nothing on the site is changed.` A `#mn-lib-reconnect` ghost button,
  `hidden` in markup and titled `Link this Orb Manager to your orb-os.net account again, and
  approve the two scopes`, joins the toolbar row beside it. A new `class="mask"
  id="up-mask"` wrapper holding `id="up-panel" role="dialog" aria-modal="true"` sits with the
  other masks, containing, by id: `#up-title`, `#up-app`, `#up-name` (both `<select>`),
  `#up-vis` (three radio inputs named `up-vis`, values `private`/`unlisted`/`public`, `private`
  checked), `#up-clash` (a `hidden` row with `#up-clash-line`, `#up-replace`, `#up-new`,
  `#up-cancel`), `#up-replace-ask` (a second `hidden` row inside the clash row, holding
  `#up-replace-line`, `#up-replace-yes` and `#up-replace-no`),
  `#up-newname` (a text `input` `hidden` by default, `maxlength="64"` — the hub's
  own `designName` bound), `#up-state`, `#up-close`, and `#up-go`.
- `app.css`: one `.uppanel{}` rule that lays out the panel and, following the mask contract,
  does **not** set `position:fixed`.
- `app.js`, all new code in the site section, none of it dialing before the human presses:
  - `drawSiteLink` (grown, not replaced): a `linked` answer also sets `siteWritable = Array.
    isArray(data.scopes) && data.scopes.indexOf("designs:write") >= 0`, enables `#mn-lib-upload`
    only when `siteWritable`, and writes the ruling's sentence `Reconnect to allow uploads` into
    the toolbar's own `#mn-lib-state` **only** when a read-only link is the reason the button is
    off — the four existing state sentences in `#mn-site-state`/`#mn-site-conn` keep their exact
    bytes and priority, and `#mn-lib-state` goes back to empty for a writable link.
  - `siteUploadOpen()` / `siteUploadClose()` drive `#up-mask` (the mask, never the panel inside
    it, exactly as `fpOpen`/`fpClose` are pinned to do), fill the app and name selects from one
    `GET /api/site/local-designs` read, and hide `#up-clash` and `#up-newname` on every open. The
    name select filters to the app select's choice; an empty library leaves the selects empty and
    says `This hub has no designs to upload.` in `#up-state`.
  - `siteUploadSend(replaceId, replaceName)`: `POST /api/site/upload` with `{app, name}` (the
    visible name — `#up-newname`'s value when it is showing, the select's otherwise — or, when
    `replaceName` is given, that string instead: the clash answer's `existing.name`, which is the
    only spelling §4 accepts on a Replace), `visibility` only when the
    radio is not `private`, and `replaceId` only when given. `#up-go` is disabled and `#up-state`
    reads `Uploading <name>…` while it runs, and the button comes back on every answer including
    a stall.
  - Answers: `ok` → `#up-state` reads `Uploaded to orb-os.net as ` plus the visibility **the site
    answered** (`private`, `unlisted` or `public`, from `data.visibility`, never from the radio),
    followed by an anchor
    `Open <name on the site's answer, not the local spelling> on orb-os.net` whose `href` is the
    answer's `url` assigned as a property (`rel="noopener noreferrer"`, `target="_blank"`, as
    `appendSiteFallback` does). A `data.shareRefused` member changes only the sentence around that
    anchor: `Uploaded to orb-os.net as private: ` + its `message` verbatim — the design is on the
    site and only its share failed, so the line says both facts and the link still appears.
    `error === "name_taken"` → `#up-clash` unhides with `#up-clash-line`
    reading `orb-os.net already has a design called “<existing.name>”.`; **Replace** unhides
    `#up-replace-ask` and sends nothing — that row's line reads `Replace adds a new version of
    “<existing.name>” on orb-os.net. It stays <existing.visibility> and keeps its link, and the
    current version can be restored on the site.` with **Replace** on `#up-replace-yes` (which is
    the press that re-sends with the `nameTaken.id` and `nameTaken.existing.name` as the two
    replace arguments) and **Cancel** on `#up-replace-no` (which
    hides the ask row and leaves the clash row standing); **Upload as a new design** un-hides an
    editable name field prefilled with
    what the human first chose, **Cancel** hides the row and leaves the dialog open. Any other
    `error` → that sentence in `#up-state`, verbatim, and the dialog stays open.
  - The no-handle rule is local: choosing `unlisted` or `public` while the last `link-status`
    answer had no `handle` writes `Unlisted and public designs need a handle: set one on ` into
    `#up-state` followed by an anchor whose `href` is that same answer's `site` + `/me` (the page
    has no `siteReleaseBase`; `link-status` is where the origin comes from) — and does not dial.
    The site's own `no_handle` refusal is still shown when it arrives, since another browser can
    set a handle between the reads: the local check is a shortcut, not the rule.
  - A read-only link's sentence and its `Reconnect` button (`#mn-lib-reconnect`, in the markup
    beside `#mn-lib-upload`, `hidden` until it is needed) live in `#mn-lib-state`'s draw; the
    button's press calls the function `#mn-site-connect`'s press calls, held in a named function
    rather than duplicated.
  - Escape, `#up-close`, and a scrim click on `#up-mask` all close; a second open while one is
    open is a no-op rather than a second dialog, as `askOpen` does.
- Curly quotes in the JS strings are written `\u201c`/`\u201d` as the file's 21 existing cases
  do; `index.html` may hold the raw character the way its one existing `“` does.

## Behaviour and edge cases

The pins go in `ui_test.go` beside `TestSiteUISectionIsThereAndShut`, in that file's idioms
(`readWebFile`, `readAppJS`, `regionOf`, `blockOf`, `blockOfLine`, `jsFuncBody` from
`jscheck_test.go`), because this page has no DOM in the test binary:

- `TestSiteUploadButtonIsEnabledByScopesOnly`: `#mn-lib-upload` carries no `disabled` attribute
  in markup, `loadLibThemes`'s `-site` hiding still lists it, and the body of the `drawSiteLink`
  function contains both the `designs:write` test and `#mn-lib-upload`, and the sentence
  `Reconnect to allow uploads`.
- `TestSiteUploadDialogIsModalHonest`: `#up-mask` appears before `#up-panel` in the markup,
  `#up-panel` carries `role="dialog" aria-modal="true"`, the `.uppanel{}` rule in `app.css`
  contains no `position:fixed`, and the open/close functions name `#up-mask` rather than
  `#up-panel` — the four assertions jscheck_test.go makes of the flash panel, for this panel.
- `TestSiteUploadSendsForeignBytesAsTextOnly`: the functions that draw an upload answer contain
  no `innerHTML`, and every site-sourced string reaches `textContent` or an `href` property; the
  success anchor's label is built from the answer's `name`, so a site answering
  `"<img src=x onerror=alert(1)>"` as a name can only ever be a label.
- `TestSiteUploadClashOffersThreeAnswers`: `#up-replace`, `#up-new`, `#up-cancel` exist, the
  clash row is `hidden` in markup, and the send function passes `nameTaken.id` as the replace id
  and `nameTaken.existing.name` as the name (never the local spelling: §4 refuses a Replace whose
  query name is not the stored one, so the hub that sent its own label would be refusing the human's
  own confirmation),
  and re-reads `#up-newname` for the new-name path — a `name_taken` can never be closed by
  anything but one of the three.
- `TestSiteUploadReplaceConfirmsBeforeItSends`: `#up-replace-ask`, `#up-replace-line`,
  `#up-replace-yes` and `#up-replace-no` exist and the ask row is `hidden` in markup; the
  `#up-replace` handler's body (`jsFuncBody`) contains no `fetch` and no `siteUploadSend(` call,
  while `#up-replace-yes`'s handler does and calls it with the clash answer's id and its
  `existing.name`; the ask sentence's three interpolations are
  `existing.name`, `existing.visibility` and nothing invented — the pin is that the human reads
  what a Replace does (a new version, the visibility kept, the old version restorable) before the
  bytes move, since a Replace is the one answer that changes something already on the site.
- `TestSiteUploadSuccessNamesTheVisibility`: the ok path's sentence is `Uploaded to orb-os.net as`
  followed by `data.visibility`, and the body contains no read of the radio's value in that
  branch (a Replace's answer says `public` even when the radio said `private`, and the line must
  not contradict the site); `shareRefused` is drawn as `as private: ` plus its `message` through
  `textContent`, beside the same anchor.
- `TestSiteUploadNoHandleIsLocal`: the visibility-change handler's body names `handle` and
  `/me`, and no `fetch` of `/api/site/upload` appears before that check in the send path.
- `TestSiteUploadReadsThePicker`: the open function reads `/api/site/local-designs` once and
  fills both selects; the empty answer's sentence is `This hub has no designs to upload.`
- The three states a human can be left in are each reachable and each recoverable: in flight
  (`#up-go` disabled, `Uploading <name>…`), refused (sentence shown, dialog open, button live),
  succeeded (link shown, and reopening the dialog clears the row and the clash). A hub whose
  `-site` door closes between the draw and the press gets the door's own `not connected to
  orb-os.net` sentence, not a JS error: the send path has one `catch` and it draws.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd tools/orb-manager && go vet ./... && go test -run 'TestSiteUpload|TestSiteUI|TestSiteDoorShut|TestSiteLink|TestEditorAPI|TestConsoleOutputIsASCII' -count=1 -v . 2>&1 | tee "$DELEGATE_EVIDENCE_DIR/h4-hub-3.log"` → every listed test PASS, no `--- FAIL`.
2. `cd tools/orb-manager && go test ./... -count=1` → the whole hub suite green.
3. `grep -q 'id="mn-lib-upload" class="ghost" type="button" hidden' web/index.html && grep -q 'id="up-panel" role="dialog" aria-modal="true"' web/index.html` → exit 0 (enabled, still hidden-until-`-site`; the panel is modal).
4. `grep -A2 'id="mn-lib-upload"' web/index.html | grep -c disabled` → `0`, and `! grep -rn 'innerHTML' web/app.js | grep -i 'siteupload'` → exit 0.
5. From the repo root: `make check-apps check-hub check-editor check-web check-runner check-migrations check-editor-origin` → the same clean answer `h2-hub-3`'s verify line produced, since that target set is the hub's own lint.

## Out of scope

- No upload from an app's editor page, no drag-and-drop, no folder or multi-select upload, no
  progress bar for a 10 MB design, and no queue: one design, one press, one answer.
- No edit of an uploaded design, no delete, no un-share, no version list, and no link to
  `/me`'s design rows beyond the one no-handle anchor. `askConfirm` is not grown to three
  buttons: the hub's confirmation is a yes/no primitive and the clash is a choice of three
  actions, which is why it lives in the dialog.
- No change to `#mn-site-row`'s layout, to `siteDesignRow`, to the import door or to the connect
  flow beyond naming the handler the reconnect button calls.

## Notes

- The dialog reads the picker once per open rather than caching it: a design saved from an
  editor minute ago should be uploadable without restarting the hub, and 500 rows of `{app,
  name, bytes, at}` is a small read.
- `siteWritable` is module state written only by `drawSiteLink`, so every draw (including the
  poll's) recomputes it; a link that loses its write scope on relink cannot leave a live button
  behind, because the relink's draw is what re-reads the scopes.
- The answer's `name`, not the local spelling, goes into the success label and the anchor: a
  case-only clash resolved by Replace kept the site's spelling (§4), and the page should show the
  name the site now answers to.
- Keep each new function small and top-level: `jscheck_test.go`'s undeclared-write pass reads
  this file's function spans, and a closure writing an outer name is a failure there.
