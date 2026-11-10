# H2 hub step 3: My orb-os.net in the Theme library

> **Read `docs/specs/h2-design.md`'s "Lead review" block first: it is binding and overrides this spec where they disagree.** For this step: the section shows one of four states from `link-status`: none → **Connect to orb-os.net**; pending → `Waiting for orb-os.net…` with the URL as a fallback link; linked → `Connected as @<handle>` (or `Connected to orb-os.net` with no handle) with **Disconnect** and the My orb-os.net list; disconnected → **Disconnected from orb-os.net** with **Reconnect** (which is Connect). Pin those strings in the static test.

## Goal

Hub step 3 of 3, the last H2 artifact. The Themes tab of Orb Manager gains the
human half of §7.2: a **Connect to orb-os.net** button that opens the browser
and waits, and once linked a **My orb-os.net** list of the account's designs,
each with an Import button riding step 2's `{designId}` door, and a Disconnect
that means it. Upload stays dark (§8 (a)). After this step the H2 gate row is
operable end to end by a human with two hubs and one account.

## Read first

- `docs/specs/h2-design.md` §1, §5 — what the section is and is not.
- `web/index.html` — the site row (`mn-site-btn`, `mn-site-row`, `mn-site-link`,
  `mn-site-go`, `mn-site-state`, ~49–61): your markup joins this block, same
  `hidden` idiom, nothing between `mn-lib-upload` and the list.
- `web/app.js` — `loadLibThemes` (~5102: `data.site === true` hides
  `#mn-site-btn`/`#mn-lib-upload` and the whole `#mn-site-row`), the site
  importer (~5480–5545: `siteImportPress`, `hubFetch`, the `j.ok === false`
  read, `siteImportEnded`), and the row-tagging idiom at ~5162 for how imported
  themes already name their origin.
- `ui_test.go` ~1741 (ids present), ~1755 (`blockOfLine` for upload), ~1765
  (gone-when-closed), and `siteimport_test.go:856` (the closed-hub id sweep):
  every one of them grows, none gets weaker.

## Contract

- Markup, inside `#mn-site-row` so the existing closed-hub hiding covers it: a
  connect block (button `#mn-site-connect`, status line `#mn-site-conn`) shown
  when unlinked, and a designs block (`#mn-site-designs`, a `ul`, plus button
  `#mn-site-disconnect`) shown when linked — exactly one of the two visible
  after any state read.
- `app.js`:
  - `loadLibThemes`'s manage-view render also calls `loadSiteLink()` → GET
    `/api/site/link-status`, then draws the right block; linked additionally
    calls `loadSiteDesigns()` → GET `/api/site/designs`.
  - Designs rows are built through the DOM like the shelf already is (the site's
    `name` is foreign bytes, never markup): name, `app · v<version>`, an Import
    button, a private/unlisted hint word. Empty list: `Nothing saved on
    orb-os.net yet.`
  - Import click: disable the button, POST `/api/site/import` `{"designId": …}`,
    and feed the successful `data` to the SAME tail `siteImportPress` runs after
    its fetch — refactor that tail into `finishSiteImport(data)` used by both,
    no copy.
  - A `designs` read answering `disconnected from orb-os.net` flips the section
    to the connect block with that sentence in `#mn-site-conn` (the token is
    already gone hub-side; the UI catches up, no state to invent).
  - Connect click: POST `/api/site/connect`; the returned URL is opened by the
    HUB (step 1 did that); the page shows `open the link the hub printed if your
    browser stayed shut` with the url as visible text, then polls
    `/api/site/link-status` every 2 s while the connect block is on screen, max
    10 min, and redraws on the flip. Disconnect click: POST
    `/api/site/disconnect`, then redraw unlinked.

## Behaviour and edge cases

- Closed hub (`-site` off, every release today): every new id is hidden by the
  existing row-hiding — the two extended static tests must be what prove it, and
  the connect block must be INSIDE `#mn-site-row`, not merely next to it, or the
  hiding has to grow and the test grows with it. Pick one and pin it.
- The poll stops when the view leaves Manage, when the block flips linked, and
  when 10 minutes pass — three exits, and a hub left polling forever is a bug
  the UI tests cannot see, so the poll interval lives in one named constant and
  its stop conditions in one named function.
- No site word reaches the DOM unescaped: `loadSiteDesigns` inserts `textContent`
  and builds structure with `tag()`/`createElement`, exactly like the shelf rows.
- Import keeps working by LINK for anonymous pastes: the `mn-site-link` input and
  `mn-site-go` are untouched, and the connect block sits apart from them.
- `#mn-lib-upload` and its hidden-from line at app.js ~5117 do not move.

## Acceptance criteria

1. `export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH && cd
   tools/orb-manager && go vet ./... && go test -run
   'TestSiteUI|TestSiteStaticIds|TestSiteDoorShut|TestSiteLink|TestSiteConnect|TestSiteDesigns|TestSiteDisconnect|TestSiteImportId|TestStateFileHoldsNoSecrets|TestEditorAPI'
   -v .` → all PASS (`TestSiteUISectionIsThereAndShut` is the new static test:
   ids present in `web/index.html`, inside the site row, and named in
   `loadLibThemes`'s hiding path as `blockOfLine` reads it).
2. `go test ./... -count=1` → the whole Go suite green.
3. `cd .. && make check-apps check-hub check-editor check-web check-runner
   check-migrations check-editor-origin` → every gate green (this is the slice's
   last line, so the whole repo gate runs here and not earlier).
4. Manual-shaped smoke, scripted: run the hub's own `TestSiteConnectEndToEnd`
   rig one level up — a test that boots the hub handlers, fakes the site, presses
   connect THROUGH `/api/site/connect`, drives the callback, reads
   `/api/site/link-status` linked and `/api/site/designs` the list; name it
   `TestSiteLinkFullPress`. It is the UI's contract minus the pixels.

## Out of scope

- No CSS beyond what the site row already uses; no icons; no new view — this
  lives in the Theme library section exactly where Import lives.
- No keyboard shortcuts, no i18n. No changes under `internal/`, `web/runtime/`,
  or any app's editor.
- Upload stays hidden; H2 is read-only against the site.

## Notes

- The 2 s poll is cheap (`link-status` reads one small file) and dumb on purpose:
  the callback page cannot poke the hub UI, and a websocket for one boolean is
  how projects die.
- The one honest flake risk in this repo's suite — sysfs cable sweeps — already
  has its known shape; nothing here adds a timer that a CI box can trip, because
  every timer in step 1/2 code paths is either event-driven or injectable.
- h2-design.md §7's last hub rows (the fake-site two-link test) name these test
  functions; if a name drifts while implementing, fix THIS file, not silently.

