# S6 — the hub app library

**Status: executing. Format 3 landed; hub library, install flow and UI follow.**

S5 deleted the hub's embedded app pair and its `POST /api/orb/seed` door, and left
`tools/orb-manager/starter/` as committed artifacts an operator copies to a card by
hand. S6 replaces that hand step with a library: `apps/` and `themes/` beside the hub
binary, read at runtime, served to a board over the device's own upload doors.

Seven owner requirements drive it. They number as the owner numbered them.

## The shipped facts this rests on

| Fact | Where |
| --- | --- |
| A card write is a LAN HTTP door: `POST /api/v1/sd/app`, `/sd/theme`, `/sd/bundle` | `main/net/orb_net.c` |
| The serial console answers only `NAME SCAN MODE RESET PING PROV` — no file verb exists | `main/net/orb_net.c` |
| Mounting is implicit, plus `POST /api/v1/sd/rescan`; there is no mount door | `main/sd/orb_sd.h` |
| Storing a theme IS activating it, and the store reboots the board | `main/net/orb_net.c` |
| A reinstall reuses the slot and keeps its serial, so grants, enable and settings survive | `orb_app_install.c`'s `decide()` |
| A card row's identity words are read from its own slot header at boot and at install | `orb_app_install.c` |
| The header's trailing free word sits OUTSIDE the `hdr_crc` span | `orb_app_manifest.h`'s word enum |

The last row is why the version cannot go in the free word. A word outside `hdr_crc`
is rewritable on the card without disturbing any checksum, and the app would gain an
identity no CRC covers — the failure D19's consent rule exists to forbid. The field
gets a real slot inside the CRC span, which makes this a format change.

## 6 first: `.orbapp` FORMAT 3 (Option B)

FORMAT 2 -> 3, GEOM_ABI stays 3. Eight words (32 bytes, the `esp_app_desc_t.version`
shape) go contiguously after the D37 editor triple and before `W_HDR_CRC`:

    W_VERSION   = W_EDITOR_CRC + 1      # 32 bytes, 8 words
    W_HDR_CRC   = W_VERSION + 8
    HDR_WORDS   = W_HDR_CRC + 4         # 51 -> 59
    HDR_BYTES   = HDR_WORDS * 4         # 204 -> 236

**An empty version is legal and means unstamped.** Words 47..54 all zero read as
"no stamp". A zero is not a permission and not an identity claim, so it needs no
refusal, and it keeps every hand-written test vector buildable. A non-empty field
must parse. This follows the container's own precedent: `editor_size == 0` means
"ships nothing" and is not a refusal.

The grammar is `orb_version.sh`'s, one rule and not a second one:

    v<MAJOR>.<MINOR>.<PATCH>[-<pre>][+<7-SHA>][-dirty]

Each number is 1..2 digits with no leading zero, so `v99.99.99-rc.99+e1a2b3c-dirty`
is the worst case and lands on exactly 31 bytes plus its NUL.

Every leg of the three-way mirror moves in one step:

- `tools/orbapp.py` — the word map, `FORMAT`, `pack_header(version=...)`, the grammar
  check, `parse_header` returning `version`, `disclosure()` printing it.
- `main/app/orb_app_manifest.{h,c}` — the enum, `ORBAPP_FORMAT`,
  `orbapp_hdr_t.version[32]`, the `hdr_size` refusal, and the version's own refusal.
- `tools/orb-manager/internal/orbnet/appcontainer.go` — the mirrored table,
  `AppContainer.Version`, and `IdentityDiff` comparing it as the one tuple member with
  an order.
- `main/app/orb_app_install.c` — the version joins the duplicate tuple. Without it a
  re-upload whose only change is the stamp reads "app upload ... is unchanged", keeps
  the old bytes and says nothing: D37's editor-region bug exactly.
- `main/app/orb_app.{h,c}` — `orb_app_t.version`, `card_entry_t.version_store`,
  `card_fill` copying it.
- `main/net/orb_net.c` — `"version"` on the card tier only, and `APPS_ROW_MAX` raised
  to the new arithmetic, priced in `tools/host_orbapp_scan_test.py`'s `row_worst`.
- `tools/make_factory_apps.py` — a `VERSIONS` table, and `serial` derived from the
  triple so the string and the numeric word can never disagree.

