# D37: Per-app theme editors ship inside the `.orbapp`

**Why this file exists.** The Theme Editor's home is wrong. It is the clock's
Watch Composer, wired to a top-level link at `web/index.html:14`, while every
other per-app concern lives on its own tab. The owner ruled a new shape on
2026-10-27: the Themes tab lists theme-aware apps, and each app brings its own
editor inside its own `.orbapp`. That ruling re-opens D28's boundary, D33's
"what lives in the browser," and D36's seed. This file records the decision, the
measured budgets, the container arithmetic, and the slices. Read it before
touching the Themes face, `orbapp.py`'s header, or `seed.go`.

**Status: RULED (owner, 2026-10-27). Not built.** Four parameters are settled
and not open for re-litigation:

| Parameter | Ruled value |
|---|---|
| Editor transport | Inside the `.orbapp`. No hub registry. No companion file. |
| Editor region cap | 2 MiB (0x200000) |
| FORMAT 3 baker | Hub-served shared resource |
| React/ReactDOM/Babel | Hub-served shared runtime |

## 1. The four rules

1. **Every app is a `.orbapp`.** Clock and HSB included. No app is
   pre-registered anywhere: not in the firmware, not in the hub binary.
2. **Flashing loads the OS only.** No app and no theme reaches the image. Apps
   are SD-card-bound: the file lands on the card before the dial can install it.
