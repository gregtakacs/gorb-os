# orb-manager: hardening and capability plan

**Status: APPROVED FOR EXECUTION — owner chose the full run, iterations 0-6,
one commit each, 2026-10-12.** Written at HEAD `476b13c` on the clean tree.
Self-contained: pick it up cold. Each iteration below is one commit and ships
with its tests. The iteration number in a commit message names the section here.

Repo: `/home/greg/docker/Projects/gorb-os`. The tool is `tools/orb-manager/`, a
Go package with its own `Makefile`. Go is at `/usr/local/go/bin/go` and is NOT
on the default `PATH` of this shell — the `Makefile` probes for it, bare `go`
does not work. Read `tools/orb-manager/README.md` before changing behaviour: it
is the tool's contract, and several tests exist to pin what it promises.

## 0. Verification ladder — run this after EVERY iteration commit

```
cd tools/orb-manager
/usr/local/go/bin/go test ./...      # or: make test
make vet
make crosscheck                      # type-checks every GOOS/GOARCH incl. tests
ORB_BUILD_DIR=/home/greg/docker/Projects/gorb-os/build \
  /usr/local/go/bin/go test ./internal/firmware/ -run BuiltInBundle
make win-test                        # Windows paths only; skip if WSL interop is down
```

Baseline green at `476b13c`: `go test ./...` exits 0 across all six packages,
107 test functions. Two facts about that baseline are load-bearing:

- The suite passes WITHOUT the bundle staleness guard, because that test skips
  unless `ORB_BUILD_DIR` is set (`internal/firmware/firmware_test.go:328-331`).
  Only `make firmware` ever sets it. Iteration 0 closes this hole.
- Eight Windows-targeted tests skip when run through WSL interop because they
  drive a `#!/bin/sh` stub browser `CreateProcess` cannot exec. A skip there is
  expected, not a failure.

## 0.1 Ground rules for the whole run

- Touch only `tools/orb-manager/`, plus the two places iteration 5 names.
- Never flash the orb as a side effect of a code change. Hardware steps are
  listed per iteration and need the orb-B cable per `.orb-target`.
- A hardware claim requires a hardware run. A code-and-compile-check change is
  reported as such, never as verified.
- Long commands run detached and polled, per `.clinerules`. Each `run_commands`
  element is its own shell: give every element its own `cd` or an absolute path.

## 1. Verified defects at HEAD (the backlog this plan clears)

Each row was read out of the file or run on the machine, not inferred.

| # | Defect | Where |
|---|---|---|
| D1 | The committed bundle is stale right now. Its app bin is stamped `9e79cca`, HEAD is `476b13c`; all four other images are byte-identical, so it is one rebuild behind and the suite does not notice | `internal/firmware/bundle/orb-firmware.zip`, guard at `firmware_test.go:328` |
| D2 | No cancellation anywhere. `internal/esprom` uses absolute deadlines and no `context.Context` appears in the package. `/api/flash`, `/api/provision`, `/api/watch` all ignore `r.Context()` | `internal/esprom/commands.go:81-135`, `flash.go`, `watch.go`, `provision.go` |
| D3 | Closing the window can kill a flash mid-write. Shutdown fires on an unconditional flag read with a 3 s budget, and there is no `os/signal` handler at all | `main.go:164-171` |
| D4 | No origin or CSRF check on any route. `grep` for `Origin`/`Sec-Fetch`/token across the package returns nothing. `-addr` accepts a non-loopback host silently | whole package, `main.go:117` |
| D5 | The proxy forwards to any IPv4 the browser names, including `10.x`/`192.168.x`/`169.254.x`, with no learned-IP restriction and no method or route allowlist | `proxy.go:13-53` |
| D6 | An uploaded bundle's `esp_app_desc_t` strings reach `innerHTML` unescaped. `who` is built from `b.project`/`b.version` and interpolated raw while sibling fields use `esc()` | `web/app.js:151-155` |
| D7 | Memory is bounded per zip entry, not in total. Each entry gets a 32 MB `LimitReader`; `Load` sums them into a map with no aggregate ceiling | `internal/firmware/firmware.go:99-116,162-169` |
| D8 | Identity is keyed by tty path only, so a different orb re-plugged into `/dev/ttyACM0` inherits the first orb's cached version. `s.lastBoot` is a single slot, so flashing two orbs in a row loses the first verdict | `identity.go:19-25`, `main.go:73,93-95` |
| D9 | macOS cannot identify an orb: `classifyPort` returns empty, and the probe refuses unattributed ports by design. There is no darwin build target and no release target | `internal/esprom/usbclass_other.go:9`, `Makefile` |
| D10 | `flash.go:29` promises a "stamp audit (unless overridden)" that no longer exists in the hub path | `flash.go:29-30` |
| D11 | `bin/flashlore` is a stale untracked local artifact not produced by any target | `bin/` |
| D12 | No `.github/`, so `crosscheck` and `win-test` run only when a human remembers | repo root |
| D13 | `proxy_test.go` has two shape tests and no test that a good forward actually forwards | `proxy_test.go` |

## 2. Iterations

### Iteration 0 — trust the artifact

Owner-visible failure: D1. The binary an end user downloads can flash a
firmware one commit older than the tree it claims to ship, and `make test`
never notices.

1. Re-pack the committed bundle from `build/`, whose app bin carries the
   correct `476b13c` stamp. Adopt it only because the descriptor matches HEAD
   and the other four images are already byte-identical.
