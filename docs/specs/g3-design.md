# G3 design: install id, the release check, the Updates list, and updates that rename

Slice **G3** of the orb-os-net design (`~/docker/Projects/orb-os-net/docs/design.md`, §12 row
G3; gorb-os side, Orb Manager). This is the **design** for the slice: what G3 adds, the
contracts it must honour, and how it splits into the steps another worker runs one by one
(`g3-1`, `g3-2`, `g3-3` in `docs/specs/g3-chain.json`). G3 adds **no site code** and touches
nothing outside `tools/orb-manager/` and these `docs/specs/` files. It builds **on** Orb
Manager's existing app/theme updates from its own shelf — it does not replace them.

## Gate (§12, row G3)

The four sentences the reviewer checks, and where each lands:

- **"a newer site release shows in Updates"** — g3-1 (the client + the cached verdict + the
  self row's data) and g3-2 (the list renders it).
- **"a rename update swaps the theme on an orb and keeps it selected"** — g3-3.
- **"a failed step leaves the old theme"** — g3-3.
- **"Keep my version stops checks for good"** — g3-2.

## Dependency position

`docs/design.md` §12's notes: *"G3 can start after H1; H2 needs G3's install id, and H3 needs
H2, G3's rename updates, W5 and M1's runner."* So G3 follows H1/H1b (already merged: the
`ThemeOrigin` sidecar `siteimport.go` writes, the `/editor/hub/<app>/` own-editor bake, and the
Theme-library home section from `h1b-3`). G3 is **upstream** of H2 and H3: it ships the install
id H2 links with, and the rename-application H3 drives with `/api/hub/resolve`.

## What G3 adds, mapped to the design's sentences

### The install id — §7.2, ruling 25

> **Install id:** Orb Manager gains an id created once and kept in `state.json` (G3), sent with
> `/connect` along with its computer name, platform and version. Linking the same install again
> **replaces** its connection instead of adding a second.

- **Format.** A hex string from 16 random bytes (`crypto/rand`), lower-case, 32 characters. It
  names *this installation of Orb Manager on this machine*, not the machine and not the person.
  Nothing in it derives from a hostname or a MAC, so it cannot leak either.
- **Where it is stored.** A new field on `hubState` (`state.go`): `InstallID string
  \`json:"installId,omitempty"\``. `state.json` lives at `<UserConfigDir>/orb-manager/state.json`
  and is written by `saveState` (0600, whole-and-renamed) and read by `loadState`; the install id
  rides that same file, so `TestStateFileHoldsNoSecrets`' discipline still holds — it is an id,
  not a credential, exactly like the remembered addresses already in that file.
- **When it is made.** Once, in `armState` (`state.go`), on the first run that finds no id: if a
  loaded `state.json` carries none (a first run, or an upgrade from a version before G3), mint one
  and `saveState()`. A hub that already has one never re-mints it — relinking must replace the
  same connection, not a fresh one (ruling 25).
- **Never sent until H2.** G3 writes the id and reads it back; it does **not** put it on any
  request. The `/connect` exchange and `hubLink.tokenHash` that send it are H2. A grep of G3's
  diff must find `InstallID` only in `state.go` and its test — never in an `http.NewRequest` or a
  request body. This is the single hard boundary of g3-1 and its test asserts it.

### The release check — §7.6 (Orb Manager row), ruling 26, §9, R1

> **Orb Manager itself** | new (G3): on start and daily, `GET /api/hub/releases/latest` (public,
> no connection needed) | **Download** (opens `/download`). Updating itself in place may come later

- **When it runs.** On start (after a short boot delay, so it never races the hub's own serving)
  and every 24 hours, in a `releaseCheckLoop` modelled on `discover.go`'s `upkeepLoop` (a ticker
  on a `context`, one failure logged and the next tick retried). It reaches the network, so it is
  its own loop, not folded into the LAN-only `upkeepLoop` that *"sends nothing"*.
- **What it compares.** The site's newest-live `version` (the R1 endpoint's top-level field) with
  Orb Manager's own build `version` (`main.go`'s `var version`, an `ldflags` tag like `v1.2.37`, or
  `dev`/`git describe` for an untagged tree). The comparison reuses `internal/firmware`'s
  `Compare(from, to Release) Verdict` and `ParseVersion` — the ordering that already decides
  firmware amber dots. If either version does not parse (e.g. Orb Manager built `dev`), the verdict
  is `VerdictUnknown` and **no update is offered**: the check fails closed, the same reason
  `liveness.go`'s amber badge refuses to promise an update it cannot prove. The site's `version`
  doubles as the firmware it carries (§7.6), so this one number answers both the Orb Manager row and
  the firmware pairing sentence.
