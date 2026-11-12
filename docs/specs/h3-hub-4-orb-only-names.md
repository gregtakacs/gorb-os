# H3 hub step 4: themes that live only on an orb — found by card, asked by name, adopted like an import

## Goal

Slice H3, hub step 4 (docs/specs/h3-design.md §7.3). A theme pushed to an orb by hand, or
installed from a file years ago, has **no library file and no origin record**: nothing in the hub
knows its design id, so the update machinery cannot ask about it. This step finds those themes by
reading the **orbs' own cards**, asks the configured site by **name + app** exactly as a file
import is asked, and on an answer adopts the theme into the library through the existing
bake-and-apply path — so from the next list onward it is an ordinary tracked theme, renamed on the
orbs when the name moves. A gone answer marks and never rows (step 3's policy, reused). No
site-side change; the fake site answers design.md §4.

## Read first

- `docs/specs/h3-design.md` §7.3 (this step), §7.2 (the answer policy reused), §4.
- `docs/specs/h3-hub-3-name-resolve.md` — `resolveName`'s signature (site, **app**, name), its
  classification table, and the gone/NotYet silence this step must not reinvent.
- `tools/orb-manager/updates.go` — `forEachOrbCard` (:715: `s.grants.list()` →
  `s.device.ThemeList(ip)`, a read error passed over), `orbWord` (:735, how an orb is named in an
  operator sentence), `themeUpdateRows` (:210, the arms step 3 changed), `updateBake` (:618:
  fetch → bake or NeedBake ticket → shelf file with a fresh `ThemeOrigin` → install/select/remove),
  `applyPayload`'s fields (:378: `OldName`, `NewName`, `Renamed`, `InstalledOn`, `NeedBake`,
  `Ticket`, `App`, `Note`), `choiceFor`, and `shelfByDesign` (:744, "does the shelf hold this").
- `tools/orb-manager/internal/orbnet/themes.go:61` — `ThemeEntry`: `Name`, `App` (the stem's first
  token; `""` only on firmware too old to answer it), `Active`, `Stored`, `OK`.
- `tools/orb-manager/internal/orbfake/orbfake.go` — `Theme{Name,App,ID,Bytes,OK,Active,Stored,
  Design,Preview}`, `New(themes …Theme)`, `Orb.Themes()`, `Orb.Calls()`; the rig
  `updates_test.go` uses to stand an orb in for a real board.
- `tools/orb-manager/siteimport.go` — `writeThemeOrigin`/`readThemeOrigin`/`themeOriginPath`,
  `siteTicketStore`/`siteTicket`, `checkBundle`, `fetchCheckedBundle`, `siteLinkRefused`,
  `jsonOK`/`jsonErr`. `updates_test.go` + `siteimport_test.go` for the two test styles.

## Contract

- New file `tools/orb-manager/orbonly.go`, two control doors registered like sitelink.go:888's
  block: `GET /api/site/orb-themes`, `POST /api/site/orb-themes/adopt` (`{"name":…}`), both
  `requireControl`.