2. Close the skip hole in D1: make `make test` export `ORB_BUILD_DIR` whenever
   `build/flasher_args.json` exists, so the guard runs by default on a machine
   that has a build dir, and still skips cleanly on a machine that has none.
3. Add `make check`: test, vet, crosscheck, and the bundle guard in one command.
4. Delete the dead stamp-audit promise (D10).
5. Remove the stale `bin/flashlore` (D11). It is build output; `bin/` is gitignored.

Commit: `fix(tools): make the bundled firmware prove it matches the build`

### Iteration 1 — never corrupt a flash

Owner-visible failures: D2, D3.

1. Put a `context.Context` on `esprom.Options` and honour it in the `Device`
   read loop and between image writes. `ctx.Err()` surfaces as a distinct
   "cancelled" error so the UI says cancelled, not timeout.
2. Pass `r.Context()` from `handleFlash`, `handleProvision`, `handleWatch`.
3. Replace the process-wide `flashing` bool with a per-port operation registry:
   port, kind, started, deadline. One flash per port, and any port may be busy
   while another is free. Add `POST /api/flash/cancel {port}` and show the live
   operation in the UI.
4. Defer window-close shutdown while an operation is live, and add `os/signal`
   handling with the same guard, so Ctrl-C cannot drop the flock mid-write.

Tests: a cancelled request stops the write loop; shutdown defers while an
operation is live; two ports flash concurrently; the same port refuses.

Commit: `fix(orb-manager): make a flash cancellable and shutdown-safe`

### Iteration 2 — lock the local HTTP surface

Owner-visible failures: D4, D5, D13.

1. Mint one per-launch token. Inject it into the served `index.html`, require it
   in `X-Orb-Token` plus a same-origin `Origin`/`Sec-Fetch-Site` on every
   mutating route. Browser fetch adds the header from the injected value; a
   cross-site page cannot read it.
2. Refuse a non-loopback `-addr` unless `-allow-remote` is passed. Log the
   exposure line on every bind.
3. Restrict `/orb/` to addresses the hub learned over USB or the operator
   explicitly loaded, to `GET`/`POST`, and to a route allowlist mirroring the
   handlers in `main/net/orb_net.c`. Cap the response size, and stop swallowing
   the body read error at `proxy.go:49-51`.

Tests: cross-origin POST is refused; wrong token is refused; an unlearned IP is
refused; a disallowed device route is refused; a good forward returns the device
body (closes D13).

Commit: `fix(orb-manager): require same-origin, tokened control requests`

### Iteration 3 — correct identity state

Owner-visible failure: D8.

1. Key the identity cache on `(port, usbMac)`. The USB serial number is the chip
   MAC and survives tty renumbering. Evict entries whose port is absent from the
   last enumeration, and add a TTL.
2. Replace the single `s.lastBoot` slot with a MAC-keyed map, reusing the
   existing 90 s freshness rule.

Tests: re-plug a second orb on the same tty and the stale version is gone; two
flashes in a row keep both verdicts.

Commit: `fix(orb-manager): key port identity by cable, not by tty name`

### Iteration 4 — trust a picked bundle

Owner-visible failures: D6, D7.

1. Escape `who` at `web/app.js:153` and audit every server string that reaches
   `innerHTML`. Extend the client/handler contract tests to pin the escaping
   rule, so a future field cannot be added raw by accident.
2. Bound the decompressed total and the entry count in `firmware.Load`, not just
   each entry.
3. Refuse a bundle whose descriptor project is not `gorb-os`, so a stray ESP-IDF
   zip cannot become the active firmware.

Tests: a crafted descriptor with markup cannot escape the panel; a zip bomb is
refused; a foreign project is refused.

Commit: `fix(orb-manager): validate bundles and escape firmware strings`

### Iteration 5 — make Manage a real manager

Owner-visible gap: the Manage card is a status panel with four buttons. Theme
management lives only on the device page.

1. Add a Theme card: list, install, restore default, delete, driven through the
   proxy against the device API that exists TODAY (`GET /api/v1/sd`,
   `POST /api/v1/sd/install?name=`, `POST /api/v1/sd/default`). Isolate the
   device-theme calls behind one small client module in `internal/orbnet`, so
   the switch to the `?id=` form that `docs/theme-identity-plan.md` mandates is a
   one-file change when that plan lands. This plan does not pre-empt it and does
   not rename anything the firmware still calls a slug.

   (*As written at iteration 5. The FORMAT 2 plan has since landed: the device takes
   `?id=` only, the slug fields are gone, and the switch this item deferred is done —
   see "Closed 2026-10-12" below. The routes above are what the card was built against,
   not what an orb answers now.)*
2. Add theme upload through the same module.
3. Persist the last-used orb IP across restarts, and poll status with backoff.
4. Add a read-only USB console card, replacing `tools/cap.py` for an end user who
   has no Python and no clone.

Hardware step, needs the cable: one live run of list, install, and delete on
orb-B. Reported as hardware-verified only if run.

Commit: `feat(orb-manager): manage themes and read the orb console from the hub`

### Iteration 6 — offline composer and distribution

Owner-visible failures: the CDN dependency in README `Caveats`, D9, and the
absence of any release artifact.