3. **The editor ships with the app.** An app that themes brings its editor. An
   app that does not, does not. One mechanism for the factory pair and for every
   third-party file (D36's point, now applied to editors too).
4. **The shared parts are hub-served, once.** The FORMAT 3 baker and the JS
   runtime are hub resources every editor imports. One copy per hub, not one per
   container. An app's own bundle carries only its UI and its section builder.

Rule 1 and rule 2 are already half-true. §2 says which half.

## 2. What the tree already does (verified 2026-10-27, v1.1.21)

**Done, needs nothing.** The firmware ships no apps. D35 left one compiled row:
the `menu` shell (`orb_app.c:294`, `orb_app.h:243` keeps `ORB_APP_BASE` at 3 so
retired indices 1 and 2 stay dead per C6). The clock and HSB reach their
compiled ifaces as card rows through `behaviour_kind` 3 and 1
(`orb_app.c:243-254`). `CMakeLists.txt:71` no longer stages
`assets/orb_default.img`. A bare flash boots to the menu with zero app rows and
zero themes (§40).

**Not done.** The hub pre-registers the factory pair. `seed.go` embeds
`seed/clock.orbapp`, `seed/color_picker.orbapp`, and
`seed/orb-default.orbtheme`. `main.go:824` exposes `POST /api/orb/seed`.
`app.js:1514` auto-fires it when a joined board reports zero themes.
`tabs.js::appFaceSeed` puts the button on the Apps face. Rule 1 deletes all of
it. §7.

**Already app-agnostic, and reusable as-is.** FORMAT 3 (`orb_bundle.h`) is one
`.orbtheme` holding N per-app sections addressed by slug, each an element table
plus an art bank. Theme identity is `sha256(slug/name)[:12]`, and the card stem's
first token is the owning slug (`orb_theme.h:51-57`). The whole hub surface
`/api/theme/{list,install,delete,rescan,preview,upload,commit,upload-status,stem}`
already ignores the app. The Themes face already has a per-app chooser. Only its
**source** is wrong: `renderThemeTabs` (`app.js:2325`) builds sub-tabs from theme
rows already on the card, so an enabled theme-aware app with zero themes is
invisible. §6 fixes the source, not the shape.

**The signal already ships.** `themes` is one of the four frozen caps
(`orb_app_manifest.c:478`, `orbapp.py:145`), and the Apps ledger row already
carries `caps` (`orb_net.c:5023`, `5049`). An app's theme-awareness is therefore
readable today without a new field.

## 3. The size arithmetic, measured before deciding

The cap is only survivable because the baker and the runtime move to the hub.
Measured on this tree:

| File | Bytes | Owner under D37 |
|---|---|---|
| `Watch Composer.dc.html` | 325,298 | app: the clock's UI |
| `watch-engine.js` | 276,917 | app: the clock's movement logic |
| `dial-renderer.js` | 50,140 | app: the clock's plate/geartrain |
| `orbtheme-baker.js` | 205,414 | **hub**: the FORMAT 3 writer |
| `support.js` | 69,150 | **hub**: the dc-runtime bootstrap |
| **Total today** | **926,919** | |
| App-owned after the split | **652,355** | inside a 2 MiB cap, 637 KB |
| Vendored React/ReactDOM/Babel | 5,300,000 | **hub**: served once |

App-owned payload is 652,355 B against the ruled 2,097,152 B ceiling. DEFLATE on
text drops it further. The clock fits with roughly 3x headroom. Without the
hub-served carve-out the same container needs 927 KB plus 5.3 MB of runtime, and
no sane cap holds that. **The hub-served shared parts are not an optimization.
They are what makes rule 3 affordable.**

Two flash facts that force the design:

- `ORB_APP_UPLOAD_MAX` is `ORBAPP_GEOM_BYTES_MAX + 0x20000` = **393,216 B**
  (`orb_app_install.h:206`). A 2 MiB region exceeds it, so a container carrying
  one is refused at the door as "too large." The ceiling must grow:
  `393,216 + 2,097,152 + a 4,096 B sector gap = 2,494,464 B`.
- The `apps` partition is `0x280000` = **2,621,440 B for 8 slots**, so
  **327,680 B per slot** (`partitions.csv`). A 2 MiB region cannot install. **The
  editor region never enters the slot.** It lives on the card, past the
  installer's copied prefix. Flash accounting is untouched, and the partition
  table does not move.

The region's home is therefore the SD card. The hub reads it out of the card file
over one new route (§5). Nothing about it reaches the `apps` partition.

## 4. The container change: a fourth region

`.orbapp` grows an optional **editor region**, laid out exactly as P7's code
region was. The precedent is the format's own: P7 added `code_off`/`code_size`/
`code_crc`/`wasm_mem_max` "INSIDE the header CRC's span … because a gate word
that lived outside the CRC could be rewritten on the card without disturbing any
checksum" (`orbapp.py:196-203`). The editor triple follows that rule to the
letter.

### 4.1 Header words

Insert after `W_WASM_MEM`, before `W_HDR_CRC`:

```
W_EDITOR_OFF = W_WASM_MEM   + 1
W_EDITOR_SIZE= W_EDITOR_OFF + 1
W_EDITOR_CRC = W_EDITOR_SIZE+ 1
W_HDR_CRC    = W_EDITOR_CRC + 1        /* moves up three words */
W_ASSET_OFF  = W_HDR_CRC    + 1        /* unchanged formula */
W_N_ASSETS   = W_ASSET_OFF  + 1
HDR_WORDS    = W_HDR_CRC    + 4        /* unchanged: asset pair + 1 free */
```

`HDR_WORDS`'s formula and the tail's shape do not change. `HDR_BYTES` grows by
12 B. The existing zero-check on the trailing free word
(`orb_app_manifest.c:160`, `w[ORBAPP_HDR_WORDS-1] != 0`) extends to the new tail
for free. Region offsets stay CRC-covered, so a card cannot move an editor
region without disturbing `hdr_crc`.

### 4.2 Versioning

`FORMAT` stays 2. `GEOM_ABI` bumps **2 → 3**: the same bump P5b's asset table and
P7's code region each cost, since the header grew. A v2 reader refuses on the
`hdr_size` equality check (`orbapp.py:966`) and on the free-word zero-check before
it reads one payload byte, and the existing `abi_min`/`abi_max` ladder carries the
refusal sentence naming both numbers. An app carrying an editor region must
declare `abi_max ≥ 3`.

### 4.3 Placement, derived and never stored-and-trusted

`check_file_layout` gains one equality, in the style of the four it already holds
(`orbapp.py:1103-1125`):

```
editor_off = align_up(code_off + code_size, SECTOR)      /* SECTOR = 4096 */
```

A container with no code and no editor ends at `code_off`. One with an editor ends
at `editor_off + editor_size`. Checked as an equality, not a range: the damage is
an overlap, and a region that could sit anywhere is a region that could sit on the
geometry and pass the CRC on the overlap. That is the same sentence the code
region's check already makes.

### 4.4 Firmware posture: verify, and do nothing else

The firmware's whole duty is three lines of intent:

1. Decode and CRC-verify the triple with the rest of the header.
2. **Never execute, parse, or interpret the bytes.** They are host-side content.
   The C reader gains no HTML, no JS, and no ZIP dependency.
3. **Never install it.** `install_decide` and the slot copy prefix stop at the end
   of the code region, so the slot stays exactly the size it is today and the
   `apps` partition keeps its current arithmetic.

This keeps D15 and D28 intact: the OS registers no behaviour vocabulary for the
region and prices only its length, and the firmware's resolver stays the format's
source of truth while the editor merely adheres to it. Refusals follow the
established shape, including the cap: an `editor_size` above 2 MiB is refused by
the writer before bytes exist and by the reader at the door, in one sentence on
both sides.

### 4.5 The three-way mirror

The rule in `orb_app_manifest.h:13-17` and `orbapp.py:69` applies unchanged:
**`main/app/orb_app_manifest.{h,c}`, `tools/orbapp.py`, and
`tools/host_orbapp_reader_test.py` move in one commit.** The sentinel suite
re-derives every cap and word position from the writer's own source text and fails
on disagreement. `host_orbapp_test.py` pins `ORB_ASSET_CLASS_ABI ==
ORBAPP_GEOM_ABI`, so the abi bump lands there in the same commit. `orbapp.py`'s
`write_container` gains an `editor=b""` argument beside `code=b""`, with the same
docstring posture: opaque bytes, length-priced, no interpretation.

**The hub is the fourth member, and it moved in with them.** D37's hub-side reader
(`tools/orb-manager/internal/orbnet/appcontainer.go`) restates the same derived word map,
the C4 ladder, the header checksum rule, the slug charset and the `app_id` cross-check,
because S4's Themes enumeration asks the container a question the dial cannot answer
before an install. Two gates hold the pair together:

* `appcontainer_test.go` parses `tests/fixtures/sentinel.orbapp` and `plain.orbapp` and
  agrees with `tests/fixtures/app-pair.json`, the sidecar `tools/make_app_fixture.py`
  wrote from `orbapp.read_container`'s own read-back — so the accept side is pinned
  against the authority's numbers, not against a hand-typed Go expectation, exactly as
  `themecontainer_test.go` does for `.orbtheme`.
* `tools/host_app_fixture_test.py` closes the triangle in the Python-only lane: it
  evaluates the C enum's derivations and the Go const block's derivations against the
  same sidecar, checks the editor triple sits inside `hdr_crc` on both sides, checks the
  two caps (`ORBAPP_GEOM_BYTES_MAX`, `ORBAPP_EDITOR_MAX`) equal the hub's, and re-bakes
  the fixtures to demand byte equality. `host_orbapp_reader_test.py` proves the dial's
  reader BEHAVES; nothing but this file proves its word map AGREES, and a renumber that
  moved each word together with its accessor would have shipped past that one.

`make_app_fixture.py --verify` re-bakes into a scratch directory and refuses to rewrite,
so a stale fixture cannot be made current by the check meant to catch it.

## 5. What the region contains, and how it reaches a browser

### 5.1 The region is a ZIP

Inside the region sits one **uncompressed (STORE) ZIP** with an `editor.json` and
the app's files. The STORE-only ZIP writer already exists in the composer
(`Watch Composer.dc.html:553-575`), and Python's `zipfile` reads it
(`docs/watch-composer-plan.md:53`). `editor.json` names:

```json
{ "editor": 1, "entry": "index.html", "runtime": ["react", "babel"], "baker": 3 }
```

`editor` is the region's own dialect, versioned independently of the container so a
(`editor.go:46` is the shipped name, and §8's landed text matches it; the earlier
`manifest.json` spelling in this section never reached the code.)
bridge change does not cost an `orbapp` ABI bump. `runtime` names which hub
resources the app needs, and the hub refuses to load an editor that asks for a
resource it does not serve. STORE, not DEFLATE: the firmware never reads the
region, but a seekable uncompressed archive lets the hub stream any one member
without unpacking the whole thing to a temp file.

### 5.2 The device route: read the region back out

Apps are card-bound, so the orb must answer for what its card holds. Today it
cannot: the route table (`orb_net.c:6804-6857`) has `sd/ls`, `sd/write`, `sd/rm`,
`sd/mkdir`, `sd/theme`, `sd/app`, `sd/bundle`, `sd/preview`, and `sd/delete`, but
**no verb that returns a card file's bytes**.

Do not add a general `sd/read`. It invites the traversal fight the file manager
already runs. Add one narrow verb:

```
GET /api/v1/apps/editor?slug=<slug>     -> region bytes, application/zip
```

One slug, resolved by walking the card under the existing `orb_files_src`
discipline. Read-only. No path arrives from the caller. The firmware locates
`<slug>.orbapp`, re-verifies the region's own CRC, and streams the region bytes. A
card with no such app, or an app with no region, answers with its own sentence
rather than an empty body. That is the refusal-sentence convention every other door uses.

**This route must land in `security.go`'s allowlist and the firmware's uri table in
the same commit.** `firmware_contract_test.go:70`
(`TestDeviceRouteAllowlistMatchesTheFirmwaresUriTable`) pins the two lists against
each other in both directions, and a one-sided commit fails it.

### 5.3 The hub serves it, same-origin

The hub proxies that region to the browser under `/editor/<ip>/<slug>/…`, serving
the ZIP's members with the entry document first. Three facts make this the right
shape rather than a convenience:

- **No CORS, ever.** The device sends no `Access-Control-Allow-*` header and the
  hub strips it on the way back (`proxy.go:182`, pinned by `proxy_test.go:213`),
  which is why D33 called the same-origin forwarder "not a convenience." An editor
  loaded from the device directly would be a dead frame.
- **No stale copy.** The editor always comes from the container actually installed
  on the board in front of the operator. A hub-side registry can drift from the
  card. This one cannot.
- **The caps already hold.** `proxyResponseCap` is 8 MiB (`security.go`), so a 2
  MiB region streams with 6x headroom and no Go assembly step.

The hub's vendor tree stays put and shared. `cmd/orbvendor` already pins React
18.3.1, ReactDOM, and Babel into `web/composer/vendor/` (5.3 MB), `cdnLocal`
rewrites the three CDN URLs, and `serveVendor` enforces subtree containment. Under
D37 that tree is the shared runtime every app's editor resolves against instead of
the clock's private copy. The same move applies to `orbtheme-baker.js`: it becomes
a hub-served FORMAT 3 writer that any editor imports, so no app duplicates the 205
KB format implementation. `make composer` and `TestEmbeddedComposerMatchesSource`
keep pinning the drift, and the D33 parity proof (`make_bundle_fixture.py` ↔
`orb_bundle3_hdr_parse` ↔ the JS writer) stays the single gate on format
correctness.

Keeping the writer hub-side is also what keeps D28 true. Every editor bakes through
one implementation of the format, and the firmware resolver remains the authority
that refuses the result at install.

## 6. The Themes face: one pane per theme-aware app

### 6.1 Enumerate from the ledger, not from the card's contents

`renderThemeTabs` (`app.js:2325`) derives sub-tabs from `themeRowsAll`, so an app
with no themes installed gets no pane. Invert the source: ask the ledger, filter by
the `themes` cap, and let the pane report an empty card itself.

`GET /api/theme/apps` returns the intersection, and the hub does the join:

```json
[{ "slug": "clock", "title": "Clock", "themeable": true,
   "has_editor": true, "editor_url": "/editor/192.168.2.34/clock/",
   "installed": 3, "active": "brass-dial" }]
```

`themeable` reads the `themes` cap off the `d.installed` row the Apps face already
fetches. `has_editor` reports whether that app's container carries a region. The
two facts stay separate on purpose: an app may store themes with no editor, which
is the HSB app today, and a future app may ship an editor for a card it has never
filled.

### 6.2 The pane

Each pane holds, in this order:

1. **Installed themes**: the existing `themeRow` list (preview, Put it on the
   dial, Delete), filtered to this slug. Unchanged machinery.
2. **Add a theme…**: the existing `/api/theme/upload` door, scoped to this app.
3. **Edit**: rendered only when `has_editor`. Opens the app's editor in the pane.
4. **Export / Import**: the bundle to disk and back, generalized per app.

An empty state names its app and says what it wants:
`Clock is theme-aware. The card holds no Clock theme yet. Edit one, or Add one.`
That sentence is the whole point of the inversion.

### 6.3 The pins this must not break

`ui_test.go:163` (`TestAppsFaceReadsTheLedgerAndThemesIsItsOwnFace`) mandates the
face reads `d.installed`, keeps the drop-zone tier, keeps `data-tab="themes"`, keeps
`id="mn-themes-tabs"`, stays lazy via `lazyTab("themes", themeOpen)`, and never
resurrects `themesVisible`. Every one survives. The test gains assertions on the
ledger-sourced chooser and the `has_editor` gate rather than losing one.
`TestThemeRowsAreBuiltWithoutMarkupInterpolation`,
`TestThemeCardAddressesThemesByID`, and `TestThemeCardSurfacesTheOrbsOwnRefusals`
also hold unchanged, including the rule that theme rows are built with DOM APIs
because a theme name arrives from a file someone else wrote.

## 7. The editor contract

### 7.1 The bridge

An editor is a self-contained document in a sandboxed iframe. It dials the hub and
never the device. The postMessage vocabulary is deliberately tiny and frozen:

| Direction | Message | Payload |
|---|---|---|
| editor → shell | `theme.export` | bundle bytes |
| editor → shell | `theme.install` | bundle bytes |
| editor → shell | `theme.preview` | RGB565 plane + extent |
| editor → shell | `theme.error` | one honest sentence |
| shell → editor | `theme.state` | `{ active, rows }` |
| shell → editor | `theme.installResult` | `{ ok, store, note }` |

The app owns its internal model completely. Six preset dials, uploadable sprites,
painted backgrounds, animation timelines: none of it concerns the shell. The one
obligation is emitting a valid FORMAT 3 section for its own slug and handing the
bytes to the existing `/api/theme/upload` → `/api/theme/install` →
`/api/theme/commit` flow, which is already app-agnostic and already enforces
D26/D28 verification at the firmware boundary. **Verification stays at the door,
not in the editor's good intentions.** That one rule is what lets a pick-one-of-six
app and a full sprite-painting app share a single mechanism.

`serveComposer`'s existing `setLaunchToken` call already proves an embedded frame
authenticates against `/api/theme/upload` on the `orb_token` cookie, including the
deep-linked case that previously 403'd (§31.3). The bridge inherits that rather than
reinventing it.

### 7.2 Why this generalizes cleanly

The FORMAT 3 container already carries N per-app sections, and the baker already
takes a section-level model. Generalizing means lifting `bakeClock3` to
`bakeSection3(slug, model)`, with the clock's element dialect behind a per-slug
builder and each app's editor owning its own model-to-section projection. The
clock's projection stays where it is. A preset-picker app's projection is two
hundred lines and no art encoding at all.

## 8. Deleting the pre-registration

Rule 1 kills the seed. D36 already moved the files out of the image, so this is a
hub-side deletion rather than a firmware change.

Removed: `seed.go`, `POST /api/orb/seed`, `seedApps`, `seedThemeFile`, `seedMinFW`,
`seedRun`/`seedNeeded`/`waitForStore`, the `//go:embed seed` tree, the join-time
auto-seed at `app.js:1514`, and `tabs.js::appFaceSeed`. Removed with them:
`seed_test.go`'s five cases, `TestEmbeddedSeedMatchesDisk`, and
`TestSeedFilesAreFresh`.

Also inverted: `ui_test.go:163` currently **demands** the seed row
(`function appFaceSeed(body) {`) and `/api/orb/seed` in `app.js`. Those assertions
flip to assertions on the empty state.
`tools/host_orbapp_install_test.py` likewise currently asserts the seed contract's
presence. It asserts the drop-zone path instead.
`tools/make_factory_apps.py` and `tools/make_seed_theme.py` stay, because they are
how the clock's and HSB's own `.orbapp` files get built and committed as **artifacts
the operator copies to a card**. The generators stay. The hub's private copy of
their output goes.

What replaces it is the honest path the Apps face already implements: card in, files
copied to `/sd/apps`, and the drop-zone Install button that `appFaceCardZone`
(`tabs.js:498`) already renders with each refused row's own sentence. A fresh orb
boots to the shell menu and waits for its owner, which is exactly the state §40 says
D30 built for.

One consequence worth stating plainly. Rule 2 plus rule 1 means a fresh board shows
no dial face until the operator copies files to a card. The base face and the shell
menu still render, so the board is usable and clearly waiting rather than broken.
The hub may offer a "copy the starter files to this card" button sourced from
**files the operator points at**, never from bytes embedded in the binary. That keeps
one path for the starter pair and for every third-party file, which is D36's own
stated point.

## 9. Slices, in the order that de-risks them

**S1 — Themes face enumerates from the ledger.** *Status: DONE.* New `GET /api/theme/apps`, the
chooser-source inversion, the empty state, and the `has_editor` gate. The clock
opens the existing composer through a temporary hub-side alias, so the tab works
before the container change lands. No firmware and no format change. This slice
alone fixes the placement complaint.

Implemented in `internal/orbnet/themeapps.go` (`ThemeAppsFor` composes the device's
`GET /api/v1/apps` caps with its `GET /api/v1/sd` rows), registered in `main.go`
beside `/api/theme/list`, consumed by `loadThemeApps`/`themeEmptyPane` in
`web/app.js`. `editorAvailability` in that module is the marked stopgap S3 deletes.
Pinned by `themeapps_test.go` and by new `ui_test.go` assertions that fail if
`renderThemeTabs` goes back to deriving apps from `themeRowsAll`.

**S2 — The editor region.** *Status: DONE.* Header growth, `GEOM_ABI` 3, the derived placement, the
2 MiB cap, the verify-but-never-install posture, `ORB_APP_UPLOAD_MAX` at
2,494,464 B, and `GET /api/v1/apps/editor` in the firmware table and `security.go`
in the same commit. `write_container` gains `editor=`. The mirror and the contract
test both move.

Landed: `orb_app_manifest.{h,c}` carry the triple and refuse a region past the
cap; `orb_app_install.{h,c}` verify the region's CRC over the card and never copy
it; `orb_net.c` registers `h_app_editor` ahead of `h_apps_list`; `security.go`
allows the GET in the same commit as the firmware table row; `orbfake` answers it,
and its cap-bit table was corrected to `orb_cap_t`'s real 4..7 range on the way.
`orbapp.py` stamps ABI 3 and refuses an over-cap region before a byte is written.
The `_Static_assert` in `orb_assets.c` forced `ORB_ASSET_CLASS_ABI` to move with
`ORBAPP_GEOM_ABI`, and the two factory containers were re-baked for the new dialect.

**S3 — The hub serves editors from the card.** *Status: DONE.* `/editor/<ip>/<slug>/…`, the ZIP
unpack, the shared runtime and baker as hub resources, and the iframe bridge.
`TestComposerEntryCarriesLaunchToken` extends to pin the token onto every editor
entry document and off its sibling assets.

Landed as `editor.go`/`editor_test.go`: `serveEditor` gates on the same
learned-address check every theme dial passes, unpacks the region in memory
(nothing is written to disk), resolves the entry document through the app's own
`editor.json` with an `index.html` fallback, and refuses a member past
`editorMaxEntry` before allocating on a declared size. The token goes on the entry
document and on nothing else — `TestEditorKeepsTokenOffSiblingAssets` is the pin.
`orbnet.EditorRegion` is the dial; `Server.fetchEditor` is the seam that lets it be
tested without a board. The web half is a pane in the Themes card plus
`themeBridge` in `app.js`, which checks `event.source === frame.contentWindow` and
same-origin before it spends a token, and posts the container to the existing
`/api/theme/upload` door.

**S4 — The clock editor moves into its container.** *Status: NOT DONE — held, owner's call 2026-10-27. Sized in §10 item 3 and it fits with ~1.9 MB to spare, so it is work, not risk.* This is where 652 KB of
app-owned files meet the cap and where the shared-runtime split stops being a
proposal. Split `Watch Composer.dc.html` into the hub-served runtime and baker and
the app-owned UI, then re-bake `clock.orbapp` with the region. The standalone
`file://` composer keeps working as it does today.

**S5 — Delete the seed.** *Status: NOT DONE — deliberately sequenced after S4. Deleting it before the clock's own editor ships in its container leaves a factory-fresh board with no clock app and no face until someone copies files to a card.* Once the drop-zone path is proven, remove the hub's
embedded pair and invert the two test assertions.

**S6 — A second app.** Any theme-aware app adds a region plus a `theme.*` bridge
handler and triggers no firmware, format, or route change. That is the test that the
extension point is real rather than clock-shaped.

## 10. Invariants and open items

**Invariants, each already pinned by a suite.** The three-way mirror moves in one
commit. Region offsets stay inside `hdr_crc`. The editor region never enters the
`apps` partition or the slot. The device route allowlist equals the firmware's uri
table. Baking stays in the browser (D33). The firmware resolver stays the format's
authority (D28). Verification of a bundle stays at the install door (D26). The
Themes face stays its own lazy per-app face. Theme rows stay built with DOM APIs.

**Open, and not decided here.**

1. **Sandboxing depth.** Is `sandbox="allow-scripts"` enough, or does an editor that
   reads local sprite files need `allow-same-origin` too? Either answer widens what a
   container's HTML may reach on the hub, and that is the security question this
   design leaves open. It wants a ruling before S3.
2. **Signed containers.** Nothing authenticates an `.orbapp` today, and a region
   changes nothing about that. A hub that serves arbitrary HTML out of a card file
   raises the stakes on where those files came from.
3. **The 2 MiB cap against a real clock build. RESOLVED — it is not close.**
   Measured on the working tree 2026-10-27, deflating each file into one archive the
   way the region stores them: the whole composer is 926,919 B raw and 283,255 B
   zipped; the app-owned half after the split (`Watch Composer.dc.html`,
   `watch-engine.js`, `dial-renderer.js`) is 652,355 B raw and **198,560 B zipped**,
   which is 9.5% of the cap; what goes hub-side (`orbtheme-baker.js`, `support.js`)
   is 274,564 B raw and 84,717 B zipped and stops being the app's bytes at all. The
   cap has ~1.9 MB of headroom over the clock's own UI, so S4 carries no sizing risk
   and the split is worth doing for the byte count it saves rather than because the
   cap forces it. Re-measure only if the app-owned files grow tenfold.
4. **Editing for a disabled app.** The Themes face lists enabled apps, so an app with
   an editor that is not enabled has no pane. Whether it should get one for
   authoring is open.

## 11. The one sentence to remember

The `.orbapp` carries an app's editor as an optional CRC-covered region the firmware
verifies, never installs, and never runs. The hub reads it back off the card over
`GET /api/v1/apps/editor?slug=`, serves it same-origin beside a shared baker and
runtime, and the Themes face lists every enabled `themes`-cap app from the ledger
with an Edit button only where a region exists. If a session is told to "move the
theme editor into the Themes tab," it is implementing this.