**One deviation here, and it changes what the stamp means.** There is no `VERSIONS`
table, and `app/clock/CMakeLists.txt` no longer exists: the tree restructured, and the
clock is `main/clock/` with no component version of its own. The script reads
`version.txt` instead — the same file `tools/orb_version.sh` and root `CMakeLists.txt`
turn into the image's stamp. A typed table would be a second source of the release
number, and the two would drift the first time a release bumped one and not the other.
The consequence is loud: bumping `version.txt` makes the committed containers stale, and
the freshness gate in `tools/host_orbapp_install_test.py` says so until
`make_factory_apps.py` is re-run and the new bytes committed. The starter pair's serial
is `major*10000 + minor*100 + patch` and no longer the literal `1`, so the sentinel that
pinned `1` recomputes it from `version.txt` rather than trading one number for another.

`tests/fixtures/plain.orbapp` and `sentinel.orbapp` and both sidecars are regenerated.
`tools/host_app_fixture_test.py` fails first if the C mirror lags the writer, which is
the proof the pin works. FORMAT 2 files then refuse with
`app format 2; this build reads format 3`. Both test boards are re-baked and reflashed.

## 1. The library directory

`tools/orb-manager/starter.go` becomes `library.go`. Resolution, first hit wins, the
answer logged at startup and carried in `/api/status`:

1. `-library-apps <dir>` / `-library-themes <dir>`, one per kind
2. `-library <root>`, where `apps/` and `themes/` sit inside the root
3. `<dir of executable>/apps` — the shipped layout
4. `<userConfigDir>/orb-manager/apps` — where a browser import files a new app
5. `<repoDir>/tools/orb-manager/apps` — a run from a clone

**Two deviations from this section as first written, both deliberate.**

The plan listed `<dir of executable>/../apps` as a rung. It is not implemented. A
parent-of-exe-dir rung on a double-clicked release build reaches into `$HOME`, so
`$HOME/apps` would outrank the library the import button writes to — a rung reachable
only by accident. The dev case it existed for (a build under `bin/`) is already covered
by the clone rung, because `detectRepoDir()` walks up from both the cwd and the
executable.

The plan named the flags `-apps` and `-themes`. They are `-library`, `-library-apps` and
`-library-themes`, because one flag pointing at one root is the common case (a curated
library on a NAS volume) and the per-kind split stays available.

`tools/orb-manager/starter/` is renamed `tools/orb-manager/apps/`, and
`tools/orb-manager/themes/` joins it. `tools/make_factory_apps.py`'s `OUT_DIR` and
`tools/make_seed_theme.py`'s output path re-point to those two.

`GET /api/library` answers `{appsDir, themesDir, apps: [...], themes: [...],
refused: [{file, why}]}`. Every candidate is parsed through
`orbnet.ParseApp`. A file that will not parse is listed with its refusal and never
hidden — the device's own shown-with-reason rule, applied to the hub's directory.

`make release` copies both dirs into each platform dir, so the existing
`find . -type f` sha256 sweep covers them and a tampered library file breaks the
checksum set rather than shipping quietly.

Every other build target copies them too, since 2026-10-29. The exe-dir rung is the one a
downloaded hub stands on, and a `make manager` binary left inside the clone was rescued by
the clone rung below it — so the shipped layout went untested on the exact box that built
it, and a hub run from a directory with no clone in its ancestry answered `/api/library`
with `appsDir: ""`, both sources `none found`, and two empty lists. `make manager`,
`windows`, `windows-gui` and a standalone `make library` now mirror both kinds beside the
executable through one `LIB_COPY` define, merged rather than wiped so an app the operator
imported beside the binary survives a rebuild. `clean` removes the copies with `bin/`: the
tracked apps/ and themes/ in the package are the source of truth, and what sits beside a
binary is build output.

## 2. The install flow, and the narrowing

**The word is Install.** The flow went by "seed" through the design phase. The owner read
the first working UI and asked for the button to say Install, and one name per act is the
rule: the press does what the Apps face's Install button already does. The route is
`POST /api/library/install`, the NDJSON step is `install`, the file is `installflow.go`, and
the flag is `-no-autoinstall`. `-no-autoseed` stays accepted as a deprecated alias so a
desktop shortcut built against the first cut keeps working.

### The LAN path, and the wired copy