1. ✅ Serve the Theme Editor with no internet. `cmd/orbvendor` downloads the three
   `unpkg.com` scripts and the `latin`/`latin-ext` woff2 subsets the eleven
   `fonts.googleapis.com` links resolve to (160 files reduced to 93, ~4.5 MB), and
   records every URL with its sha256, size and the SRI into
   `web/composer/vendor/MANIFEST.json`. `composer.go` then rewrites the entry page at
   serve time: CDN URLs → `/composer/vendor/…`, eleven font links → one
   `fonts.css`, and a `window.__resources` map injected immediately before the
   `support.js` tag. `support.js` itself is never edited — that map is the override
   hook it was written with, and `TestEmbeddedComposerMatchesSource` plus
   `TestComposerServesSiblingsUntouched` still pin it byte-exact (the first attempt
   rewrote it and both tests failed, which is the proof they work). The rewrite is
   gated on `vendorReady()`: a partial tree stays on the CDN rather than mapping to
   404s. `make vendor-check` re-verifies the committed tree and runs in `make check`.
   Two ordering bugs were found and pinned by test: the URL substitution must run
   BEFORE the map is inserted (after, it rewrote the map's own keys and the editor
   silently stayed online-only), and `serveVendor` needs a separator between the
   embed dir and the vendor subtree (`…/vendor` + `react…` 404'd every asset).
   `TestComposerHasNoUnknownExternalDependencies` pins the whole external surface —
   three unpkg URLs plus Google Fonts — so a re-vendored `support.js` that adds a
   fourth fails the build instead of half-loading at a bench.
2. Add `make release`: darwin/amd64, darwin/arm64, linux/amd64, windows/amd64,
   plus `sha256sums.txt`. DONE. `bin/release/<name>-<os>-<arch>/` holds both
   binaries per platform, and the sums are recorded relative to that dir so
   `sha256sum -c` passes from inside it. Verified here: `file` confirms Mach-O
   x86_64, Mach-O arm64, static ELF and PE32+; appending a byte to one `orbflash`
   turned exactly one line to FAILED and restoring it returned all eight to OK.
   `crosscheck` now defaults to the same platform list, so nothing ships that has
   not been type-checked — darwin/amd64 had been slipping through.
3. Implement `usbclass_darwin.go` via `ioreg` so macOS can attribute `303a:1001`
   and Identify stops refusing. DONE, with the limitation stated rather than hidden:
   `CGO_ENABLED=0` rules out calling IOKit the way pyserial does, so it asks
   `ioreg -p IOUSB -l -x` for XML and hands the bytes to `usbClassFromPlist`, which
   lives in the portable file and is unit-tested on Linux — three deliberate
   breakages of that parser each turned a test red. No Mac was available to capture a
   real dump, so the fixtures are hand-authored to Apple's plist DTD using the
   property names IOKit is documented to expose. Every failure path returns the zero
   `USBClass`, so the worst a wrong guess can cost is the refusal message the hub
   already showed, never a DTR/RTS edge on a bench device. Still owed: one `ioreg`
   capture beside a real orb, confirming `"attributed": true` in `GET /api/ports`.

The browser-side confirmation is still owed: open `/composer/` with DevTools' network
panel and an airgapped interface, and confirm zero requests leave the origin. No
Chromium or Firefox exists on this dev box, so the served bytes were checked
statically instead — the entry document contains no `http(s)://` reference outside the
override map's own keys, and every vended asset, font included, returns 200 with the
right content type.

Commit: `feat(orb-manager): serve the composer offline and ship a release set`

### Iteration 7 — CI, DEFERRED pending owner sign-off

`.github/` does not exist in this repo and adding one is a new top-level
convention, so it stays out of this run. A workflow running vet, `go test ./...`
and `make crosscheck` on push is the intended content.

### Closed 2026-10-12 — the hub spoke FORMAT 1 while the device spoke FORMAT 2

Filed 2026-10-12 and closed the same day. The two clones merged in `448a703`, which is
what turned every item below from a forecast into a present defect in one tree; the
follow-up commit on the merged tree is what closed it.

The firmware, `tools/orbtheme.py` and `tools/watch-composer/` are FORMAT 2. Each row was
read off the merged file on 2026-10-12:

| Fact | Where |
|---|---|
| `FORMAT = 2`, `NAME_MAX = 56` (55 usable bytes + NUL) | `tools/orbtheme.py:52,56` |
| the baker packs one `name`, not the `slug`+`title` pair | `tools/watch-composer/orbtheme-baker.js:28` |
| the copy the hub embeds and serves is that same FORMAT 2 baker | `tools/orb-manager/web/composer/orbtheme-baker.js:28` |
| install and delete send `?id=`, the handle the firmware reads | `internal/orbnet/themes.go` `ThemeInstall`, `ThemeDelete` |
| `ThemeEntry` carries `ID` and `Changed`, and no `Title` | `internal/orbnet/themes.go` |
| `CheckThemeName` allows free-text UTF-8 to 55 bytes; `CheckThemeID` is the 12-hex gate | `internal/orbnet/themes.go` |
| an upload is filed by the name inside the container; the hub reads it with `ContainerName` and refuses FORMAT 1 before dialling | `internal/orbnet/themecontainer.go` |

The device answers the other way. `h_theme_install` and `h_theme_del` both refuse unless
`?id=` is exactly 12 chars of `[0-9a-f]` (`main/net/orb_net.c:2719,2774`, `theme_id_ok` at
`main/art/orb_theme.c:356-366`). `h_sd` emits `{name,id,why,bytes,serial,ok,active,changed}`
(`main/net/orb_net.c:2681`) — no `title` — and `try_slug`/`damage_slug` became
`try_id`/`damage_id` (`:2662`). Upload takes `?overwrite=1` and reads the name from the
container header (`:2825,2829`). The id is `sha256(name)[:12]` lowercase hex
(`main/art/orb_theme.h:53`, mirrored by `theme_id()` at `tools/orbtheme.py:340-356`).

Three breaks were live on the merged tree, not hypothetical. All three are closed on the
hub side; none of them could be closed by a green `make check`, which is the real finding.

1. Install and delete sent `?name=` to routes that read `?id=`. Both answered 400
   `"id is missing or not a theme id"`. The card's buttons were dead with no local error.
2. `ThemeUpload` sent `?name=`. The firmware reads the name from the container header
   instead, so that parameter was inert, and the hub never sent the `?overwrite=1` the
   device offers.
3. `CheckThemeName` refused every valid FORMAT 2 name. Names are free-text UTF-8 up to
   55 bytes. The regex was ASCII-only and capped at 31 characters, so it rejected
   `Ünit ☃` and `Mirage — Gold` — names the baker had already accepted and baked.

The fourth break from the original list — the hub serving a FORMAT 1 baker — closed with
the merge. Both bakers report `FORMAT = 2` and `make vendor-composer` verifies.

The worst part was that the suite could not see any of this. The client, its unit test and
the fake device all encoded the FORMAT 1 contract together, so they agreed with each other
and disagreed with the firmware: `themes_test.go` asserted `?name=` on install, delete and
upload, and `cmd/orbfake` answered `?name=`. `make check` was green while a real orb 400s
every install.

That is fixed structurally, not by editing assertions. The device double moved out of
`cmd/orbfake` into `internal/orbfake/`, where the suite and the live binary run one copy,
and it was rebuilt from `orb_net.c`: it refuses any name-addressed verb with the
firmware's own 400, holds its own copy of the 12-hex rule and the container read, and
files uploads by the embedded name. `internal/orbfake/orbfake_test.go` pins those
refusals, so the double cannot quietly agree with the client again.
`themes_contract_test.go` then drives the real client against it, taking ids off the
list the device returned rather than typing them. Seven mutations of the client — revert
`?id=` to `?name=`, drop the container gate, loosen the id regex, count the name cap in
characters, hash the padded field, drop `changed`, revert `try_id` to `try_slug` — each
one turns that suite red.

One naming trap deserved its own paragraph, and is now closed in the code. The comment at
`themes.go` read the plan's `id` as an orb id, "so one hub can address more than the orb
it last happened to learn". The `id` that landed is a theme id: `sha256(name)` truncated
to 12 hex. Same token, two meanings. The module comment now says so out loud, because the
orb's own device identity is a third thing and will need its own parameter.

The merge already settled what the editor ships. `support.js`, `dial-renderer.js` and
`watch-engine.js` were byte-identical between the two clones, so iteration 6's offline
work and its SRI/manifest pins came through untouched — `make vendor-composer` verifies
against the merged `tools/watch-composer/` today. Only `orbtheme-baker.js` and
`Watch Composer.dc.html` moved.

Not verified, and no one should claim it: the hub's theme card has never driven a real
device. What now stands behind that claim is a double built from `orb_net.c`, plus the
host suite `tools/host_sd_test.py`, which pins the firmware's `theme_id()` against
`tools/orbtheme.py` on the same vectors `internal/orbnet/themes_test.go` pins the Go copy
against — so C, Python and Go agree on the hash, and only the physical orb remains
unproven.

## 3. Deliberately NOT changed

- The safety model stays "you own the cable". No `.orb-target` binding and no
  MAC gate return, per README "Safety model". Iteration 2 restricts who may ask,
  not which orb you may act on.
- No subnet sweep, no ARP. Every network call stays a dial to one address the
  hub learned or was handed. mDNS discovery, banned here since the sweep was
  deleted, is now in scope as the replacement for handing the hub an address;
  see §5. The sweep itself stays banned. Discovery is solicited from the orb,
  never extracted from the subnet.
- The ROM-only flasher path stays. No stub loader returns.
- The hub always writes the full manifest. App-only reflash stays in `orbflash`.
- `docs/theme-identity-plan.md` and `docs/app-platform-plan.md` keep their own
  sequences. This plan does not implement their firmware sides.

## 4. State at hand-off

Updated at each commit.

- `476b13c` baseline: clean tree, `go test ./...` green, bundle one rebuild stale
  (D1), no cable run.
- Iteration 0 `344afd8`: bundle re-packed from `build/` and stamped `476b13c`;
  guard armed in `make test`; `make check` added; dead stamp-audit comment and the
  stale `bin/flashlore` gone. Guard proven: one changed byte in the build dir fails
  the suite.
- Iteration 1: `context.Context` through `esprom` (read loop, command, block loop,
  session, boot read, `ProbeOrb`, `ProvisionOn`); per-port `opRegistry` replaces the
  process-wide `flashing` bool and `Server.mu`; `/api/ops` + `/api/flash/cancel`;
  guarded quit and two-stage Ctrl-C; Cancel button and live-op line in the card.
  `make check` and `make win-test` green. Both new guards proven by mutation: with
  the block-boundary and `command()` checks removed the wire takes 3 blocks instead
  of 2, and with `quit`'s wait removed the listener is released during a flash.
- Iteration 2: per-launch token (`X-Orb-Token` header + HttpOnly SameSite=Strict
  cookie) and a same-origin/`Sec-Fetch-Site` check on every mutating route including
  the whole `/orb/` proxy; `-addr` refuses a non-loopback host without
  `-allow-remote`; the proxy now requires a LEARNED IP literal, a device-route and
  method allowlist mirroring `orb_net.c`/`theme_app.c`, hub-owned dial port, and
  byte caps; the swallowed proxy read error is reported. All four gates verified
  live against a running hub and by mutation: without the learned-IP gate the hub
  dials `127.0.0.1:80` and returns its 404, and with the token or origin check
  disabled the cross-site cases go 200. The token injection bug (identifier eaten
  by `ReplaceAll`) was caught here and its test hardened to pin the property name
  `app.js` reads.
- Iteration 3: the identity cache is keyed on the cable, not the tty name — an
  answer records the USB serial behind the port and is refused once a different
  serial is in the socket, expires after `identityTTL`, and is dropped when its port
  leaves the enumeration. `lastBoot` became `boots`, a MAC-keyed map with the same
  90 s rule, pruned on write. Re-plug and two-orb cases covered; both verified by
  mutation (ignoring the serial comparison, and making `bootFor` answer any MAC, each
  fail their test).
- Iteration 4: `firmware.Load` caps the DECOMPRESSED aggregate (48 MB) and the member
  count (32), not just each entry — per-entry caps summed to entries x limit, which is
  how a small archive could make the hub hold gigabytes. A bundle whose
  `esp_app_desc_t` announces another project is refused; an image with no descriptor
  is still accepted and shown as unknown. Descriptor fields drop non-printables where
  parsed, and `refreshBuild` now escapes the headline built from project/version
  (it was interpolated raw while its neighbours were escaped), pinned by a scan that
  fails on any unescaped server interpolation in that template. Confirmed by
  mutation: restoring the raw `${b.version}` fails the new test.
- Iteration 5: the Manage card became a manager. Theme card (list, install,
  restore built-in, re-read card, delete behind a confirm, `theme.bin` upload that
  never touches the hub's disk), all device theme calls isolated in
  `internal/orbnet/themes.go` so the future orb-`id` has one home; read-only USB
  console registered as a port operation; learned addresses remembered in
  `state.json` (0600, addresses only) and restored at startup; status poll that
  backs off to 4 min on misses and resets on a hit. Two dial-port owners were
  caught disagreeing and collapsed into `orbnet.Client.DevicePort`, with an
  `-orb-port` bench override and `cmd/orbfake` to drive it.
  Verified live end-to-end against the real hub binary plus the fake orb: refused
  before `allow`, list/install/delete/upload after, the firmware's own refusal text
  preserved, traversal names refused pre-dial, `/api/v1/ota` still refused through
  the proxy, and a fresh process dialling the remembered address with no typing.
  The client has no JS engine here: the balance/selector checks are documented as a
  smoke check in the README, not a parser.
- Iteration 6 (offline composer): `cmd/orbvendor` vendors the three `unpkg.com`
  scripts and 93 `latin`/`latin-ext` woff2 files into `web/composer/vendor/` with a
  sha256/SRI `MANIFEST.json`; `composer.go` serves that tree at
  `/composer/vendor/` and rewrites the entry document to it at serve time, using
  `support.js`'s own `window.__resources` hook rather than editing vendor bytes.
  `make vendor-composer` produces the tree, `make vendor-check` verifies it and is
  now part of `make check`. Gated on `vendorReady()`, so a build without the tree
  behaves exactly as it did before. Verified by serving both builds from the real hub
  binary and diffing the entry document, and by mutation: flipping the inject order
  or tampering with `babel.min.js` each turn a test red.
- Iteration 6 (release + darwin): `make release` cross-compiles both binaries for
  four platforms into `bin/release/` with a `sha256sums.txt` that was checked by
  tampering, not just by reading it back. `usbclass_darwin.go` attributes a macOS
  port by parsing `ioreg -x` output; the parser is portable and mutation-tested on
  Linux, the positive branch still needs one capture from a real Mac.
- Hardware: nothing in iterations 0-5 was run against a cable. The cancel path and
  the console read both drive real ROM/CDC timing and need one live run on orb-B
  before either is called verified. The theme card was verified against the device
  double in `internal/orbfake`, which is written from `main/net/orb_net.c` and holds
  its own copy of the id rule — a faithful stand-in, but not the firmware.
- Theme identity: closed 2026-10-12 in `feat(orb-manager): speak theme ids`. The hub
  spoke FORMAT 1 (`?name=`, `Title`, `try_slug`, a 31-char ASCII name rule) while the
  firmware in the same tree, merged in `448a703`, required FORMAT 2 — so install and
  delete were dead against a real orb and nothing in `make check` noticed. The client
  now addresses by `?id=`, `ThemeEntry` carries `ID`/`Changed`, the name rule matches
  the header's 55 UTF-8 bytes, upload reads the name out of the container and refuses
  FORMAT 1 before dialling, and `?overwrite=1` is wired to the firmware's 409. The
  device double moved to `internal/orbfake/` and was rewritten from `orb_net.c`, so
  client, test and double no longer share one wrong assumption; seven mutations of the
  client each turn the contract suite red. `docs/theme-identity-plan.md` §8 never named
  `tools/orb-manager/` as a consumer, which is how the gap survived the merge.



## 5. Fleet UI redesign: Onboard merged into Manage (2026-10-12)

Onboard and Manage are one surface. Manage lists named orbs; Onboard's
serial flow becomes "Add an orb" inside it. Discovery moves from the deleted
/24 sweep to mDNS: the firmware advertises `_orb._tcp` and the hub browses.
The owner types nothing.

### 5.1 Decisions taken

1. **Menu-as-settings.** The web settings page renders the device's own option
   registry over `GET /api/v1/menu` + `POST /api/v1/menu`. The `opt_*` ABI is
   already data-driven and accepted (`main/app/orb_app.h:104-118`, rulings
   D11-D13, executed as §13 of `docs/app-platform-plan.md`); `menu_view.c`
   walks it generically and §13.4 forbids an app keeping a second opinion
   about what is on the page. New apps and new rows therefore appear in the
   hub with no hub change. The POST path must expose arm/disarm, because
   ruling D11 made a pick install-and-reboot by itself and an unguarded POST
   would reboot a wall clock with one click.
2. **Factory reset, full stop.** Implemented as an armed `opt_*` menu row and
   mirrored over the serial path. Erases `ssid`/`pass`/`tz` from
   `storage`/`orb.net` and reboots into the provisioning SoftAP. Sends an mDNS
   Goodbye first so the fleet does not show a ghost. `orb_net.c:233` is the
   reason this is never a side effect.
3. **Display-pairing, not manager self-assertion.** `POST /api/v1/pair/start`
   mints a 6-digit nonce the orb renders full-screen; the operator types it
   back; the orb issues a session token. Proves the requestor had display
   access. Threat model is explicit: physical presence, over HTTP, not
   cryptographic.
4. **Pairing is permanent.** One pairing per orb, stored in NVS, TTL forever
   unless revoked. Revocation is a factory reset **or** a dedicated on-device
   "Clear pairing" row, so an owner without the hub is never stranded. Every
   state-changing write is gated, timezone included. While `s_provisioning` is
   true (`orb_net.c:120`, set only at `:3573` on no-credentials boot) the orb
   is exempt, so first setup needs no code; the exemption cannot be reached
   remotely, and the reset that re-enters it sits behind pairing plus arm.
5. **OTA is its own plan.** Sequenced after the fleet UI is real. Prerequisites
   stated up front: `h_status` must carry `built` + `elf_sha` so
   `firmware.Compare` has something it can compare, and `/api/v1/ota` must earn
   its way back into `deviceRoutes` (`security.go:232-236`) against the
   validated-bundle guarantee that exclusion cites.
6. **Unconfigured orbs reach the owner by QR.** Factory reset leaves the orb in
   the SoftAP flow of `docs/ap-onboarding-qr-plan.md` (PLAN ONLY; the QR half
   is not written). The Add wizard hands off to the phone rather than implying
   the desktop finished.
7. **LAN refuses during a cable flash.** Correlated by MAC, since the port
   holder and the IP proxy have no shared key until item 1.
8. **The board gate is mandatory**, per item 2 below.

### 5.2 Facts that changed the design (verified 2026-10-12)

- **`ESP_MAC_WIFI_SOFTAP` is base + 1.** `mac_addr.c:366-387` derives it as
  `memcpy(base)` then `mac[5] += 1`, live because
  `CONFIG_ESP_MAC_ADDR_UNIVERSE_WIFI_AP=y` on this S3 target. STA and BASE are
  exact copies. Both of the hub's join keys are BASE: the USB serial descriptor
  (`esprom/identity.go:138`, `usbclass.go:22-25`) and the flasher's eFuse read
  (`esprom/commands.go:222-223`). Copying the existing
  `esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP)` idiom (`orb_net.c:667`, `:2253`)
  into `h_status` would report base+1, miss every USB correlation by one in
  the last octet, and be invisible on this bench. `h_status` uses
  `ESP_MAC_BASE`, and that is now hardware-verified rather than read from
  source: on orb-B the field returned `44bd8d782618`, equal to `esptool
  read_mac` on that board, to the eFuse read taken through the native Go
  flasher, and to the serial-number descriptor sysfs exposes for the same port
  (`/sys/bus/usb/devices/1-2/serial`). That measured equality is what the hub
  needs, and it makes the offset itself no longer load-bearing: whatever
  SoftAP's address turns out to be, the field is the BASE the join keys use.
  The AP name was not observed, only derived: `orb_net.c:671-672` formats it
  from `ESP_MAC_WIFI_SOFTAP`, so with the documented base+1 it should read
  `orb-2619` on that board while its base ends `2618`. Raise the AP once and
  the derivation becomes an observation. The displayed SSID comes from the
  boot verdict (`setBoot`, `main.go:127-147`), never recomputed in Go, so the
  `ap_fill_config` SSID builder (`orb_net.c:667-668`) stays the single source
  of the name.
- **The image chip ID is at bytes 12-13, little-endian, 2 bytes.** Packed
  `esp_image_header_t` is exactly 24 bytes (`esp_app_format.h:79-107`);
  `esp_chip_id_t` is asserted 16-bit (`:33`) and `ESP_CHIP_ID_ESP32S3 =
  0x0009` (`:23`). `s3ImageChipID = 9` (`commands.go:38`) was declared and
  never referenced: the flasher verifies the attach-time magic
  (`commands.go:205-216`) and so knows the chip is an S3, but never checks
  what is about to be written to it. Exercised on hardware 2026-10-12: a
  committed bundle with its app chip ID doctored from 9 to 2 was refused with
  "built for the ESP32-S2 (chip ID 2)" before any write, and the same board
  then accepted the untampered bundle through `Session()` and booted it.
- **Headerless images are legitimate.** `partition-table.bin`, otadata, and
  `../assets/clock_assets.bin` carry no `0xE9` header. The gate skips them, as
  `patchBootloaderFlashParams` already does (`flash.go:67-69`). A gate that
  rejected them would break every real flash.
- **`macNorm` (`watch.go:173-176`) is not enough.** It lowercases and maps
  `-`→`:`, but never collapses the separator-free hex a device emits, and lets
  an empty MAC equal an empty MAC. It must canonicalize and refuse empty.
- **The 960 B status buffer measured 668-669 B on the cable**, replacing the
  650 B estimate the comment carried. The `mac` field costs a fixed 21 B,
  counted off the wire. The figure wobbles a byte with the free-psram digit
  count and six bytes with the width of `fw`, so it is measured per build and
  never added up.
- **`wifi_reason` outlives a successful connect.** Observed on orb-B
  2026-10-12 after hard resets issued over the serial cable by esptool and by
  the native Go flasher: `state: connected`, `assoc_fails: 0`, and still
  `wifi_reason: 3` named `auth-leave`. It is the last disconnect code, not a
  current failure. The device's own page labels it so (`orb_net.c:2973`,
  "last reason"). Any hub wizard or pairing screen that surfaces it must repeat
  that label and must never gate on it, or an orb that is healthy reads as
  broken and an orb that is broken reads as healthy.
- **LWIP_IGMP is forced to 1** (`components/lwip/port/include/lwipopts.h:474`),
  so the hand-rolled advertiser's multicast joins work in this build.
- **Socket budget is 10** (`sdkconfig.defaults:68`): one mDNS socket with two
  `IP_ADD_MEMBERSHIP` calls, not per-interface sockets.

### 5.3 The ladder

Each commit carries the §0 verification ladder. Firmware items are gated on
the cable; hub items go green on this bench.

| # | Commit | Depends |
|---|---|---|
| 1 | `feat(net): report the station MAC in /api/v1/status` | none (keystone for 6, 7, 9) |
| 2 | `feat(esprom): refuse a flash whose image chip ID is not ESP32-S3` | none |
| 3 | `test(orb-manager): give orbfake the device verbs` — done; mDNS advertiser moved to 5 | none |

Item 3 landed as the verbs and shipped with two pieces moved off it. The `menu` and
`pair` verbs in its original description do not exist in firmware until items 8 and
9, and putting them in the double early would make the fake answer routes no orb
serves — the self-agreeing-double failure this package's own doc comment was written
to prevent. The mDNS advertiser moved to item 5, to be written against the real
announcement from item 4: an advertiser built now can only be tested against a
browser that is also fiction. Item 4's dependency on 3 stands, because announcing a
service needs no test double.
| 4 | `feat(net): announce _orb._tcp on the STA and AP interfaces` | 1, 3 |
| 5 | `feat(orb-manager): browse _orb._tcp and list candidates without dialing` | 3, 4 |
| 6 | `feat(orb-manager): name, remember and cross-lock the fleet` | 1, 3, 5 |
| 7 | `feat(orb-manager): merge onboarding into Manage as Add an orb` | 6 |
| 8 | `feat(net): render the option registry as /api/v1/menu` | 1, 3, 6 |
| 9 | `feat(net): gate device writes behind a display pairing code` | 1, 6, 8 |
| 10 | `feat(net): factory reset into provisioning mode` | 8, 9, QR screen |
| 11 | separate plan: OTA | 1 |

Item 10 is last on purpose: the most destructive new verb is the only one that
cannot ship until the pairing gate and the on-device arm both exist. Item 9
must also register `/api/v1/menu` and the pair routes in `deviceRoutes`
(`security.go:238-253`): the last device verb added in firmware but not in
that map was unreachable from the hub.

Device header is `X-Orb-Pair`, never `X-Orb-Token`: `X-Orb-Token` is the hub's
own control token (`security.go:32`), and reusing it would let a proxied
request satisfy the hub's gate with a device secret. `proxy.go` strips any
client-supplied value of both names before injecting the stored one. The nonce
lives about 120 s with a small attempt cap, so 10^6 is never guessable; the
token is what the hub keeps. Fleet identity is `{mac, name, room}`, keyed on
MAC and never on IP, so a DHCP rebind cannot orphan a name or misroute a
credential; liveness comes from announcements, replacing the 6 h `ipGrantTTL`.

### 5.4 Documentation debt carried by these commits

Item 5 rewrites the six sites that encode the discovery ban: `README.md:12-14`,
`main.go:1-8`, this file's §3, `web/app.js:639-641`, `watch.go:93`, and
`internal/orbnet/client.go:60`, the last of which still justifies its timeout
against a "subnet sweep" that no longer exists. Item 6 rewrites the
`state.go:16-20` promise that "no credential is stored anywhere", which the
permanent token makes false; the hub still refuses to persist a Wi-Fi
passphrase, since the orb already owns that (`orb_net.c:1819-1820`).
`orb_net.c:225` retires its "Phase 4" `PATCH /api/v1/settings` note: that
numbering is the net plan's, not `app-platform-plan.md`'s P4, and the real
target is `/api/v1/menu`.

### 5.5 Logged, not built: the settings content gap

The `opt_*` plumbing is accepted and renders through the shell, but only
`clock_themes.c:207` registers rows today. Idle-dim timeout and motion
threshold, knob steps-per-detent, SNTP servers, and the app `enabled`/`failed`
toggle each still need an owner's rows; the `enabled` toggle arrives with P4
(`app-platform-plan.md` §12.6). The plumbing being ready is not the content
being ready, and the hub cannot show a row nobody registered.

### 5.6 The fork with the P3 line, and the resolution already proven

Verified 2026-10-12 before pushing anything. `origin/main` moved to `62a5e91`
carrying nineteen app-platform P3 commits from `gorb-os-2`: the broker, the
grant table, the D18 theme family, the P3 pass line, and the C2 fault counter.
This clone holds its own line of commits on top of the shared `d7de71f`, ending
with items 1 and 2 of this ladder. Neither side is an ancestor of the other, so
a plain push is rejected and `--force` would delete their P3 work. Neither was
done.

The only real coupling is `h_status`: `d2839ae feat(svc): P3.6` publishes the
broker ledger from the same snprintf that now carries `mac`. Their branch adds
no endpoints, so the hub's route-vs-token test sees no new obligations, and it
touches no orb-manager file at all.

The merge was resolved and gated in a scratch worktree, not guessed. Take their
`char json[1536]`, because the ledger needs it and the `mac` field's 21 B fits
with room to spare. Keep both blocks in the second hunk and restore the one
brace the shared context line would otherwise have eaten. Git's auto-merge of
the format string is correct: 44 conversion specifiers, with both `mac_str` and
`apps_json` present in the argument list. Both gates passed on the resolved
merge: `idf.py build` exited 0 with no warnings for `orb_net.c`, and the compiled
image contains `"mac":"%s"`, `"apps":%s`, `ledger too large`, and
`status too large`. All thirteen host suites pass merged, including the broker
suite `tools/host_svc_test.py`, and all six hub packages pass with the bundle
guard armed.

Two obligations fell out of a merge, and neither was optional. The bundle had
to be re-packed from the merged build: armed against it, the freshness guard
went red on `gorb-os.bin` and `bootloader.bin` exactly as designed, and shipping
it unre-packed would have put out a hub whose firmware has no broker. The merged
status body had to be re-measured on the cable before the buffer comment could
state a number. Both are discharged, in 5.6.2.

#### 5.6.1 How the merge landed (2026-10-12, same day)

The P4 commits landed first and the other agent did the merge, as `af1bc79` in
`gorb-os-2` (parents `e223a01` and `3d2df79`). This clone's `main` was then
rebased onto it, which is a fast-forward: the nine fleet commits are ancestors
of the merge, so no commit was rewritten and none duplicated.

Their resolution differs from the scratch one in two ways, both better. The
`json` buffer is `static`, not stack, following the `s_scan_json` rule: 1536 B
on a 4 KB httpd task is most of a stack. And the ledger row grew past what this
section was written against, gaining `declared` and `denied_mask` beside
`grants`. The `mac` code came through untouched, still `ESP_MAC_BASE`, still
feeding `mac_str` to the same snprintf.

Their line never reached `origin`. `origin/main` sat at `62a5e91` throughout, so
the push that closes this session publishes their P3 and P4 work from this clone
alongside the fleet commits.

#### 5.6.2 Cable verification of the merged image (2026-10-12)

The merge left one obligation unmet and the guard caught it: it kept the fleet
branch's bundle, so the hub shipped a `gorb-os.bin` with no broker in it. Armed
against a real merged build, the freshness test named the two offenders,
`bootloader.bin` and `gorb-os.bin`. Nothing went out until both were rebuilt.

orb-B runs the merged firmware. `serflash.sh` gated each write (`want=f716ffa
have=f716ffa`), and the board answers with `fw: f716ffa`, `img: valid`,
`slot: ota_0`, `mac: 44bd8d782618`, and three ledger rows: clock, color_picker,
menu. Item 1's field therefore survived the merge intact, and the base MAC still
agrees with the eFuse, the Go flasher and the sysfs descriptor. The committed
bundle is packed from that same image, so its stamp names an ancestor of HEAD.
That is the convention the repo already follows, because an image cannot be
stamped with the commit that stores it.

Two findings came out of measuring, and both are in the buffer comment now:

1. **The ledger binds before the frame.** `apps_json` holds the whole array in
   768 B, and the three compiled rows filled 440 B of it, 146 B per row. Two more
   rows fit. The P3 note's `~110 B` per row was low by a third, because `declared`
   and `denied_mask` joined the row after that figure was set. A fifth app reaches
   `ledger too large` while the 1536 frame still has room, so a P4 that installs
   apps has a lower ceiling than its buffer suggests.
2. **The body length is not a constant.** Ten requests at one commit read 1117 B.
   Ten at the next read 1116 B, and a fresh boot read 1111 B. `wifi_reason_name`
   is a variable string out of `reason_name()`, whose longest entry is
   `handshake-timeout` at 17 B, and `idle.dimmed` prints `true` or `false`. A
   wizard must treat the size as a range of 1111-1123 B, not a single reading.

`host_idle_dim_test.py` is the suite that catches a missing Kconfig symbol, and
it fired for the wrong-looking reason: the merged tree needed
`CONFIG_ORB_APP_POWER_MAX_S`, which their P4 added as a new `default y`. The
local untracked `sdkconfig` excluded it. One symbol of 1185 was missing, so the
`build/config/sdkconfig.h` diff against a healthy clone found it in one pass,
and the suite went green. Thirteen of thirteen host suites now pass.

The comment edit that recorded all this is not inert: shortening the block moved
`__LINE__` for every `ESP_LOGx` below it and changed 75 bytes of the image. A
comment-only change therefore still owes a rebuild and a re-flash, or the board
runs code the tree no longer describes.

