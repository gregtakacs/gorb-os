# Orb Manager: one Updates list (with Skip this version and Keep my version)

## Goal

Step **g3-2** of slice G3 (`~/docker/Projects/orb-os-net/docs/design.md` §7.6, ruling 26, §7.4
discovery, §12 row G3). Orb Manager gets **one Updates list** on the home view, with a count in
its header, gathering every kind of update the §7.6 table names — the Orb-Manager-itself row from
g3-1's release cache, the firmware and app rows from the existing shelf/per-orb reads, and the
**themes from the site** rows §7.4 defines, discovered by resolving each shelf theme's `origin`.
Each row carries its choices; **Skip this version** and **Keep my version** are wired here and
stored in `state.json`. **Keep my version** removes a theme's origin so it stops being tracked for
good (the gate's fourth sentence). The *actionable* update of a site theme — the amber **Update**,
including the renaming one — is g3-3.

## Read first

- `~/docker/Projects/orb-os-net/docs/design.md` §7.4 (the resolve order, the *"no longer shared"*
  sentence), §7.6 (the whole table, the Skip and Keep-my-version bullets, *"Save a copy to
  orb-os.net" waits*), §8.4 (the shared-name / handle-history that makes a rename resolve), ruling
  26 and 27.
- `docs/specs/g3-design.md` and `docs/specs/g3-1-install-id-and-release-check.md` (this step reads
  g3-1's `GET /api/release/latest` cache; do not re-fetch the release in the list's own path).
- `tools/orb-manager/siteimport.go` — `ThemeOrigin`, `themeOriginPath`, `readThemeOrigin`,
  `removeThemeOrigin`, the `siteimport` `httptest.NewServer` fake, `routesSiteImport`, one
  `requireControl` handler: the origin record and the network-client pattern to copy.
- `tools/orb-manager/library.go` — `LibraryFile` (`Origin` field), `listThemeLibrary`, the
  `/api/library` handler `h_library`; how a theme's `file`, `app`, `title`, `origin` are listed.
- `tools/orb-manager/state.go` — `hubState`, `saveState`/`loadState` round-trip (this step adds a
  choices field and must survive it); `TestStateFileHoldsNoSecrets`.
- `tools/orb-manager/web/index.html` `~25–67` (`<main id="manage">`, `#mn-libthemes` as the section
  to sit beside); `tools/orb-manager/web/app.js` `show`(~81, already calls `loadLibThemes()`),
  `hubFetch`(~39), `loadLibThemes` and `libAppTitle` (H1b-3), `tag(...)` in `themeLibraryRows`
  (~4504, the DOM row builder to imitate), `fwUpdateDue`/`current.row.update`/`updAllApps`/
  `updateGate`/`paintUpdateAll` (~1126, the existing per-orb update reads to *reuse, not replace*).
- `tools/orb-manager/ui_test.go` — `readWebFile`, `readAppJS`, `regionOf`, `blockOf`,
  `blockOfLine`, `TestSiteImportDoorInTheLibrary` (~1735): the source-pin style to write the UI
  test in.
- `docs/specs/h1b-3-library-import-door.md` — the section/behaviour/test structure to imitate.

## Contract

### Storage (`state.go`)

- `type UpdateChoice struct { Kind string \`json:"kind"\`; Key string \`json:"key"\`; SkippedVersion int \`json:"skippedVersion,omitempty"\`; Keep bool \`json:"keep,omitempty"\` }`.
- Add `UpdateChoices map[string]UpdateChoice \`json:"updateChoices,omitempty"\`` to `hubState`, keyed
  by `Kind + ":" + Key`. For a theme `Key` is its `origin.designId`; for an app its slug; for
  firmware `"hub"`. Save and load it like every other field (round-trip; a missing key is "no
  choice"; corrupt file ⇒ start with none, never invent a choice).

### The resolve client (`siteimport.go`, beside the origin helpers)

- `type resolveAnswer struct { Resolved bool; DesignID, App, SharedName string; Version int; Gone bool }`.
- `resolveOrigin(ctx, o *ThemeOrigin) (*resolveAnswer, error)` — `GET o.site + "/api/hub/resolve"`
  with the query the origin carries (`?site=…&designId=…&version=…`; the name-based §7.4 point-3
  lookup is the site's resolve handler's job and is H3's route, so G3 sends the record it has).
  Parse at minimum `{designId, app, version, sharedName}` for a resolved design (the answer is the
  design's **current** version and **current** shared name, §7.4). A **404 or 410** is not a
  transport error: it is the design *"no longer shared"* → `Resolved:false, Gone:true`. Any other
  error → returned (the row says it could not be checked, and never claims up-to-date). **The exact
  response fields are owned by slice H3**; code to this minimum and note it in the report.

### The aggregate endpoint (`release.go` or a new `updates.go`)

- `GET /api/updates` (`requireControl`, `serveUpdates`) → `jsonOK` `{count, items:[…]}`, one item
  per offer, each `{kind, key, title, from, to, renamed, gone, choices:[…]}`:
  - `kind:"manager"` — from the g3-1 cache when `Verdict=="upgrade"`: `from` = `Current`,
    `to` = `Latest`, `choices:["download"]`. Never trigger a fetch here: report the cache.
  - `kind:"firmware"` / `kind:"app"` — reflect the **existing** reads (`fwUpdateDue`/`current.row`
    for the orb in hand; the shelf app set). `choices:["skip"]` for apps, per §7.6. No new install
    path: the row links to the existing per-orb action; the amber buttons stay.
  - `kind:"theme"` — for each `/api/library` theme with an `origin` that is not kept and not
    skipped-at-this-version, after `resolveOrigin`: if the resolve version > `origin.version`,
    offer it: `from` = the theme's title/`sharedName`, `to` = the resolve's `sharedName`, `renamed`
    = the shared name differs, `choices:["update","skip","keep"]` (`update` becomes live in g3-3).
    A `Gone` resolve → a `kind:"theme", gone:true` row, `choices:[]`. A theme with a `keep` choice
    is **never** listed (its origin is already removed; the `keep` entry just records the intent so
    a re-import stays personal).
- `POST /api/updates/skip` `{kind, key, version}` (`requireControl`) — record
  `SkippedVersion` on the item's `UpdateChoice`, `saveState`. The next *newer* version is offered
  again (§7.6), so the listing compares against this value.
- `POST /api/updates/keep` `{kind, key}` (`requireControl`) — set `Keep` on the theme's
  `UpdateChoice`, **and** take the theme's `.origin.json` sidecar away with `removeThemeOrigin`,
  then `saveState`. After this the theme has no `origin` on `/api/library`, is not offered, and is
  never resolved again (ruling 26: it stops tracking its design; the copy is unchanged, name
  included). Refuse (400 `{"ok":false,"error":…}`) for a kind other than `theme`.

### The Updates section (`web/index.html`, `web/app.js`)

- New `<section id="mn-updates" class="fleet">` in `<main id="manage">`, **after** `#mn-libthemes`,
  with `<h3>Updates <span id="mn-updates-count" class="count"></span></h3>`, a `<ul id="mn-updates-list">`,
  and a `<span id="mn-updates-state" class="hint">`. Header count = `item count` from the endpoint.
- `async function loadUpdates()`: `hubFetch("/api/updates")`, read `{ok,data}`; render rows through
  the DOM (`createElement`/`textContent`, never `innerHTML` with hub data) — the `tag()` pattern.
  Each row: the kind and `from → to`, then buttons by `choices`:
  - `download` → a button opening `data.downloadUrl` (g3-1's cache url) in a new tab;
  - `skip` → POST `/api/updates/skip {kind,key,version}`, then `loadUpdates()`;
  - `keep` → POST `/api/updates/keep {kind,key}` (confirm in the button's `title`, not a modal),
    then `loadUpdates()` and `loadLibThemes()` (the theme loses its `from <site>` tag);
  - `update` → a button with `id`/`data-kind`/`data-key` wired in **g3-3**; in g3-2 it is present
    but `disabled` with `title="Applying this update lands in the next release."` so the row is
    honest and no dangling handler exists.
  - a `gone:true` theme row shows the §7.4 sentence `"<name> is no longer shared on orb-os.net; the
    theme stays, and Orb Manager will stop checking it."` with one **OK** that POSTs `/api/updates/keep`.
  - empty: `li.hint` `Nothing to update.`; a failed read → `#mn-updates-state` `the update list
    could not be read: <msg>`, list left as it was.
- Call `loadUpdates()` wherever `show("manage")` runs `loadLibThemes()` (add the call beside it),
  and after a skip/keep press.
- `make check-web` must pass (new ids must be wired, no missing handler).

## Behaviour and edge cases

- No orb connected, nothing imported: the list shows the manager row when the cache says newer, and
  an empty otherwise; the release row needs no orb (g3-1).
- A theme whose origin resolves to the same version: not listed. Newer version: listed with the
  three choices. **Skip** hides it; a later resolve to an even newer version lists it again.
- **Keep my version**: after the press, `/api/library` shows that theme with **no** `origin`, it is
  gone from the list forever (until re-imported, which the existing duplicate question governs), and
  a fresh resolve would-not-be-asked (the row's absence proves it). The theme file and its name are
  byte-unchanged.
- A resolve that 404/410s: a `gone` row, one press of OK retires the origin like keep.
- A resolve that errors (offline site): the theme is not falsely "up to date"; the row (or a note)
  says it could not be checked.
- `GORB_SITE` unset / site behind OAuth still: the release row reflects g3-1's cached verdict; the
  manager row and the (empty) rest render regardless — an offline hub is not an error page.

## Acceptance criteria

From `tools/orb-manager`, `GOFLAGS=-buildvcs=false`, Go at `/usr/local/go/bin/go`:

1. `node --check web/app.js && node --check web/tabs.js` → no output.
2. `go test -run 'Updates|UpdateChoices|ResolveOrigin|KeepMyVersion|SkipThisVersion' -v .` passes,
   new tests in a new `tools/orb-manager/updates_test.go` and `siteimport_test.go`/`state_test.go`
   additions:
   - `TestKeepMyVersionStopsTracking` — a shelf theme with an origin (build it the way
     `TestSiteKeepFilesTheThemeAndRecordsWhereItCameFrom` does); `POST /api/updates/keep {kind:"theme",
     key:<designId>}` with a token → the `.origin.json` file is gone, `/api/library` shows the row
     with no `origin`, `hubState.UpdateChoices[key].Keep` is true and survives a save/load, and a
     later `GET /api/updates` lists no theme row for it (the origin is gone, so resolve is never
     called — assert a `resolveOrigin` call counter stays 0).
   - `TestSkipThisVersionHidesAndReoffers` — resolve fake at version 5, origin version 3 → a row;
     `POST /api/updates/skip {…version:5}` → no row; the fake moves to version 6 → a row again.
   - `TestResolveOriginReadsTheAnswerAndTheGone` — a fake `/api/hub/resolve` returning
     `{designId,app,version,sharedName}` → `Resolved`; returning 404 → `Gone:true, error==nil`;
     returning 500 → an error.
   - `TestUpdatesAggregateRows` — fake release cache `upgrade` + one theme needing an update → `GET
     /api/updates` returns a `manager` row and a `theme` row with `choices` containing `keep`, and
     `count` matching.
3. `go vet ./...` and `go test ./...` pass (every existing test unedited).
4. `go test -run 'TestUpdatesListOnHome' -v .` passes, in `ui_test.go`: `#mn-updates` is inside
   `<main id="manage">` and after `#mn-libthemes`; the section holds `#mn-updates-count`,
   `#mn-updates-list`, `#mn-updates-state`; `loadUpdates` posts to `"/api/updates/skip"` and
   `"/api/updates/keep"` and reads `"/api/updates"`; `show`'s manage branch calls `loadUpdates()`;
   the `update` button is rendered `disabled` in this step.
5. From `tools/orb-manager/web/runtime/_selftest`: `PY=python3 node headless-test.js` passes. From
   the repo root: `make check-web` passes.

## Out of scope

- **Applying** a site-theme update — same-name or renaming (g3-3). The `update` button is present
  but disabled here; no `/api/updates/apply` endpoint in this step.
- A self-update / binary swap (forbidden in G3).
- H2's *"Save a copy to my orb-os.net"* remix, and any linking (H2).
- The site's `/api/hub/resolve` **server** and the handle/shared-name history (H3).
- New firmware or app install paths, changes to `updateEverything`, `adopt.go`, `installflow.go`,
  `liveness.go`, or the existing per-tab amber buttons (they stay).
- Any `app.css` beyond what a row of the new section needs.

## Notes

- g3-2 is the **detection and the two refusals**; g3-3 is **the taking**. A worker may be tempted to
  wire the Update button here — do not: g3-3 owns it, and the disabled button is what keeps g3-2
  self-contained and green.
- The list is a *catalog*: the amber buttons on the firmware/apps tabs stay; the site-theme rows are
  the only ones whose whole flow lives here. The §7.6 firmware-pairing sentence is derived from
  g3-1's cache `Latest`, not a second fetch.