The LAN path needs no new file door. A wired copy does, and the console has none. The earliest moment
the hub can act is the first LAN answer after provisioning. The wired path is real
and it is its own slice: MTP `SendObject` over python-mtp, with the hub rebooting the
board into the share window, pushing, and rebooting back out.

`installflow.go` does the LAN path. It restores the gate shape `git show acfb8ec~1`
keeps for the deleted `seed.go` — firmware at or above the version that speaks
FORMAT 3, card mounted (one rescan and re-read), port claim held for the whole run,
every step asking the device before it writes — and swaps the `embed.FS` reader for
the library resolver.

Triggers:

- `joinDone()` in `web/app.js`, once per flash claim, and only when the ledger answers
  zero card rows.
- `POST /api/library/install?ip=`, streamed as NDJSON like `/api/flash`, for a press.
- `-no-autoinstall` turns the automatic arm off and leaves the press.

Only the clock rides the automatic run. Everything else in `apps/` waits for a press,
so a hub directory full of experiments cannot refill a board an operator emptied.

**The firmware floor is named, not probed.** `installflow.go` gates on `installMinFW`, the
first release whose reader speaks FORMAT 3. The status block carries no format field to ask,
so the alternative — send the file and read the per-file refusal — holds the board off the
sweep through a run that cannot land.

The floor is **`v1.1.28`**, and it took two wrong values and one hardware run to get there.
The record, because this number has now been wrong in both directions:

- The first cut said `v1.1.28` on the theory that a floor sits ahead of its reader's release.
  `TestInstallFloorIsNotAheadOfTheTree` rejected it for exceeding `version.txt`.
- That test was then "fixed" by lowering the floor to `v1.1.27`. This was the wrong reading of
  a right test: the floor may not exceed the stamp because the floor must name a firmware
  someone can actually build, and at that moment no firmware of any stamp read FORMAT 3.
- The board proved it. It self-reported `v1.1.27`, the gate waved it through, 660 KB crossed
  the room, and it refused with `app format 3; this build reads format 2`. Committed
  `main/app/orb_app_manifest.h` says `ORBAPP_FORMAT 2u`, and the bundle the hub ships is
  stamped `v1.1.27`. The FORMAT 3 reader had never been committed, let alone released.

So the reader change and the stamp are not separate facts, which is what the paragraph here
claimed until this rewrite. `.clinerules` and `docs/architecture.md` §7 both require an
unreleased change to what a shipped image promises to carry a `version.txt` bump, and the
container format is exactly such a change. `version.txt` is now `v1.1.28`, and `installMinFW`
names it: the floor is the first release that CAN read these files, and that release does not
exist outside this tree yet.

The consequence is worth stating plainly, because it is the operator's next action: the
firmware in this tree must be rebuilt (`idf.py reconfigure && idf.py build` — a bump with no
reconfigure keeps the cached stamp, per `.clinerules`), packed with `make firmware`, and
flashed. Until then the hub's floor refuses every reachable board before the upload, which is
the correct answer and costs one status read instead of a minute of WiFi. The bundle the hub
currently embeds is stamped `v1.1.27` and reads FORMAT 2, so the Update offer on the Manage
face cannot fix a board by itself.

`TestInstallFloorNamesTheReleaseThatReadsFormat3` replaces the test that misled us. It reads
the format from the firmware header rather than trusting prose, and fails all three drift
modes, each verified by mutating the tree and watching it refuse: a floor ahead of an
unbumped stamp, a floor at or behind the last release when the reader is uncommitted (the
state this section documented as correct), and artifacts whose format the tree's own reader
will not accept. The artifacts' format is read from their header word, never from their name.

**The post-flash wait line got shorter.** It ran on `watchLanBudget`, 45 seconds, and every
other tick first sent a line naming the address, the identity source and the phrase "s of the
45s allowed" — enough words to wrap onto two lines in the join dialog while a board was
booting. It now reads `still waiting - 12s of 45s`, under the headline
`waiting for the orb to restart`. The address and the source stay in the hub's own log,
where a hand reading a failure wants them. Nothing tests the prose; the tests here assert on
the budget and the outcome.


### Timeout and reporting fixes (from the first live press)

The first real press against a board on the LAN failed with `writing clock to the card:
orbnet: 192.168.2.32:80 unreachable: Post "http://192.168.2.32:80/api/v1/sd/app?overwrite=1":
context deadline exceeded`. Three separate defects sit behind that one line, and all three are
fixed.