- **Enumeration** (`func (s *Server) siteOrbOnlyThemes(ctx) ([]orbOnlyTheme, error)`): walk
  `forEachOrbCard`; for each card entry (deduplicated by `Name`, first orb seen wins) a candidate
  **only when all of**: `Name` parses through `checkSharedName` with both halves present;
  `App != ""`; the **library holds no theme whose title equals `Name`** (the tracked case, step
  3's business); and the ledger does not mark it gone. Answer:
  `{"themes":[{"name","app","orbs":["<orbWord(ip)>",…],"gone":false}]}` sorted by name; a
  ledger-gone entry is reported `"gone":true` with no orbs, so the tab can say the sentence and
  offer nothing. An orb that will not answer contributes nothing (the walk swallows it already).
- **Ledger**: `<configDir>/site-orb/` (0700), `<sha1(name)>.json` (0600, atomic write, the inbox
  hold's style), `{"name","app","askedAt","gone":bool,"designId","version"}`. Written **only** for
  a gone answer (durable: a once-public name is not re-asked every minute) and for a resolved
  answer whose adoption is unfinished. Removing the library theme (`removeThemeOrigin`'s path)
  deletes its ledger file: a theme removed and re-pushed by hand is a fresh candidate.
- **Adopt** (`POST /api/site/orb-themes/adopt`, `{"name":…}`): the name must be in the current
  enumeration, else `siteLinkRefused` (the same sentence as an unreadable paste, which is honest —
  the tab drew that button from that list). Then `resolveName(ctx, siteReleaseBase(), app, name)`:
  - **Gone** → write the ledger gone mark, answer `siteLinkRefused`, one log line. No row, no
    shelf write, no orb touched (review 5's policy, reused).
  - **NotYet / transport error** → `siteLinkRefused` + one log line, **no ledger entry**: *ask
    again next time* is the whole state.
  - **Resolved** → the hub just learned the design id, so run **the existing apply path**: a
    `ThemeOrigin{Site: siteReleaseBase(), DesignID: ans.DesignID, Version: ans.Version,
    ImportedAt: now}` handed to `applyThemeUpdate` (design id as key, the card's app,
    `oldName = name`, the answer as `ans`) so `updateBake` fetches the current bundle, bakes (or
    answers `NeedBake`+`Ticket`+`App` on a hub whose baker is the browser, as an import does),
    files the shelf theme with its origin record, installs on the orbs that answered, selects, and
    **removes the old card name after the install and select**. The door's answer **is**
    `applyPayload` — the keys `/api/updates/apply` answers — so the web flow is one path, not a
    second. The ledger's resolved entry is deleted once `shelfByDesign` proves the shelf holds the
    design and the answer carried no `NeedBake`; on a `NeedBake` answer it stays, and the existing
    browser-driven second half (editor keep → apply with `baked`) settles it.
- **`web/app.js`**: inside the From orb-os.net block, a **Themes on orbs only** sub-list (the
  enumeration's non-gone entries) — one row per theme, `<name> · <app> · on <orbWord list>`, with
  one **Track this theme** button posting the adopt door and rendering the answer the way an update
  row's apply result renders today; a `"gone":true` entry prints §8's `<name> is gone from the
  site.` with no
  button. Empty: `Nothing untracked.`
- No new background loop: the enumeration runs on the block's existing refresh and adoption is a
  press — a button, because the enumeration dials orbs. Minute-cadence asking is step 3's job.

## Behaviour and edge cases (the tests)

`tools/orb-manager/orbonly_test.go` — fake site as `siteimport_test.go` builds one, fake orbs as
`updates_test.go` builds them (`orbfake.New(orbfake.Theme{Name: "Clockface by @greg", App: "clock",
Stored: true, Active: true})`):

1. `TestOrbOnlyEnumeratesCards` — one orb carrying `Clockface by @greg`, one library theme titled
   `Skeleton by @g`: exactly the card is enumerated, with the orb word, and the library title is
   absent (tracked themes are step 3's business).
2. `TestOrbOnlySkipsHomemadeAndBlankApp` — cards `My Doodad`, ` by @greg`, and `Fine by @g` with
   `App:""`: none enumerated, zero resolve requests on the fake.
3. `TestOrbOnlyAdoptInstallsAndRemovesOldName` — the fake answers v6 `Clockface by @g2`: the shelf
   gains a theme whose origin record reads v6 (fields asserted), the fake orb's wear list shows
   the new name installed and the **old name gone after the select**, the ledger entry is deleted,
   and the answer's `Renamed` is true with `OldName`/`NewName` the two names — G3's apply
   assertions, on the adopt door instead.
4. `TestOrbOnlyAdoptNeedBakeKeepsLedger` — with `s.themeBake == nil` the answer carries
   `NeedBake`+`Ticket`, the ledger entry remains, and the browser's second half (editor keep →
   `/api/updates/apply` with `baked`) settles both.
5. `TestOrbOnlyGoneMarksAndStaysQuiet` — fake answers `{"gone":true}`: refusal answer, ledger mark
   written, and **two further enumerations send no request** while the entry reports `gone:true`;
   removing the shelf theme after a *resolved* ask brings the candidate back into the enumeration.
6. `TestOrbOnlySilentOnNotYet` — a plain 404, and a transport failure: refusal answer, no ledger
   entry, and the next enumeration asks again (request count proves it).
7. `TestOrbOnlyAdoptRefusesUnknownName` — a card not in the enumeration, an empty body, a name
   failing `checkSharedName`: the refuse sentence, zero requests, no ledger write.

## Verify

```bash
set -e; root="$(git rev-parse --show-toplevel)"; cd "$root"
! git -C "$root" diff --name-only delegate/h3-hub-3-name-resolve | grep -qvE '^tools/orb-manager/'
test -f tools/orb-manager/orbonly.go
grep -q 'site/orb-themes' tools/orb-manager/orbonly.go
export GOFLAGS=-buildvcs=false PATH=/usr/local/go/bin:$PATH
cd tools/orb-manager && go vet ./... && go test -run 'TestOrbOnly' -count=1 -v . 2>&1 | tail -30
go test ./... -count=1 2>&1 | tail -20
```

## Done

Seven `TestOrbOnly*` names as `--- PASS`; the whole hub suite green; `go vet ./...` clean; the
scope gate measured against **hub step 3's branch from the repo root** (review 15); `web/app.js`
changed in one block, and `ui_test.go` grows one case for the new block inside this step's diff
rather than having its existing assertions edited.