- **What it shows.** A cached `{checkedAt, current, latest, verdict, notes, downloadUrl}` the web
  reads from a new `GET /api/release/latest` (requireControl). The Updates list turns a
  `VerdictUpgrade` into one row. **The row's only action is Download**, which opens the site's
  `/download` in a browser — never a byte download, never a swap of the running binary. *"Updating
  itself in place may come later"* is honoured exactly: G3 adds **no** self-update path; the
  existing firmware and app update machinery in `adopt.go`/`installflow.go`/`web/app.js`
  (`updateEverything`) is untouched by this check.
- **The endpoint is a fixed shape, not an invention.** `route.ts` (R1, shipped) answers
  `200 {version, publishedAt, notes, assets:[{platform, filename, sha256, size, url}]}` or
  `404 {error:"no_release"}`. The client reads only `version`, `notes` and the platform's `url` for
  the Download link. It must not expect any field R1 does not send.
- **The default host.** The release check runs with no connection and no prior import, so it needs
  a built-in site host. It reads `GORB_SITE` (a full `https://` origin) and otherwise defaults to
  `https://orb.takacsfamily.com` — ruling 1 (the site starts there; the move to `orb-os.net` is
  §11). See **Owner questions**: the §11 move changes this default and nothing else.

### The Updates list — §7.6, ruling 26

> Orb Manager gets one **Updates list**, with a count in its header, gathering every kind of
> update. The per-tab amber **Update** buttons stay; the list is a single place to see them all.

- A **home-view section** (`#manage`, after the Theme library `#mn-libthemes`), because it gathers
  updates a hub has whether or not an orb is in hand (the release row needs no orb at all).
  `show("manage")` re-reads it beside `loadLibThemes()`. The per-tab amber buttons stay: the list
  is a place to *see* everything, not a new way to *do* the per-orb `updateEverything` run.
- **What it lists** (the §7.6 table), each row a `{kind, title, from, to, choices}`:
  - **Orb Manager itself** — from the release check. Choice: **Download**.
  - **Firmware** — the existing per-orb read (`fwUpdateDue`/`current.row.update`), plus the §7.6
    sentence when a newer Orb Manager exists: *"Orb Manager v1.2.17 brings firmware v1.2.17"*.
    Choice: the existing firmware update.
  - **Apps** — the existing shelf read (`updAllApps`/`updateGate`). Choice: **Update**, **Skip this
    version**.
  - **Themes from the site** — §7.4, resolved from the `ThemeOrigin` records H1/H1b wrote. Choice:
    **Update** (renaming when the name changed — g3-3), **Skip this version**, **Keep my version**.
- **Skip this version** hides that one update; the next newer version is offered again. Stored in
  `hubState.UpdateChoices` keyed by the item, remembering the exact version skipped (for a theme,
  the design id + the version it was offered at; for an app, slug + offered version).
- **Keep my version** turns the imported theme into the user's own copy: Orb Manager forgets where
  it came from and never checks it again, and it stays exactly as it is, **name included**
  (ruling 26: *"The copy isn't renamed"*). Mechanically that is `removeThemeOrigin(themePath)` —
  take the `.origin.json` sidecar away and the theme has no origin for §7.4 to trace, so it drops
  out of every future check; and a `keep` choice so nothing re-adds it. The choice is stored in
  `hubState.UpdateChoices` too, because a later re-import of the same design must not resurrect the
  tracking of a copy the owner claimed (the §7.6 duplicate question covers the re-import itself).
  *"Save a copy to my orb-os.net"* (the private remix) needs the account link and so waits for H2 —
  see below.

### Updates that rename — §7.4, ruling 23

> **New name** (the owner renamed it, or the handle changed): an **update that renames**. Orb
> Manager installs the new-named theme, makes it the orb's selected face if the old one was, and
> only then removes the old-named theme. If any step fails, the old theme stays. Library entries
> are renamed the same way.