- **The app upload was dialed on the management client.** `installOneApp` called
  `s.device.AppUpload`, and `s.device` carries `deviceTimeout` of EIGHT seconds.
  `h_app_upload` writes the whole container to the card inside the request and answers only at
  the end, so a healthy board's upload was cut mid-transfer and the refusal blamed the board.
  §4.1's ask-device-first rule could not rescue it: the run never reached the ledger. The
  firmware's own page allows 900 seconds (`x.timeout=900000`) for this same POST, and the
  theme upload beside it was already correct on `s.ota`. Both app-upload call sites now use
  `s.ota` (`otaTimeout`, 180 s, the ClassBulk lane) through `orbnet.AppUploadProgress`, the
  app twin of the `ThemeUploadProgress` that already existed.
  `TestAppUploadRidesTheBulkClient` proves it structurally rather than by sleeping past a
  timeout: two fakes, one per client, and the test reads which one the container reached. It
  fails on the old code and passes on the new.
- **The refusal sentence carried Go plumbing.** `deviceSentence` translated what an orb SAID
  and passed through what Go FELT, so the operator read a transport name, a URL and
  `Client.Timeout exceeded while awaiting headers`. The new `plainDeviceErr` words the
  transport cases instead: a timeout states the bytes already sent, says the write often
  finishes anyway, and points at the app list before re-pressing.
  `TestPlainDeviceErrRefusesToShowGoPlumbing` bans the plumbing strings.
- **D23 extended: the press now opens the shared progress panel.** This section originally
  specified a line in the card row, and that is what shipped — which for a 660 KB container
  crossing WiFi meant a `<pre>` under the row that filled in only when the run ENDED. That is
  the silent-spinner the S5 fix exists to kill. Both Install presses now call
  `fpOpen()`/`fpUI()` BEFORE the fetch and stream into that panel: the headline names the
  current step, and the bar is driven by the new `uploading` steps whose note is `N of M KB`.
  Success closes the panel; a refusal leaves it open, since no operation closes its own
  completion row. `TestLibraryInstallOpensTheSharedProgressPanel` reads each handler's OWN
  body, so a silent button cannot pass on the other press's `fpOpen`.
- The card's `#lib-install-log` stays the sink for the automatic join-time run, which already
  has the join dialog as its frame and must not take the panel mid-provision.


## 3. Themes ride along

Same run, after the app. Ask `GET /api/v1/themes` first: any theme row, or any active
theme, stops the step with `the dial already carries themes`. Otherwise upload the
first `.orbtheme` in the library, wait on the store witness, and then wait for the
dial to answer again — the store reboots the board as it commits.

## 4. Import on the Apps face

`POST /api/apps/import?keep=1`, behind the launch token. The hub parses the bytes with
`orbnet.ParseApp` before anything moves, so a corrupt container or a name disagreement is
refused locally and never costs a dial.

The button reads **Import an app** and it imports: the file lands on the shelf and the face
lists a row for it. It does not install, and the two checkboxes that used to sit beside it
("keep in library", "install on this orb") are gone. Getting bytes onto a board is always a
separate deliberate press on that row's Install, so an app imported from a laptop travels the
same path as an app that was already on the hub. The server still accepts `install=1&ip=`
for scripted callers; the face never sends it.

`?replace=1` overwrites a library file of that name. That is how a newer build reaches a hub
now that importing is the only way in, so the refuse-on-exists default stays for hand calls
and the import passes replace when the operator re-imports. The response reports
`overwrote:true` rather than leaving the shelf's new contents a matter of inference.

`POST /api/library/rm?kind=app|theme&name=` deletes one library file and nothing else. It
never dials a board, never uninstalls a row, and never touches a card, so the Apps face can
offer the pair in the order the operator means them. The name is one plain segment inside a
resolved library directory or the call is refused.

## 5. Themes face tabs

`/api/theme/apps` merges two sources: the device's theme-capable rows, and library
containers whose parsed header carries the `themes` cap. Each row gains
`source: "orb" | "hub"` and `installed`. A hub-only pane lists zero themes, says
`not on this orb`, and its Edit button opens `/editor/hub/<slug>/`, which slices the
region out of the library file with the reader the starter-freshness test already uses
and rides the same token-free sibling-asset middleware. Saving still dials
`/api/theme/upload`, so an app with no themes on any board becomes themeable the
moment it is installed — a first-party app is just one every operator can theme.

