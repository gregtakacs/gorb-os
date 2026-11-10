# Orb Manager: updates that rename, applied in place (§7.4)

## Goal

Step **g3-3** of slice G3 (`~/docker/Projects/orb-os-net/docs/design.md` §7.4, ruling 23, §12 row
G3). The **actionable** theme update from g3-2's Updates list. Same name → the ordinary amber
**Update**. New name (the owner renamed it, or the handle changed) → an **update that renames**:
Orb Manager bakes the new-named theme into its library, installs it on the orb and — only if the
old one was the orb's worn face — makes the new one worn, and only then removes the old-named theme
from the orb and the library. Any failed step leaves the old theme installed and selected
(gate: *"a rename update swaps the theme on an orb and keeps it selected"*; *"a failed step leaves
the old theme"*). This lights the disabled `update` button g3-2 left and reuses H1b's own-editor
bake rather than a new one.

## Read first

- `~/docker/Projects/orb-os-net/docs/design.md` §7.4 (the rename paragraph verbatim, the *"if any
  step fails, the old theme stays"* and *"library entries are renamed the same way"* sentences),
  §8.4 (*"renames reach orbs as updates"*, the H3 note that Orb Manager bakes the new name itself
  from the bundle), ruling 23.
- `docs/specs/g3-design.md`, `docs/specs/g3-1-install-id-and-release-check.md`,
  `docs/specs/g3-2-updates-list-skip-keep.md` (this step lights g3-2's disabled `update` button and
  reads g3-2's `/api/updates` `renamed` flag + its `resolveOrigin`).
- `tools/orb-manager/siteimport.go` — the `/api/site/import`, `/api/site/bundle`, `/api/site/keep`
  handlers, `writeThemeOrigin`/`readThemeOrigin`/`removeThemeOrigin`, the fake-site test harness
  (`siteimport_test.go`). The rename re-bake drives these same doors (fetch bundle → bake with
  `/editor/hub/<app>/` → keep), so the update's new version reaches the library exactly as an import
  does.
- `tools/orb-manager/theme.go` — `handleThemeInstall` (`s.device.ThemeStore(ip, id)` installs and
  makes the stored theme active), `handleThemeDelete`/`deleteNext`/`themeRowByID` (remove a theme,
  naming what should wear next), the `ThemeList`/`ThemeEntry` `Chosen`/`Active` worn flags.
- `tools/orb-manager/web/app.js` — H1b's `openLibraryEditor(slug, ticket)` / `themeBridge`
  `theme.imported` path (the own-editor bake already wired), `themeDue`/`themeListWithRetry`, the
  g3-2 `loadUpdates`/`update` button.
- `docs/specs/h1b-1-hub-library-editor.md`, `h1b-3-library-import-door.md` — the bake-with-own-editor
  door and its keep test to reuse.
- `tools/orb-manager/ui_test.go` (`blockOf`, `readAppJS`) and `siteimport_test.go` (`httptest.NewServer`)
  for the test patterns.

## Contract

### Apply endpoint (`updates.go`)

- `POST /api/updates/apply` (`requireControl`) body `{kind:"theme", key:<designId>}` (g3-2's `key`).
  Reads the theme's `origin`, `resolveOrigin` it fresh (g3-2), and:
  - **Same shared name** (`!renamed`) → the ordinary update: re-bake the current version under the
    same name and install it on any orb wearing the old (reuse `ThemeStore`). Refuse with
    `{"ok":false,"error":…}` if the theme is no longer on the shelf.
  - **New name** (`renamed`) → the rename, in the strict order below, each step gated so a failure
    stops before any destructive one. The design id and origin are re-armed on the new file so the
    copy still tracks its design (only a *Keep my version* stops tracking).
- On success return `{ok:true, data:{oldName, newName, renamed:true|false, installedOn:[ip…]}}`.

### The rename, step by step (the order is the whole point)

1. **Fetch + bake the new-named theme into the library** — drive `/api/site/import` → the
   own-editor bake → `/api/site/keep` (H1b's doors, no orb needed) so the new `sharedName` file
   lands on the shelf with a fresh `.origin.json` at the resolve's version. **Nothing on the orb
   has changed yet.**
2. **Install on each orb wearing the old name** — `ThemeStore(ip, newID)`, which installs and makes
   the stored theme active; this is how *"keeps it selected"* is satisfied for the worn face: the
   store selects it. (For an orb where the old theme was **not** worn, install without selecting,
   using the existing keep-not-select path so the orb's chosen face is unchanged.)
3. **Only after every install succeeded, remove the old-named theme** — the orb row via
   `handleThemeDelete` (`ThemeDelete`, with a `next` so the orb lands on a sane face if the old one
   was active), and the library file plus its sidecar via `removeThemeOrigin` and the file removal.

A failure at **step 1 or step 2** aborts the run *before* step 3: the old theme is still installed
on every orb it was on, still worn where it was worn, and the library still holds it. A failure at
step 3 (a partial removal) leaves the *new* theme installed and selected **and** the old one still
present — never the reverse — so the orb is never left showing nothing; the row says the rename
"landed; the old copy needs a manual remove" and offers a retry.

### Wiring (`web/app.js`)

- `loadUpdates`' `update` button is no longer disabled: `data-kind`/`data-key` present → press
  `POST /api/updates/apply {kind, key}`, then `loadUpdates()` and `loadLibThemes()`; a renaming row's
  button reads `Update (renames to "<newName>")`.

### README

- Add to the Updates subsection from g3-1/g3-2: a theme imported from the site can be updated by the
  site's design even across a rename; a rename swaps the theme on the orb and keeps it selected, and
  a failed step leaves the old theme in place (the copy is only lost once the new one is on). Match
  the tone.

## Behaviour and edge cases

- Worn old face → after a successful rename the **new** theme is the worn face (assert the store was
  asked to select it; assert the old is gone).
- Not-worn old theme on an orb → new theme installed, that orb's worn face unchanged.
- Bake fails (editor refuses a newer schema / a wrong name) at step 1 → abort; old theme untouched
  everywhere; the failure sentence is the editor's/hub's, not a guess.
- Install on the orb fails at step 2 (orb unreachable, no room) → abort before removal; old theme
  still worn; `apply` returns `ok:false` naming the orb.
- Two quick presses → the second is refused while the first is in flight (a single in-flight lock),
  like `siteImportPending`.
- Apply on a design whose resolve now says `gone` → refuse with the §7.4 *"no longer shared"*
  sentence (g3-2 already retired it; a stale button press finds nothing and says so).

## Acceptance criteria

From `tools/orb-manager`, `GOFLAGS=-buildvcs=false`, Go at `/usr/local/go/bin/go`:

1. `node --check web/app.js && node --check web/tabs.js` → no output.
2. `go test -run 'RenameUpdate|ApplyUpdate|OrdinaryUpdate' -v .` passes, new tests in
   `tools/orb-manager/updates_rename_test.go`:
   - `TestRenameUpdateSwapsAndKeepsSelected` — a fake site serving the new-named bundle + a fake
     `resolveOrigin` returning a newer version at a new `sharedName`; a fake orb whose `ThemeList`
     shows the old theme `Chosen:true`. Drive `POST /api/updates/apply`; assert the fake orb received
     a `ThemeStore` for the new id (installed+selected) **before** a `ThemeDelete` for the old id, and
     the library gained the new-named file and lost the old with its sidecar.
   - `TestRenameUpdateFailedInstallLeavesOldTheme` — same, but the fake orb's store call fails; assert
     the fake orb received **no** `ThemeDelete` and the old file + sidecar are still on the shelf
     (gate: a failed step leaves the old theme).
   - `TestRenameUpdateNotWornDoesNotSelect` — old theme present but not worn → the store call names no
     select, the orb's worn face is left alone.
   - `TestOrdinaryUpdateSameName` — resolve returns a newer version at the **same** shared name → an
     install, no delete-old, no rename.
3. `go vet ./...` and `go test ./...` pass (every existing test unedited).
4. `go test -run 'TestUpdateButtonIsLive' -v .` passes, in `ui_test.go`: `loadUpdates`'s `update`
   button now contains `"/api/updates/apply"` and is **not** `disabled`; a renaming row's label
   contains `renames to`.
5. From `tools/orb-manager/web/runtime/_selftest`: `PY=python3 node headless-test.js` and
   `node headless-gorbhost.test.js` pass (the own-editor bake path this step reuses is unchanged);
   from the repo root: `make check-web` and `make check-runner` pass.

## Out of scope

- The Updates **list** itself, Skip/Keep, and the `resolveOrigin` client (g3-2) — reuse them.
- The install id, the release check, the manager row (g3-1).
- The site's `/api/hub/resolve` **server** and the handle-migration that *produces* renames (H3,
  §8.4). This step only consumes a resolve answer; the tests fake it, exactly as H1 faked
  `/api/hub/theme`.
- Uploading the theme to the site / *"Save a copy to my orb-os.net"* (H2).
- A new bake mechanism: drive H1b's existing doors; do not add another import path.
- Changes to `adopt.go`, `installflow.go`, `liveness.go`, the firmware/app update paths, or the
  existing per-tab amber buttons.

## Notes

- *"Library entries are renamed the same way"* is satisfied because the new file is a normal library
  theme (a keep wrote it) and the old library file is removed in step 3 — the library ends holding
  the new-named theme, exactly as the orb does.
- The store call is the select: `ThemeStore` both installs and activates (§7.4's *"makes it the
  orb's selected face"*), so a rename onto a worn face needs no separate select call; a rename onto
  a non-worn row must not select, which the existing keep-not-select install path already does.
- H3 is what starts sending newer shared-name versions from a real handle change; g3-3 makes Orb
  Manager able to land them the moment it can see one.