- **Discovery** (g3-2): for each shelf theme carrying an `origin`, ask the site `GET
  /api/hub/resolve` (§7.4, §9) with the origin record; the answer is the design's current version
  and current shared name. §7.4's order — (1) the origin record, (3) the name through the handle
  history — is the *site's* job in the resolve handler; G3 only sends what it has and reads the
  answer. `/api/hub/resolve` is H3's route, so G3 ships the **client** and tests it against a fake
  server (the same discipline H1 used for `/api/hub/theme/<id>`); a hub whose site does not serve
  it yet simply lists no theme rows, exactly as it lists none when there is no origin.
- **Application** (g3-3), same name → the ordinary amber **Update** (the existing
  `ThemeStore`/library-push install path, `keep=1`). New name → the rename, in this order so a
  failure is never destructive: (1) bake the design at the new shared name into the library with
  H1b's own-editor door (`/editor/hub/<app>/` + `/api/site/keep`), under the resolve's current
  `sharedName`; (2) install it on the orb and, **only if the old theme was worn** (`Chosen`/`Active`
  on its `ThemeList` row — `theme.go` `deleteNext`/`handleThemeDelete`), make the new one the
  selected face via the store path; (3) **only then** remove the old-named theme — the orb row
  (`handleThemeDelete`) and the library file with its sidecar. Any step that fails stops the run
  before step 3, so the old theme — on the orb and in the library, still selected — is untouched.
  The rename is one transaction in the operator's eyes: they see the theme under its new name or, if
  it failed, exactly the theme they had.
- **A design that stopped being shared**: resolve answers 404 (private/unknown) or 410 (deleted).
  §7.4: *"says so once, keeps the theme, and stops checking it"* — the Updates row reads once
  *"Skeleton by @greg is no longer shared on orb-os.net; the theme stays, updates stop"*, and the
  origin is retired like Keep-my-version does, so no later check re-offers it.

## The gate, as concrete checks

- **g3-1**: a fake site serving R1's shape at a newer tag → the cached verdict is `upgrade`; the
  web's `GET /api/release/latest` reports it; an older/equal/`no_release`/unreachable site → **no**
  update offered; `hubState.InstallID` round-trips and is minted exactly once; a grep proves the id
  appears in no request.
- **g3-2**: the home-view Updates section renders the Orb-Manager row from the release cache; a
  theme row with a fake resolve showing a newer version renders **Update / Skip / Keep my version**;
  a **Skip** hides that version and re-offers a later one; **Keep my version** removes the origin
  sidecar, and the theme then lists **no origin** on `/api/library` and is never re-checked (the
  gate's fourth sentence).
- **g3-3**: a fake resolve returning a **new name** drives the full swap and the tests assert,
  end-to-end against a fake orb, that the new-named theme is installed and, when the old one was
  worn, is the selected face; and that a **failure injected at the install or select step leaves
  the old theme installed and selected** (gate sentences two and three).

## What waits for H2 or H3, and why

- **Sending the install id** (the `/connect` exchange, `hubLink`, Connected Orb Managers on `/me`) —
  H2. G3 only mints and stores it. (Ruling 25, §7.2.)
- **"Save a copy to my orb-os.net"** under Keep my version (the private remix, *"remix of Skeleton
  by @greg"*) — H2, because it posts a design into an account and needs the link's token (§7.6). In
  G3 the row offers Keep my version, which does its whole job locally (stop tracking, keep the
  name); the extra upload button is simply absent until there is a link to post through.
- **"Upload to orb-os.net"** on the Theme library stays the disabled button `h1b-3` left it — H2.
- **`/api/hub/resolve` itself** and the handle/shared-name history it reads — H3. G3 ships the
  client and the application; the live theme-update rows only appear once H3's route answers. The
  site's handle-change migration that *creates* the rename updates is H3 too (§8.4).
- **App updates from the site** (`GET /api/hub/apps`) — A1/§7.5. G3's Apps rows come from the hub's
  own shelf only.

## Owner questions

- **The release check's built-in default site host.** G3 must reach `/api/hub/releases/latest` with
  no import and no link, so it needs a default origin. Ruling 1 (the site *starts* on
  `orb.takacsfamily.com`) and §11 (it *moves* to `orb-os.net`) point at two answers, and only the
  owner knows when the move happens. **Options:** (a) default to `https://orb.takacsfamily.com`,
  overridable by a `GORB_SITE` env var; (b) default to `https://orb-os.net` now; (c) no default — a
  required setting the operator fills before the first check. **Chosen: (a)**, as the one most
  consistent with the rulings (ruling 1 is the present, §11 the future, and an env override makes
  the move a config change, not a rebuild). The affected step is **g3-1**; when §11 lands, the only
  change is the default string.