**Bench note (the operator-visible half, delivered).** The pane naming above was worth
nothing while `renderThemeTabs` returned early below TWO apps: a dial with one theme-capable
app — every dial in the fleet — drew no chooser, no name, and a state line reading "3 themes
on the card" with no owner. The chooser's job is naming the pane, not offering a choice, so
one app now gets one tab (`Clock themes (3)`) and the choice only appears when there is one.
One function, `themeFaceScope`, answers "whose themes is this pane" for the tabs, the state
line, the empty pane and the Edit row; it replaced three private guesses and the bare
"This app" they produced. `TestThemesFaceNamesItsAppEvenWithOnlyOne` pins it.

The library's theme rows name their app too: `LibraryFile.App` carries the container's own
section slug — FORMAT 3 files hold exactly one, the same fact the device keys the card stem
by — so the row reads `av-kit · theme for clock · 1150 KB`. The shipped-artifact listing
test asserts no theme row lists without one and no app row carries the field.

The merge itself (`source: "orb" | "hub"`, `/editor/hub/<slug>/`) is still unbuilt, and
`ThemeApp` carries neither key. What ships instead is D37's inversion plus the hub's own
editor loader, so a hub-only library theme is installed by press and only then appears on
the Themes face. Recorded as a deviation, not as done.

## 6. The seed theme is Aviator

The shipped seed was `themes/orb-default.orbtheme` carrying the embedded name `av-kit` —
a filename and a theme name with no substring in common. That cost more than tidiness. The
device files a theme under `theme_stem(slug, name)` (`main/art/orb_theme.c:277`), derived
from the **name** and ignoring the incoming filename entirely, so the card held
`clock-av-kit-5d6e52ace6b6` while the hub shelf listed `orb-default`. No Themes row could
line a library file up with the installed row it was, and the first diagnosis of that gap
was wrong because of it: the seed was reported as failing the firmware's compound-stem rule,
when in fact it passes the rule cleanly and the mismatch was one layer up.

Renamed to **Aviator** at the source. `assets/orb_default.img`'s name field now reads
`Aviator`, `make_seed_theme.py` emits `themes/aviator.orbtheme`, and the device files it as
`clock-aviator-8767574dd3cb`. Filename, embedded name and card token agree, so the seed
behaves like any theme the Composer sends. Only ten header bytes changed — the name words
and the CRC over them; every art byte is identical, so the pixels are the same face.

<!-- RETIRED 2026-10-29 by D37 (§0.12 of docs/app-platform-plan.md). The rename above was
correct and it did not finish the job, because the Aviator seed carried no class 9 preview
row and a slot image has no plane to lift: the theme a wiped board is GUARANTEED to own was
the one theme that could not picture itself, and /api/v1/sd/preview answered 404 for it.
themes/aviator.orbtheme and tools/make_seed_theme.py are deleted. The shelf ships one theme
now — themes/skeleton.orbtheme, the Composer's Skeleton preset baked through exportTheme3,
which attaches previewPlane3() to every bake — and tools/make_default_theme.py is the
browser-free gate that refuses a default with no preview row. This section stands as the
record of the rename and of the stem rule it was chasing, which is unchanged and still
enforced. -->

Two consequences worth knowing:

- **The derived id changed with the name.** A dial that already holds the old `av-kit`
  seed keeps it as its own row; the renamed seed arrives as a second theme rather than
  replacing it. On a fresh or wiped board, which is where seeding runs, this is invisible.
- **The invariant is now gated, not just satisfied.** `library_test.go` asserts a shipped
  theme's filename stem matches its own embedded title, so the next theme added to the
  shelf is held to the agreement instead of rediscovering the gap.

The `THEME_ID_HEX` comment that produced the bad diagnosis said the tail was 13 hex chars.
It is 12; the 13th byte from the end is the separator. Corrected in place.

## 7. Menu stops being an app

Delivered on the bench, with one refinement over this paragraph's first draft.

