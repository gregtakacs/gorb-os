# Orb Manager: an install id in state.json, and the site release check

## Goal

Step **g3-1** of slice G3 (`~/docker/Projects/orb-os-net/docs/design.md` §7.6 Orb-Manager row,
§7.2 install id, ruling 25, §12 row G3). Two Go-only changes with no JavaScript, HTML or CSS:

1. an **install id** minted once and stored in `state.json` (`hubState.InstallID`), **never sent
   anywhere** until H2 links this Orb Manager to an account;
2. a **release check** that on start and daily asks the site's `GET /api/hub/releases/latest`
   (shipped by R1) whether a newer gorb-os release exists, compares it to Orb Manager's own
   version, caches the verdict, and serves it to the web from a new `GET /api/release/latest`.

This is the *data* behind the gate sentence *"a newer site release shows in Updates"*; the list
that renders it is g3-2. G3 adds **no** self-update: the check's only downstream choice is
Download, and the running binary is never swapped.

## Read first

- `~/docker/Projects/orb-os-net/docs/design.md` §0 (rulings 25, 26), §7.2 (the install id, the
  several-Orb-Managers rules), §7.6 (the table's Orb Manager and Firmware rows and the *"Updating
  itself in place may come later"* sentence), §9 (`GET /api/hub/releases/latest` row), §12 (row G3
  and the dependency note *"H2 needs G3's install id"*), §11 (the move to orb-os.net).
- `~/docker/Projects/orb-os-net/src/app/api/hub/releases/latest/route.ts` and
  `~/docker/Projects/orb-os-net/docs/specs/r1-design.md` — the **exact** JSON the check consumes.
  Its shape is the contract; invent no field.
- `tools/orb-manager/state.go` — all of it: `hubState`, `stateVersion`, `statePath`, `saveState`,
  `loadState`, `armState`; how a new field must survive a round-trip and a missing file.
- `tools/orb-manager/state_test.go` — `newHub()`, `s.statePath`, `TestRememberedAddressesSurviveARestart`,
  `TestStateFileHoldsNoSecrets`: the round-trip and no-secret patterns to copy.
- `tools/orb-manager/discover.go` lines ~145–175 — `upkeepLoop`: the ticker-on-context housekeeping
  pattern (its own `context`, one failure logged, retried next tick).
- `tools/orb-manager/internal/firmware/firmware.go` — `Compare(from, to Release) Verdict`,
  `ParseVersion`, `Release{Version, CompiledAt}`, the `Verdict*` constants (~705–830): the
  ordering to reuse for the version comparison. Do not reimplement it.
- `tools/orb-manager/main.go` — `var version` (~78), `type Server` (~81), `func (s *Server)
  routes(mux)` (~1073, and the `s.routesX(mux)` calls at ~1237–1246).
- `tools/orb-manager/firmware.go` lines ~255–285 — `jsonOK` (envelope `{ok:true,data:...}`) and
  `jsonErr`; `siteimport.go`'s `routesSiteImport` (~509) and one of its handlers, the pattern for a
  requireControl'd hub route that reads the network.
- `docs/specs/h1-1-hub-site-import.md` and `docs/specs/h1b-1-hub-library-editor.md` — the spec style,
  the Go-test style, and how a site call is faked with `httptest.NewServer`.

## Contract

### The install id (`state.go`)

- Add to `hubState`: `InstallID string \`json:"installId,omitempty"\`` with a one-line comment: a
  per-installation id, created once, an id and not a credential, and not sent until H2 (§7.2).
- `mintInstallID() string` — 16 bytes from `crypto/rand`, hex-encoded lower-case (32 chars). If
  `rand.Read` fails (it should not), return `""` and let `armState` leave the field empty rather
  than crash; a hub with no id still serves, and H2's link will retry.
- `armState` after `loadState`: if `s.installID == ""` and the loaded file named none, set
  `s.installID = mintInstallID()` and, when non-empty, `s.saveState()` so it is on disk before
  anything could read it. Add `installID string` to `Server` (beside `statePath`/`stateErr`).