`web/tabs.js`'s `loadApps` now drops `compiled` rows, and `menu` has left `APP_TITLES`. The
refinement: a compiled row that FAILED stays. `apps_send_installed` emits the built-in tier
first, with `"compiled":true`, and it reports a C2 quarantine as a row — which is the one
built-in state carrying a verb (`Clear fault`). Dropping it unconditionally would hide the
only sentence explaining a dial that draws half a face. The predicate is
`!a.compiled || a.failed`, and the state line says how many built-ins it left out rather
than going quiet about a row that vanished.

The hub's reader still reports the ledger honestly; only the face filters. The dial's own
`/apps` page keeps the shell row — removing it is a firmware change and nobody asked.

Two things that paragraph left unsaid, both found while delivering it:

**The flag was not in Go.** `orbnet.AppInstalledRow` had no `Compiled` field, so the filter
existed only in JavaScript and every Go caller counting "apps on this dial" counted the shell
as the owner's inventory. The field is there now, and `orbfake` emits built-ins the way
`apps_send_installed` does — first, flagged, carrying `reason` and deliberately **no**
version or identity tuple. Two orbfake tests had pinned the opposite shape on the strength of
a reading taken from an older firmware (`"installed":[]` on a board running three apps);
correcting them is part of this change, because a double that hides the shell cannot model
the one built-in state with a verb on it.

**An app has three states, not four.** The face dropped its enable/disable toggle. An orb
either runs an app — in flash, in a slot, on the carousel, one fact — or it does not have it
installed. A switch that could hide an installed row off the carousel re-created a state the
product decided not to have, and it made "which apps do I have" unanswerable from the page.
`POST /api/v1/apps/enable` still exists on the device and the hub still proxies it; the Apps
face never asks, and a gate in `ui_test.go` fails if the button comes back.

What the face offers instead, per row:

| state | verbs |
|---|---|
| in the hub library, not on the dial | Install, Remove from hub library |
| on the card, not installed | Install, Remove from card |
| installed | Uninstall, Remove from card |
| installed and in the library, same version | Remove from hub library |
| installed and in the library, library newer | Upgrade, Remove from hub library |

The row names the dial's own version next to the shelf's, and `update available` appears only
when the ordinals say the library is ahead. A dial newer than the shelf is reported as ahead
of it, not stale. **Remove from card** walks the device's two doors in order — uninstall,
then `/api/v1/sd/rm` — because Uninstall alone leaves the container on the card and the
operator believes it gone. **Remove from hub library** deletes the hub's file and nothing
else: a running dial keeps running.

Two things this paragraph did not say and the bench found:

- **The double never sent the shell at all.** `orbfake` built its installed tier from
  `cardInstalledLocked`, which excludes built-ins, while `apps_send_installed` emits them.
  No hub-side test could have seen this bug, and the same gap hides any future one of its
  kind. `AppInstalledRow` also decodes no `Compiled` field, so a Go caller cannot filter
  what the browser can — recorded rather than changed, because adding the field shifts
  every row-count assertion in the suite and the face reads raw JSON through the proxy.
- **The join-time gate was reading a field that no longer exists.** `autoInstall` gated on
  `/api/fleet`'s per-row `apps.installed`, and `9f56b7e` turned the fleet store into an
  address book carrying no such field: the line became `if (undefined) return`, never
  fired, and every rejoin refilled a shelf the owner had emptied. It now reads the dial's
  own ledger and excludes compiled rows — count the shell and no dial ever reads empty, so
  the seed would have died in the opposite direction.
  `TestAutoInstallGatesOnTheLedgerNotOnAVanishedField` pins both halves.

## Tests and docs

- `ui_test.go` loses its ban on the install press and gains a mandate for the library
  press. A resurrected `//go:embed` of app bytes fails it, which is the S5 invariant.
- `library_test.go`, `installflow_test.go`, `import_test.go`, plus `themeapps_test.go`
  rows for the hub-only pane.
- `orbfake` grows `GET /api/v1/apps`, `POST /api/v1/sd/app`, `/apps/install` and
  `/apps/enable`, scripted off `orb_net.c` and not off the hub's client.
- `docs/architecture.md` loses the firmware-baked-factory-blobs claim. This file gains
  the bench verdict.

## What S6 does not do

- No wired file transfer. The console has no file verb and MTP is its own slice.
- No version on a compiled row: it was never a container, so the key is omitted.
- No hub-side registry of imported apps beyond the files themselves — the directory
  IS the library, and replacing a file needs no restart.
- No Format-2 reader. A FORMAT 3 build refuses FORMAT 2 by number.