- `saveState` writes `st.InstallID = s.installID`; `loadState` sets `s.installID = st.InstallID`.
  Round-trip, never regenerated when one already exists.
- The value is **never** placed on any outgoing request in this step. It reaches the network only
  in H2's `/connect`. Keep the field out of every `http.NewRequest`, request body and URL built
  elsewhere in the package.

### The release check (new file `tools/orb-manager/release.go`)

- `siteReleaseBase() string` — `strings.TrimRight(os.Getenv("GORB_SITE"), "/")`, or
  `"https://orb.takacsfamily.com"` when unset (ruling 1; §11's move is the only future change). A
  `GORB_SITE` that is set but not an `https://` origin (and not `http://127.0.0.1`/`localhost` for
  tests) is ignored in favour of the default, so a typo cannot make the check dial plaintext.
- `type releaseInfo struct { CheckedAt int64; Current, Latest, Notes, DownloadURL, Verdict string; Fetched bool }`
  — the cache on `Server` (`relMu sync.Mutex`, `rel releaseInfo`). `Verdict` is the `firmware.Verdict`
  string; `Fetched` is false before the first attempt and after one that could not reach the site.
- `fetchLatestRelease(ctx context.Context, base string) (version, notes, downloadURL string, err error)`
  — `GET base + "/api/hub/releases/latest"` (same client discipline as `siteimport.go`: bounded
  body, follows no redirect). Decode `{version, notes, assets:[{platform, filename, url}]}`; pick
  the caller's platform asset (windows/macos/linux from `runtime.GOOS`) for `downloadURL`, falling
  back to base `+ "/download"` when that platform is absent. A `404 {"error":"no_release"}` is not
  an error: it is "nothing to offer" (verdict none). Any other failure is returned as an error.
- `runReleaseCheck(ctx context.Context)` — call `fetchLatestRelease`, build
  `firmware.Compare(firmware.Release{Version: version}, firmware.Release{Version: siteVersion})`
  (`from` = Orb Manager's own, `to` = the site's newest): store `Verdict` only when it is
  `VerdictUpgrade`; store the info with `Verdict:""` otherwise (a `same`, `downgrade` or
  `unknown`/unparsable `dev` build offers nothing — fail closed). Mark `Fetched:true`, `CheckedAt`
  = Unix seconds. A network error sets `Fetched:false` and keeps no verdict.
- `releaseCheckLoop(ctx context.Context)` — started from main next to the other `go s.*` loops:
  sleep a short boot delay (a constant, e.g. 20s, so it never races serving), run once, then a
  24h `time.Ticker`; exit on `ctx.Done()`; a failed check is logged and retried next tick.
- `GET /api/release/latest` (`requireControl`, registered in `routes`, `serveReleaseLatest`) —
  `jsonOK` the `releaseInfo` (`CheckedAt`, `Current`, `Latest`, `Verdict`, `Notes`, `DownloadURL`,
  `Fetched`). It never triggers a fetch itself: it reports the cache, so an offline hub shows the
  last known answer, and a web reload cannot hammer the site.
- README: in `tools/orb-manager/README.md` add a short subsection under the update/firmware area
  saying Orb Manager checks `GET /api/hub/releases/latest` on start and daily (no sign-in), and
  offers only **Download** (`/download`) — it does not update itself in place. Match the README's
  tone. Do not touch the "Import from orb-os.net" section (that is H1b's).

## Behaviour and edge cases

- First run, empty config dir: `armState` mints an id, writes it, and a restart reads the **same**
  id back (never a new one).
- An upgrade from a pre-G3 `state.json` (no `installId` key): the id is minted once on the first
  G3 run and persists.
- A `state.json` that already carries an id: `armState` leaves it, `saveState` rewrites it
  unchanged.
- The site answers a newer tag → cached `Verdict:"upgrade"`, `/api/release/latest` reports it.
- The site answers the same or an older tag → `Verdict:""`. `404 no_release` → `Verdict:""`,
  `Fetched:true`. The site unreachable / TLS fail → `Fetched:false`, `Verdict:""`. A `GORB_SITE`
  pointing at plaintext (not loopback) → falls back to the default origin.
- Orb Manager's own version `dev` (untagged `go build`) → `VerdictUnknown` → `Verdict:""`: an
  untagged build never claims an update.
- The install id appears in `state.go` and `state_test.go` only: a grep of the whole package's
  diff finds it in no request-building code.

## Acceptance criteria

From `tools/orb-manager`, with `GOFLAGS=-buildvcs=false` and Go at `/usr/local/go/bin/go`:

1. `go vet ./...` clean.
2. `go test -run 'InstallID|ReleaseCheck|ServeReleaseLatest' -v .` passes, new tests in
   `tools/orb-manager/release_test.go` (and, for the id, additions in `state_test.go`):
   - `TestInstallIDIsMintedOnceAndSurvives` — an empty temp `state.json`: first `armState` sets a
     32-char lower-hex id; a fresh `newHub()` on the same path loads the **same** id; `saveState`
     mid-way leaves the file's `installId` unchanged across a re-load.
   - `TestStateFileHoldsNoSecrets` still passes and now also asserts `installId` is present (it is
     an id, not a secret) and that none of `s.token` etc. leaked.
   - `TestReleaseCheckOffersNewerOnly` — an `httptest.NewServer` fake serving R1's exact shape at
     `v9.9.9`; a `Server` with `version` forced to `v1.0.0` and `GORB_SITE` at the fake URL; after
     `runReleaseCheck` the cached `Verdict == "upgrade"` and `DownloadURL` is the fake's linux (or
     matching-`GOOS`) asset url. Flip the fake to `v0.0.1` → `Verdict == ""`.
   - `TestReleaseCheckRefusesNoRelease` — the fake returns `404 {"error":"no_release"}` →
     `Fetched true`, `Verdict == ""`. `TestReleaseCheckOfflineKeepsLastAnswer` — point `GORB_SITE`
     at a closed port → `Fetched == false`, a prior `upgrade` verdict is not cleared to a claim but
     `Verdict` reflects no fresh upgrade and `/api/release/latest` shows `fetched:false`.
   - `TestServeReleaseLatestShapes` — `GET /api/release/latest` with a token returns
     `{ok:true,data:{...verdict,latest,downloadUrl,fetched}}`; without a control token it is refused
     like every requireControl door.
3. `go test ./...` passes (every existing test unedited; in particular
   `TestStateFileHoldsNoSecrets`, `TestRememberedAddressesSurviveARestart`, `TestMissingStateFileIsQuiet`).
4. `grep -n "InstallID" tools/orb-manager/*.go` matches only `state.go`, `state_test.go` (and the
   `installId` json tag), never a file that builds a request (`api.go`, `siteimport.go`,
   `release.go`'s `fetchLatestRelease`, `installflow.go`, `adopt.go`).
5. `git diff --name-only HEAD~..` in the eventual chain confirms this step touched no `.js`, `.css`
   or `index.html`.

## Out of scope

- Any JavaScript, HTML or CSS (that is g3-2). No Updates list, no `/api/updates` aggregate.
- The `/api/hub/resolve` client and any theme-update detection (g3-2).
- Applying any update, ordinary or renaming (g3-3).
- A self-update / in-place binary swap (design forbids it for G3).
- Sending the install id anywhere (H2). No `/connect`, no token, no `hubLink`.
- Modifying `internal/firmware`, `adopt.go`, `installflow.go`, `liveness.go`, `updateEverything`,
  the Themes tab's amber buttons, or the existing `/api/orb/update` path.

## Notes

- The comparison uses the site's `version` as the gorb-os release tag AND as the firmware version it
  carries (§7.6). One fetch, one verdict, both rows; g3-2 renders the pairing sentence from it.
- Reuse `firmware.Compare` rather than a string compare: a `git describe` Orb Manager build is not
  a clean tag and must fail closed, which is what `Compare` already decides for firmware dots.
- `GORB_SITE` is the whole origin (scheme+host+port), not just a host, so the loopback-test allowance
  and the §11 move are one string.
