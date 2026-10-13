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
| 4 | `feat(net): announce _orb._tcp on the STA and AP interfaces` — done on STA, AP join owed | 1, 3 |

Item 4 shipped `main/net/orb_mdns.c` hand-rolled, one socket with two
`IP_ADD_MEMBERSHIP` joins, as §5.2's socket note requires. IDF v5.4 has no in-tree
mDNS component, and the managed one allocates a socket pair per interface against a
four-socket margin. The captive-DNS responder at `orb_net.c:860` was the in-repo
template: a pure builder lifted for host tests, and a task that owns its socket.

What the bench settled, measured rather than reasoned:

- IGMP is genuinely compiled in. `#define LWIP_IGMP 1` at
  `components/lwip/port/include/lwipopts.h:474` in the IDF tree, which is where that
  §5.2 citation points, and the ELF carries `igmp_joingroup_netif`. Without it an
  advertiser sends into a void and looks healthy in the log.
- `IP_PKTINFO` is declared at `sockets.h:282` and never implemented in `sock.c`, so
  one socket carrying two joins cannot report which join a query arrived on. The A
  record therefore follows the querier's subnet. Answers go back unicast, which is
  what makes the missing option harmless.
- The stack is sized to a reading. FreeRTOS reported 2,020 bytes free of 4,096 after
  the first announce burst, implying a 2,076-byte peak, and the same peak appeared on
  the 5,120-byte build before it was cut. The first 3,072-byte version overflowed
  because it carried three 512-byte packet buffers on that stack; they live in PSRAM
  now. The task prints its own headroom each boot, so the number cannot silently go
  stale.
- The AP SSID derivation is now an observation, retiring §5.2's outstanding line: the
  boot log prints `advertising orb-782618._orb._tcp (mac=44bd8d782618 ...) while the
  SoftAP is named orb-2619` on the same board, base and base+1 side by side.

Verification was kept independent on purpose. `tools/host_mdns_test.py` lifts the
codec between its own banners and asserts wire bytes, with twelve mutations all
caught; it also lints the plumbing, because the codec is compiled there and the
plumbing is not. On the wire the announcement is read by python-zeroconf 0.151.5,
not by anything in this repo: a browser of mine agreeing with an advertiser of mine
would be the same self-agreement that moved the fake off item 3. It parses PTR, SRV
`port=80 server=orb-782618.local`, TXT and A, with the cache-flush bit set on the
three unique records and clear on the shared PTR. Two scripts hold the recipe:
`tools/bench/probe_orb_mdns.py` and `tools/bench/browse_mdns.py`.

Two limits belong in the ledger rather than in a footnote. First, this WSL2 box does
not originate multicast onto the LAN: the same PTR query gets 184 bytes back over
unicast and nothing over the group or broadcast, while inbound multicast is received
fine. So the browse is verified passively, from the orb's own announce burst after a
reboot. Second, the SoftAP was never observed on air from this bench. The AP half of
the join is coded and the per-interface A record is host-tested, but
`POST /api/v1/wifi/ap` returns 200 without the driver ever logging `WIFI_EVENT_AP_START`, so the
announcement that half would send has never been seen. That path is not item 4's,
and the reason is causal rather than experimental: the responder's hook runs on
`WIFI_EVENT_AP_START`, so it cannot be what stopped that event from firing. A build
with `CONFIG_ORB_NET_MDNS=n` showed the same silence, but that arm posted a
form-encoded body rather than the JSON the endpoint parses, so it is corroboration
and not a control. Item 5 must treat the AP-side announcement
as unverified until a phone or a second radio browses `192.168.4.0/24`.

Third, corrected by measurement (2026-10-13, the true control the paragraph above
called for): an `MDNS=n` image — responder strings verified absent from the binary,
5.7 KB smaller — was built into a scratch dir and MAC-gated-flashed to orb-B, then
`POST /api/v1/wifi/ap` with the JSON body the parser accepts. The SoftAP still
panicked, identically: `wifi:alloc eb len=752 type=4 fail` twice, then
`LoadProhibited`, `EXCVADDR 0x2c`, backtrace
`ieee80211_hostap ← wifi_softap_start ← _do_wifi_start ← wifi_set_mode_process ←
ieee80211_ioctl_process ← ppTask` (symbolized off the scratch ELF). The responder is
exonerated. The failed 752-byte allocation is a WiFi-driver EB from its own buffer
pool, not the general heap: at the crash the control held `largest=7168` and
`dma_free=5863`, both far above 752. Two consequences for the fix: a heap gate on
`heap_caps_get_largest_free_block(INTERNAL|DMA)` (the planned step 2) would not have
refused this call — the block it checks was big enough — and shrinking the mDNS task
stack (the planned step 3) removes a consumer that was not the cause. orb-B was
restored to the verified `094fa22` and re-checked healthy (`ap=false`, connected)
afterward. The AP fix must instead address the driver's EB pool: pre-provision the
SoftAP's buffers at `esp_wifi_init` while internal DRAM is ample, or shrink the
STA-side pool (dynamic RX/TX `32/32`, AMPDU, `MGMT_SBUF_NUM`) so the AP beacon's EB
still fits. The Wi-Fi-side browse experiment stays blocked until the AP actually
comes up — there is nothing to join while the open panics. Item 4's AP half stays
open.

**Resolved by the second build (same session).** The driver-heap diagnosis was
confirmed and the AP half closed, on the air. A scratch image with `MDNS=y` and the
driver's dedicated EB demand trimmed — `STATIC_RX_BUFFER_NUM 10→6`,
`DYNAMIC_RX/TX_BUFFER_NUM 32→12`, `MGMT_SBUF_NUM 32→12` (config only, no tracked
source change) — brought the SoftAP up on the *same* JSON POST that had panicked:
`WIFI_EVENT_AP_START` logged, `SoftAP "orb-2619" OPEN (900 s hold)`, captive DNS
answering, and `orb_mdns: advertising _orb._tcp on SoftAP`. The host's second radio
(WSL mirrored `eth1`, wired `eth0` holding the LAN) joined `orb-2619` at L2 (AP BSSID
`44:bd:8d:78:26:19` = STA base +1), was placed on-subnet `192.168.4.100`, and an
independent `zeroconf 0.151.5` decoder, sending the browse query *unicast* to
`192.168.4.1:5353`, read back PTR→SRV(`port=80 server=orb-782618.local`)→TXT(47 B)→
**A `orb-782618.local = 192.168.4.1`** — the querier-subnet pick (`orb_mdns.c:419`)
proven on air for the first time, returning the SoftAP address because the querier sat
on the AP `/24`. The config page answered `200` at `192.168.4.1`, closing item 10's
provisioning path too. Three bench notes the attempt surfaced: WSL mirrored mode owns
the joined NIC's IP and reverts a host-side `netsh` static, so the on-subnet address
was set from inside WSL; `dig` cannot elicit the answer because it discards the orb's
RFC-6762 `id=0` reply, so the query must be framed as real mDNS; and the orb answers a
query from an ephemeral source port (`orb_mdns.c:630` replies to the `recvfrom` peer).
**Shipped.** The trim is in `sdkconfig.defaults` (the tracked file, so a clean
clone rebuilds the working pool size), committed at `bafcd4e`, and the committed
firmware bundle was re-packed from a clean `build/` via `make firmware` with the
`BuiltInBundle` staleness guard and full `make check` green. orb-B runs this image
(stamp `bafcd4e`): the same `POST /api/v1/wifi/ap` that panicked now returns `200`,
reaches `ap=true`, logs `wifi:mode : sta … + softAP`, `SoftAP … OPEN`, and
`advertising _orb._tcp on SoftAP` with no Guru. The minimality pass — the smallest
counts that still open the AP every time, since buffer headroom also carried the OTA
art-bank transfer — is deferred to the next firmware change: `6/12/12/12` is validated
and safe to ship now, so shipping a known-good config beat tuning it further at the
risk of re-introducing the panic. The surgical alternative (an EB-reclaim retry inside
`ap_open()`, no throughput cost) stays open as item 4's possible follow-up.

**2026-10-13 correction — item 10 REOPENED; item 4 vindicated by measurement.**
The paragraph above closed item 10 on a false premise, and the error was still
being repeated an hour ago, so both halves are recorded here.

Item 10's closure was an artifact. The bench client that "answered `200` at
`192.168.4.1`" was hand-assigned `192.168.4.100` (see the `netsh` note three
lines above), and every byte proven over the SoftAP was **unicast** — the mDNS
probe was deliberately framed unicast to `192.168.4.1:5353`. **DHCP over this AP
has therefore never been tested even once.** A DHCP DISCOVER is broadcast at both
L2 and L3, which is the one traffic class that test could not exercise.

First real phone join, `af43c50-dirty`, config-page-opened `orb-2619`. **The
phone IS issued an address.** `esp_netif_dhcps_get_clients_by_mac()` reports
`192.168.4.2` three seconds after association and `192.168.4.3` one second after
a second join, and the driver establishes an AMPDU block-ack session with the
client (`<ba-add>idx:2 (ifx:1, …), winSize:64`). The earlier "no lease, four
timeouts" reading was **wrong**: it rested on `IP_EVENT_AP_STAIPASSIGNED`, which
does not fire on this IDF path even when a lease is granted, so its silence was
evidence of nothing. There is therefore **no fleet-wide lease regression**, the
shipped bundle needs no emergency revert, and `bafcd4e` stands. Still open is
only the user-visible half: a client took its address and left ~16 s later
(`reason 8`, a normal client-initiated deauth), and httpd logs no requests, so
whether the config page actually rendered on the phone is unmeasured.

The trim was **not** the cause, and `bafcd4e` is **correct**. Restoring the IDF
defaults (10/32/32/32) and re-running the same POST brought the panic straight
back: `alloc eb len=752 type=4 fail` twice, then `LoadProhibited` in
`ieee80211_hostap`. `esp_wifi_disconnect()` immediately before the promotion
does **not** help either — heap genuinely improves (`free=12791 largest=7168`)
and it still faults, because static RX EBs are carved at `esp_wifi_init` and a
disconnect never returns them. The failing 752 B type=4 EB *is* the beacon: the
driver logs `Init max length of beacon: 752/752`. So `static-rx=6` works by
leaving slack in that carve, and no amount of station-releasing substitutes for
it. Both dead ends are now written into `ap_open()` and `sdkconfig.defaults`.

Two things that were asserted as fact and are **false**, listed so nobody trusts
them again: `RX_BA_WIN` was never `32→6` (independent symbol, default 6 all
along, and a lower static-rx satisfies the guidance anyway), and there is no
`--rebind-orb` flag or `tools/orb_erase.sh` — neither exists.

Why the failure was invisible: `lwip/apps/dhcpserver/dhcpserver.c` is 1567 lines
with **no logging of any kind**. The device could not distinguish "no DISCOVER
ever arrived" from "it arrived and the server refused it," which is why four
hypotheses got traded instead of measured. Instrumentation now in place:
`WIFI_EVENT_AP_STACONNECTED` posts a delayed
`esp_netif_dhcps_get_clients_by_mac()` poll, so one join reports whether the
server actually granted a lease. The `netif->input` frame counter tried first was
REMOVED — IDF delivers Wi-Fi frames through `esp_netif_receive()`, which bypasses
`netif->input`, so that counter read zero during provably-live traffic. Treat any
`netif->input` hook as structurally unable to witness RX in this stack.

Two build findings that affect the fleet test, unrelated to WiFi:
`sdkconfig` (untracked, generated) **overrides** `sdkconfig.defaults` — `idf.py`
will not re-apply defaults to an existing `sdkconfig`, so deleting lines in the
tracked file silently changes nothing (measured: a "reverted" build still had
6/12/12/12). And `ORB_IDLE_DIM_*`, `ORB_IDLE_MOTION_*`, `ORB_NET_AP_HOLD_S`,
`ORB_NET_AP_MANUAL_S`, `ORB_NET_AP_RESCUE_FAILS` and `ORB_APP_POWER_MAX_S` exist
**only** in that untracked file, so a clean clone does not reproduce this
device's build. Related gap: nothing can clear the net credentials — there is no
erase URI in `orb_net.c`, no hub endpoint, and `main.c:60`'s `nvs_flash_erase()`
cannot reach `storage`/`orb.net` by design. Item 10's "factory reset then
re-provision" therefore has no trigger, and the rescue path is today the only
door back to the SoftAP.

**2026-10-13, later the same day — item 10 CLOSED. Root cause found, and it is not
WiFi at all.** The fault is `CONFIG_LV_MEM_SIZE`: LVGL's builtin heap was a 64 KB
**static array in internal DRAM** (`LV_USE_CLIB_MALLOC` is off, so it is not a
malloc), which left the network stack unable to hold a DHCP server, an HTTP server
and an AP in the same boot. Which symptom appeared depended only on which
allocation lost:

- Server started after the radio → `httpd_start` returns `ESP_ERR_HTTPD_TASK`,
  `main` logs `networking down — clock running without config page`, the clock
  keeps animating, the captive DNS still answers, and phones are pointed at
  `192.168.4.1` with **nobody listening**. That is "Secured, connected, no prompt."
- Server started before the radio → server comes up, but `dma_free` drops to 2095 B
  and the AP cannot answer a DISCOVER: client gets no address. (A reorder was tried
  and is reverted; it moved the failure, it did not fix it.)

Measured at the trim, same firmware, 64 KB → 24 KB LVGL pool: `post-wifi_start`
5,499 free / 3,328 largest → **63,803 / 31,744**; `post-httpd_start` 20,623 / 7,168
→ **47,051 / 23,552**. Provisioning on orb-B then handed out a lease **and served
the menu with the scan list** — verified by eye on the phone, which is the behavior
two earlier orbs were provisioned with. Side effects on the wired path confirm the
diagnosis rather than merely accompanying it: first HTTP request **26.9 s →
0.26 s**, and ping loss **12% → 0%** (control, same window: router 0% / 3.2 ms).
The clock is unaffected: `blend/s 913707`, no LVGL allocation errors.

Seven explanations were tried and falsified before this one, all of them inferred
from one side of a transaction: EB pool starvation, `esp_wifi_disconnect()` before
promote, broadcast-vs-unicast, a missing DNS option, DHCP option 114, late
promotion ordering (a driver stop/start was implemented, verified to remove the
double `AP_START` and the Guru, and **reverted** — it cost the LAN 8 s and fixed
nothing), and the buffer trim itself, since raising the counts to 8/24/24 made the
ping loss *worse* (4%/4%/8% at 154 ms mean vs 12% at 42 ms). The recurring error
was treating the device's own accounting as evidence of the client's experience:
the lease table said "ASSIGNED" while the phone said "nothing", seven times.

Still open, in priority order:
1. **Is the buffer trim still needed?** ~40 KB of internal DRAM came back, and the
   trim exists to give the beacon's 752 B EB a slot during late promotion. To test:
   delete the four `CONFIG_ESP_WIFI_*_BUFFER_NUM` lines from BOTH
   `sdkconfig.defaults` and `sdkconfig` and repeat the `POST /api/v1/wifi/ap` that
   first panicked. Until then the trim stays and `bafcd4e` remains load-bearing.
2. **No supported way back into provisioning on a credentialed device** (item 10's
   remaining half). Today this was tested by temporarily forcing the provisioning
   branch in a bench build; credentials were never erased. The real fix is a
   flag in `orb.net` that makes the next boot enter the provisioning path
   *without* wiping the stored SSID — proven feasible by that bench build — plus a
   hub/UI trigger.
3. **Raise `CONFIG_LV_MEM_SIZE` awareness**: 24 KB is sized to today's dial. If a
   widget-heavy screen lands and LVGL reports exhaustion, measure with
   `lv_mem_monitor()` and move the pool to PSRAM rather than growing it internally.
   The same 64 KB that broke provisioning is what the OTA failures (items 2/9) were
   tracing to, so this line is the cheapest lead on that problem too.

**How provisioning ever worked at 64 KB — measured, and it corrects the block
above.** A/B on the SAME forced-provisioning boot (bench hack taking the
`ssid[0] != '\0'` branch at orb_net.c:3903, so NVS is untouched), map-verified
LVGL array, IDF-default buffers, one variable:

| LVGL pool | Result on a provisioning boot |
|---|---|
| 65536 | `post-wifi_start` free 15,715 / largest 7,936 / dma 8,211 → `E orb_net: httpd_start: ESP_ERR_HTTPD_TASK` → `E main: networking down - clock running without config page`. The field symptom, reproduced exactly. |
| 24576 | server listening, DHCP server started, phone got a lease, scan list rendered. |

The two paths differ structurally, which is why only one was ever broken:
provisioning calls `esp_wifi_set_mode(WIFI_MODE_APSTA)` at orb_net.c:3934,
BEFORE `esp_wifi_start()` at 3971, so the driver sizes its EB pool for AP+STA at
init - no beacon block is retrofitted, no second AP_START. The config page and
the rescue path promote on a driver already started in STA mode: a different
allocation order, and the only one that ever Guru'd.

**Correction:** at 64 KB + default buffers the CONNECTED late promotion works
fine - HTTP 200 in 0.128 s, `option 114` logged, `SoftAP OPEN`, `DHCP server
started`, zero `alloc eb`, zero Guru; the AP costs ~7.6 KB of internal free. The
array did not starve that path. It starves the **provisioning boot**, where AP +
DHCP server + captive DNS + mDNS-on-AP + the 4 KB provserial stack + the boot
scan all coexist and httpd's task is the last thing that must fit.

So why did it pair the fleet and then stop? The margin was hundreds of bytes.
httpd's stack is 4,096 (esp_http_server.h:55); the failing boot's largest block
was 7,936 with DHCP, captive DNS and mDNS still to come. Every feature that
landed on the provisioning path after the fleet was paired took a slice: mDNS
advertised on the AP interface (094fa22), the captive-DNS + option-114 work,
PROVSCAN, provserial, the 15-handler route table. Nobody touched the WiFi code;
the boot path simply stopped fitting - and `networking down` at boot is the
signature of exactly that.

Caution, recorded the hard way: two probe readings earlier today that I
attributed to a 64 KB boot came from an image that was not map-verified and
differed from the true one by exactly 40,960. Treat only map-verified heap
readings as evidence. The same firmware also swings 39,575-48,071 internal free
with device state, so one `heap_internal` sample is not a capacity claim.

---

## DESIGN NOTE (not built) — "Open AP" should REBOOT into provisioning

Raised 2026-10-13 during the provisioning fix. Record only: no code, no schema
change, no endpoint. Decide the open questions first, then build.

**The ask.** The on-orb config page's "Open AP" button (and the hub's equivalent)
should put the orb into a state where the SoftAP is reliably joinable, **including
when the stored credential is now wrong**. Today it tries to do that without
rebooting, and that is the difference between the working case and the broken one.

**What exists today (four doors, only one of which has ever been proven):**

| Door | Path | Radio sequence | Proven? |
|---|---|---|---|
| No credentials in `orb.net` | boot branch, `orb_net.c:3911` — does NOT call `ap_open()`; inlines `set_config(AP)` + `set_mode(APSTA)` | `set_mode(APSTA)` from `WIFI_MODE_NULL`, before `esp_wifi_start()` | **Yes** — verified by eye on orb-B: lease + menu + scan list |
| Physical: power-cycle an unprovisioned orb | same boot branch (it is unprovisioned, so the same code) | same | Yes (that is how the fleet was first paired) |
| Serial: `PROV <hex-ssid> <hex-pass> <hex-tz>` on the console | `prov_reader_task`, armed only when the boot branch already took | (join on the running APSTA boot) | Yes — but only reachable on a device with no stored SSID, and only over a USB serial cable |
| Credentials refused 8x (`CONFIG_ORB_NET_AP_RESCUE_FAILS`) | `NET_CMD_AP_RESCUE` -> `ap_open()` | **late promotion** on a running STA | Fragile |
| Config page / hub "Open AP" (`POST /api/v1/wifi/ap`) | `NET_CMD_AP_OPEN` -> `ap_open()` | **late promotion** on a running STA | Fragile |

**Corrected 2026-10-13 — there is no physical door on a credentialed orb.** An
earlier version of this table claimed a "long-press-at-boot" door, pre-existing.
It does not exist: nothing samples the knob before `orb_net_start()`
(`main.c:201`), `orb_net_start()` takes no force argument, and `s_provisioning` is
assigned in exactly two places — set at `orb_net.c:3911` (no stored SSID) and
cleared at `orb_net.c:1506` (STA lease acquired). The long press is the in-app
"return home" gesture (`orb_app.c:199`, `input_router.c:242`) and reaches no
networking code. Row 1's mechanism was also wrong: the boot branch never calls
`ap_open()`, which is why it arms no expiry timer.

Consequence for the design: for a credentialed orb whose network has gone away,
the only doors are the two fragile late promotions, or opening the case and
attaching a serial cable. A wall-powered device with no button-reachable
recovery is the real defect here, and it is bigger than the button semantics.

"Late promotion" means `esp_wifi_set_mode(WIFI_MODE_APSTA)` on an already-running
station driver. Every failure seen in this investigation lived there: the Guru
panic inside `sta_set_mac`/`SetBlock`, the double `AP_START` (one per mode bit),
and the `alloc eb len=752 type=4 fail` that surfaced as a half-open AP — an
interface that associates and then dies silently, because the beacon is the only
thing that needs a type-4 buffer and the driver has no retry path for a failed
beacon allocation. The WiFi buffer trim (`bafcd4e`) was a workaround for exactly
this, and see the retest result below before assuming it still applies.

**The proposal.** Change the *semantics* of the button, not the mechanism: it
requests a reboot into the provisioning state instead of promoting the radio in
place. The boot-time path is the one that works, it is already written, and it
costs nothing to reach — a reboot from the APSTA-connected state is the same boot
sequence as a power-cycle, which has always opened a joinable AP.

**Why this is more than a workaround.** It removes an entire class of ordering
bugs by refusing to reconfigure the radio under load. It also matches the user's
mental model: pressing "Open AP" on a device that cannot reach its network is a
recovery action, and recovery actions restart things.

**Design points to settle before any code:**

1. **A boot-intent flag, not a credential wipe.** Needs a one-shot key in
   `orb.net` (e.g. `reprov`) that forces the provisioning branch even when
   credentials exist. The branch to force is the `else` at `orb_net.c:3904` — its
   real condition is `ssid[0] != '\0'`, not a `have_prov_creds` helper.
   Tonight's bench build proved the mechanism works: one forced condition on a
   credentialed orb entered the full provisioning path, served the menu and the
   live scan list, and erased nothing.
   Corrected 2026-10-13: preserving the SSID is still the right call, but NOT for
   the reason stated here originally. Nothing pre-fills the form. The stored SSID
   is never echoed — `/api/v1/status` has no `ssid` field, `GET /api/v1/config`
   returns timezone zones only, and the sole `"ssid"` string in the file is the
   parse of the POST body in `h_wifi_join`. So preserve the credential for two
   other reasons: it is the fallback the bounded sojourn reboots back to (point 3),
   and wiping it converts a recovery action into a factory reset. If pre-fill is
   wanted, that is a separate small endpoint, not a property of the flag.
2. **Write the flag before rebooting, and make it survivable.** If power is lost
   between setting the flag and booting, the device must still come up in
   provisioning, and the flag must clear on a successful re-provision or on the
   next plain power-cycle, so an orb cannot get permanently stuck in AP mode.
   Decide: does the flag expire (one boot only, or N boots)?
3. **Bounded sojourn, then resume — this is net-new code, not a config change.**
   Corrected 2026-10-13 against the source: a provisioning boot arms NO expiry
   timer (arming lives in `ap_open()` at `orb_net.c:811`, which the boot branch
   does not call), and `NET_CMD_AP_EXPIRED` deliberately does nothing while
   `s_provisioning` (`orb_net.c:1104` closes only on a lease; the re-arm branch is
   gated by `!s_provisioning`). So a boot into provisioning has exactly one exit —
   acquire an STA lease — and otherwise holds the AP open indefinitely. That is
   correct for a factory device and wrong for a recovery session, because the
   stored credentials may be perfectly good and the router merely rebooting: the
   orb would sit in AP mode forever instead of rejoining. Specify a real exit:
   arm the existing expiry timer on a flag-initiated boot, and on expiry REBOOT to
   normal boot so the stored credentials get retried. Note `ap_close()` cannot
   serve as that exit — it calls `set_mode(STA)` and leaves `s_provisioning` set,
   so it tears the radio down to a half state rather than restoring a join attempt.
4. **The page dies mid-action.** The config page is served over the LAN while the
   AP it advertises is a *different* network the phone must join by hand. A reboot
   means the tab the user clicked is gone. UI must state the consequence and give
   the post-reboot instructions inline: SSID `orb-<id>`, then `http://192.168.4.1/`,
   for N minutes.
5. **Hub visibility and the offline gap.** Rebooting into provisioning drops the
   hub connection for the whole session, so the hub must record the intent and
   show "awaiting re-provisioning" — and should stop alerting on "unreachable" for
   that device while the intent is outstanding. Otherwise the feature manufactures
   false alarms.
6. **Auth and blast radius.** Today the config page is open on the AP subnet with
   no auth and no CSRF protection (deliberate, so provisioning works). A reboot
   command is still harmless in the worst case (one reboot), but confirm it is not
   reachable in a way that lets a LAN neighbour reboot-loop the device; rate-limit
   or require the AP to be up first.
7. **Trigger set — corrected.** The config page button and the hub's orb actions.
   There is NO physical trigger to inherit (see the door table above): no boot-time
   knob sample exists, so "hold the knob while powering up" is work to be
   implemented, not an existing door. Given a credentialed orb otherwise has no
   button-reachable recovery, the physical gesture is arguably the most important
   trigger in this feature, and it is the only one that works when the network is
   gone and no phone is on the LAN.
   On the 8-failure rescue: it is still a late promotion, but the fragility argued
   for here was measured to be the LVGL pool, not the promotion. At 24 KB + default
   buffers the promotion path returns HTTP 200 in 0.128 s with `SoftAP OPEN`,
   `DHCP server started`, zero `alloc eb` and zero Guru. So moving the rescue to a
   reboot is now a judgement about robustness under an unattended failure, not a
   fix for a known panic. Do not move it on the strength of the old evidence.
8. **Keep a plain "Open AP" too, or not?** If late promotion is dropped entirely
   the code gets simpler and there is one path to test; if it stays, the config
   page has two buttons that look the same and behave very differently. Recommend
   one button, reboot semantics, and delete the promotion path once the rescue
   path moves over.

**Next step in the design (2026-10-13): settle ONE question, not eight.**
Points 1, 2, 3 and 7 are all downstream of a single choice — is provisioning a
*boot-time mode* with exactly one entry path, or a *radio state* the running
device can enter and leave at will? Tonight's measurements answer the empirical
half of it: booting into APSTA is the only door ever proven to work, and the
running-device door now also works at 24 KB. So the choice is no longer about
reliability; it is about whether a recovery action is allowed to reconfigure the
radio under load. Recommend boot-time mode, because it makes points 2, 3 and 8
collapse into "clear the flag and reboot", removes the need for an AP-close
endpoint, and makes every door identical to the one that works.
Before writing that spec, run one bench test that prices it. The serial `PROV`
door is armed only while `s_provisioning`, so forcing the boot branch on orb-B
both enters provisioning AND makes the join path drivable over the console — no
phone needed. Send a deliberately wrong passphrase and answer the one open
question the spec turns on: does the 8-failure rescue ever fire during a
provisioning boot? It is gated on `!s_ap_up` (`orb_net.c:1286`), and `s_ap_up` is
set true by the `WIFI_EVENT_AP_START` handler (`orb_net.c:1296`) on a boot that
never called `ap_open()`. If the gate blocks it, then a wrong password typed on
the provisioning page has no recovery but the bounded sojourn of point 3 — which
means the spec must make that sojourn end in a REBOOT, not an `ap_close()`.

**TEST RUN 2026-10-13 — done, and the answer is worse than the question.** On
orb-B (24 KB pool, IDF-default buffers, boot branch forced, real credentials left
untouched in NVS): `PROV <GTROUTER2> <wrong-pass> <Etc/UTC>` over the console
returned `prov: received ssid="GTROUTER2" pass=(set) +tz` then `PROV-ACK`. The
join attempt was refused — `wifi:state: auth -> init (0x600)` — and then the
device went silent. No `disassociated:` line, no retry, no attempt counter, no
rescue, no timeout. It sat broadcasting for the rest of the observation window.

The cause is one line: `if (s_provisioning) break;` at `orb_net.c:1260`, in
`WIFI_EVENT_STA_DISCONNECTED`, annotated `/* AP-only mode: nothing to retry */`.
That comment is false on this path. The boot branch deliberately sets
`WIFI_MODE_APSTA` rather than AP precisely so the station CAN associate (its own
comment at `orb_net.c:3927` says under AP-only "connect() is a no-op, so the orb
looks configured and never comes back"). So the flag that exists to enable a
join-in-provisioning is the same flag that discards the join's failure.
Consequences, all measured: (a) every retry ladder, backoff, and the mDNS goodbye
are skipped; (b) `s_assoc_fails` never increments, so the rescue threshold is
unreachable; (c) the 8-failure rescue is additionally gated on `!s_ap_up`, which
is true here via the `AP_START` handler, so it could not fire even if counted;
(d) nothing arms `s_ap_timer`, so there is no timeout. A wrong password entered on
the provisioning page therefore ends the session permanently, with the AP still
broadcasting and nothing behind it. The only recovery is resubmitting the form or
power-cycling.
  Corrected same day, after this test cost a bench credential: the submit path
DOES persist immediately — `nvs_set_str` of ssid and pass at `orb_net.c:2147`
(serial PROV) and `2450` (config page), before any lease is held. So a mistyped
passphrase is not a transient session failure; it overwrites the good credential
in flash, the orb is cut off from its network on the next boot, and the only
recovery is learning the real passphrase again. The recovery-AP design should
therefore withhold the write until a lease is held, or keep the previous
credential until the new one proves itself.

**This settles the design question, and it changes the shape of the work.** The
feature is not "make Open AP reboot into provisioning." It is "give a provisioning
session a failure path," which does not exist today at all. Point 3's bounded
sojourn is now mandatory rather than tidy — it is the only exit for the most
common operator error, a mistyped passphrase. And it must end in a reboot, not
`ap_close()`: a close leaves `s_provisioning` set, which re-triggers the same
discard. Fixing `orb_net.c:1260` to retry under APSTA is a one-line change worth
making on its own merits regardless of which way the larger design goes, because
it is the reason a mistyped password looks like a dead device.

**Explicitly out of scope for this note:** whether the AP is joinable without an
IP (it is — static lease `192.168.4.2`, and wired tests are unaffected by the
phone's AP address), captive-portal DNS specifics, and the separate question of an
erase-credentials endpoint.

**Measurement this note depends on.** The trim's own necessity is now in doubt —
see the retest result appended directly below.

**RETEST RESULT 2026-10-13: the trim is NOT needed. Removed.** With all four
`CONFIG_ESP_WIFI_*_BUFFER_NUM`/`MGMT_SBUF_NUM` lines deleted from BOTH
`sdkconfig.defaults` and the generated `sdkconfig` (verified in
`build_*/config/sdkconfig.h`: 10 / 32 / 32 / 32, mgmt-sbuf 32), five independent
first promotions — each a fresh boot, station connected to the LAN, then
`POST /api/v1/wifi/ap` — reached `SoftAP "orb-2619" OPEN` with the lwIP banner
`DHCP server started on interface WIFI_AP_DEF`. Zero `alloc eb len=752 type=4
fail`, zero Guru, zero backtraces, in any capture window. Heap at defaults:
internal free 64,415–69,907, largest block 28,672–31,744, dma_free 56,911–62,403.
For contrast, the boot that could not promote had `dma_free 2,095`.

The measurement that originally justified the trim recorded defaults failing with
`alloc eb len=752 type=4 fail` then a Guru in `ieee80211_hostap`. It was taken at
`CONFIG_LV_MEM_SIZE=65536`, so it measured the LVGL shortage, not the buffer
counts — the EB pool was starved from the other direction. Both facts stand; only
their attribution changes. `bafcd4e` is therefore superseded by `9d21afc`, not
complemented by it.

The wired path refused to be a discriminator. Four 250-packet samples at 5 pps:

| Build | AP state | RSSI | loss | mean rtt |
|---|---|---|---|---|
| defaults | down | −71 | 11.6% | 60.6 ms |
| defaults | up | ~−70 | 8.0% | 59.5 ms |
| trim 6/12/12/12 | down | −61 | 5.6% | 50.8 ms |
| trim 6/12/12/12 | up | −63 | 10.0% | not recorded |

Router control, wired, same host, same window: **0.4%**. Loss tracked RSSI, not
buffer counts, and the order even flipped between the two AP-up/down pairs. The
earlier "12% → 0%" improvement claimed for the LVGL fix is not reproducible in
tonight's RF and should be read as a coincidence; the claim that raising counts to
8/24/24 made loss *worse* was the same noise. Ping loss on this bench is a
WiFi-link measurement, not a memory one — the orb is the only wireless hop in it.

**Two harness bugs that each produced a false reading, recorded because both will
recur.** (1) `curl` reported `HTTP 000` on requests whose bodies were fully
received: esp_http_server holds the connection open, and curl's `--max-time` fires
waiting for the close. Add `-H 'Connection: close'` (or `--no-keepalive`) or every
timeout is ambiguous — with it, the same promote POST returned `HTTP 200 in
0.168s` and the body `{"state":"ap_opening","ap_ssid":"orb-2619","seconds":900}`.
Distinguish `000` with a written body (keep-alive, request landed) from `000` with
an empty body (unreachable).

**2026-10-13 — "was 64 KB raised for a reason?" No. Measured, and the answer
changes what the risk actually is.**

Provenance first, because the premise was wrong in a useful way: `65536` was never
chosen by anyone. It is LVGL's own default (`managed_components/lvgl__lvgl/Kconfig:112`,
`default 65536`), and `git log --all -S 'CONFIG_LV_MEM_SIZE'` returns only my two
commits — no tracked file ever mentioned the symbol, so no requirement was being
honored and no prior rationale exists to preserve. The app-based UI train is
explicitly not the cause: `orb_carousel.c` creates exactly two objects total
(`s_chip` and one label, `main/app/orb_carousel.c:105-108`), independent of how
many apps are registered.

Demand was then measured rather than estimated, using `lv_mem_monitor_core()`
sampled on the LVGL task (the only context that mutates the pool; there is no
LVGL locking in this app) and exposed through `/api/v1/status` — a temporary probe,
since reverted. The app was driven through every screen it has (clock, menu plus a
full knob walk in both directions, color picker):

| Pool | Usable (TLSF) | Live used | **Peak** | Biggest free block | Allocations |
|---|---|---|---|---|---|
| 24,576 | 22,052 | 15,804 | **19,796** | 5,936 | 310 |
| 65,536 | 62,880 | 15,804 | **19,796** | 46,764 | 310 |

Peak is identical to the byte at both sizes. The extra 40 KB bought literally
nothing; it only parked memory the network stack needed. On the same boot at
64 KB, internal free measured 7,411 / largest block 3,328 — the exact signature of
the provisioning failure, reproduced by accident and matching the original
5,499/3,328 reading.

**So the negative impact of the reduction is headroom, not behavior.** Nothing
regressed on the dial (15 fps, `blend/s` in the same band, zero LVGL allocation
warnings, `art_ready` true), but peak sits at 90% of the 24 KB pool with 5,936 B
in its largest block. Two consequences for anyone adding UI:

- Exhaustion is **silent**, not fatal. `CONFIG_LV_USE_ASSERT_MALLOC` is off (LVGL
  asserts are opt-in and none are enabled in this build), so a failed allocation
  logs `LV_LOG_WARN` and returns NULL; `lv_obj_create` yields NULL and the widget
  simply never appears while the clock keeps rendering. That is the same
  invisible-failure shape as the provisioning bug, so a new screen must be checked
  for `lv_max`, not just looked at.
- Screen switching does not grow the pool: 310 allocations before and after the
  full walk, because views are created once and reused. Growth would come from new
  persistent widgets, not from more apps in the carousel.

`CONFIG_LV_USE_CLIB_MALLOC=y` was tested as a way out of the fixed array and is not
the fix it appears to be. Internal free came back healthy (55,183 / largest 25,600)
and the clock held 15 fps, but PSRAM was untouched (7,472,960 free, unchanged):
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` routes every allocation under 16 KB to
internal RAM regardless of PSRAM, and LVGL objects are all small. It also blinds
the readout — the clib core has no TLSF walker, so `lv_mem_monitor_core()` returned
1,070,150,384 for total. It buys flexible sizing, not PSRAM residency. Moving LVGL
objects onto PSRAM for real means lowering `ALWAYSINTERNAL` and then re-measuring
dial frame rate, since object reads would cross the SPI bus.

Recommendation: keep 24,576, and re-measure with this same probe the first time a
screen adds persistent widgets. A permanent `lv_*` field in `/api/v1/status` is the
cheap guard — it is roughly ten lines, read-only, and is the only way this failure
mode is visible from the hub without a serial cable. Not added, pending the
decision on what the status payload should carry. (2) After repeated hard resets the host's ARP entry
for `.34` sat in `FAILED` for a minute or more while the orb's own serial log
showed a valid lease and a working clock, and no number of plain pings fixed it.
`ip neigh del 192.168.2.34 dev eth0` cleared it instantly — interface is `eth0`,
not `eno1`. A "device stopped responding" reading on this bench is therefore
suspect until the ARP entry has been checked or cleared.

**Finding made while testing this: there is no way to close the AP.** The route
table registers only `/wifi/scan`, `/wifi/join` and `/wifi/ap`; a POST to
`/api/v1/wifi/ap/close` returns 404 `Nothing matches the given URI`. `ap_close()`
exists in the file and is reachable only from `NET_CMD_AP_EXPIRED`, so the sole
door down is the hold timer — `CONFIG_ORB_NET_AP_MANUAL_S=900`, fifteen minutes of
a broadcast SSID with an unauthenticated config server after anyone opens it from
the LAN. Any redesign here must add an explicit close, and the reboot semantics
proposed above make the question moot inside a provisioning session (the session
ends by rebooting) but do not remove the need for one.

**Methodology warning, self-inflicted and worth recording.** Four close POSTs in a
row returned 404 with the response body discarded (`-o /dev/null`), and the run
was read as "seven clean promotions". It was one promotion and six no-op re-arms
of an AP that never went down: `esp_netif_dhcps_start()` on an already-running
server returns `ESP_OK` without printing its banner, which is why the banner count
(1) disagreed with the open count (7). The disagreement is the only honest signal
in the log. Two rules follow, and both are the same rule as the root-cause lesson:
never discard the response body of a call you are treating as evidence, and count
the effect at the far end, not the call at the near end.


| 5 | `feat(orb-manager): browse _orb._tcp and list candidates without dialing` — done; the card lists what announced itself, and dials none of it | 3, 4 |

Item 5 shipped `internal/orbmdns` (a hand-parsed RFC 1035/6762 decoder, a
per-interface browser, and the identity checks a candidate must pass), the
`GET /api/orbs` card and its token-guarded `POST /api/orbs/ask`, the advertiser in
`internal/orbfake`, and `tools/bench/browse_mdns.py` as the stranger's browser. The
`/24` sweep and its `!sweep` ban came out with it: an announcement answered beats 254
dials that must each be refused.

What the bench settled, measured rather than reasoned:

- Both sides of the socket had to leave `net.ListenMulticastUDP`. The stdlib sets no
  `SO_REUSEADDR` before bind, so the hub lost `EADDRINUSE` to avahi and
  systemd-resolved on the box the orb is browsed from, and it sets
  `IP_MULTICAST_LOOP=0`, which makes a same-host browse unanswerable by construction —
  the advertiser sends, the browser receives nothing, and the log reports a LAN with no
  orbs. `internal/orbmdns/mcastsock_unix.go` and `internal/orbfake/mdnssock_unix.go`
  set the options before bind through `net.ListenConfig`'s Control hook.
- `IP_MULTICAST_IF` takes the local address in the `ip_mreq` struct's *first* word,
  while `IP_ADD_MEMBERSHIP` reads that same word as the group and the interface from
  the second. One struct, two opposite conventions: two values, one per option, or the
  join is aimed at the source address and no membership is ever added.
- `SO_REUSEPORT` is deliberately not set. It load-balances a multiply-bound group
  port, which would hand each of the browser's own per-interface sockets a different
  slice of one orb's four records and produce an orb with no address.
- With those three decisions the loop closes on one host: `orbfake -advertise` and
  python-zeroconf 0.151.5 agree on PTR, SRV `port=80 server=orb-782618.local`, TXT and
  A on `192.168.2.48`, and the hub's own card listed the double
  (`orb-782618 / 44bd8d782618 / 094fa22 / 192.168.2.34:80, ttl 120, dialable=false`)
  while `refused` stayed empty on a LAN carrying roughly fifteen other mDNS speakers.
- The advertiser's counters separate our defect from a neighbour's traffic, which is
  the only reason the number is readable: `read=22 answered=2 broken=0 neighbours=20`
  on that run. `broken` counts a packet we should have answered and did not;
  `neighbours` counts traffic that was never ours, and a live LAN at `broken=0` is the
  healthy state.
- Two decoder defects surfaced only from bytes off the air, and both are pinned by
  `testdata/mdns-neighbours-2026-10-12.json` (26 packets from five neighbour hosts,
  captured with our own sockets excluded). The name walker moved the cursor to the end
  of the bytes a pointer expanded instead of the end of the pointer field, so the next
  record's TYPE was read from inside a name: Home Assistant's probes came back
  "reserved label form 0x65" and a Chromecast's A/AAAA pair "RDLENGTH 28525 over the
  bound". `decodeTXT` refused RFC 6762's zero-length character-string and RFC 6763's
  flag string, and because a refused record takes its datagram with it, one iPad
  filled the card with thirty refusals. Reverting either fix fails
  `TestNeighbourStackTrafficIsParsedNotRefused`; the orb-level rules (a candidate whose
  TXT carries no mac this instance can have) are untouched and still tested.
- The provisioning AP is now exercised end-to-end off the air-free path:
  `TestAnnounceNamesTheApAddressAndTheProvisioningState` walks the fake's SoftAP-only
  announcement back out (A=192.168.4.1, `state=provisioning`, SRV port 80),
  `TestAnswerToPicksTheAddressOfTheInterfaceTheQuerierUsed` pins the
  querier-subnet-or-STA fallback, and
  `TestProvisioningApRowIsLabelledAndNotDialable` feeds those bytes to the browser and
  the card and requires provisioning set and `dialable=false`.

§5.4's six documentation sites are rewritten in this item, since a browse makes every
one of them a half-truth if they are left alone: `README.md`'s bench-discipline line
and its "no LAN discovery" paragraph, `main.go`'s package comment, the Manage card's
header in `web/app.js`, `watch.go`'s Phase B comment (which keeps its cable-only rule
and now states why the browse does not feed it), and
`internal/orbnet/client.go`'s timeout justification against a sweep that no longer
exists. `TestDiscoveredAddressIsNotDialable` and `internal/orbmdns/imports_test.go`
are what keep the rewrites from becoming a licence: the ban on sweeping stays in the
code as well as in the prose.

Two limits stay in the ledger. The AP half of item 4 is still unobserved on air from
this bench — these tests prove the fake emits it and the browser reads it, not that a
real orb does. And a hub-side `Ask` cannot be shown to originate multicast here: the
card's `askError` reports it, and rows that appear unasked are marked
`solicited=false` rather than passed off as answers to our question.

| 6 | `feat(orb-manager): name, remember and cross-lock the fleet` — done; `{mac, name, room}` keyed on the chip, liveness from announcements, and a dial the fleet can refuse | 1, 3, 5 |

Item 6 shipped `internal/orbfleet` (the MAC-keyed store and its rules), `fleet.go`
(the cross-lock the proxy consults, the browse fold, `GET /api/fleet` and the two
token-guarded writes), the MAC-carrying `POST /api/orb/allow`, and state file version 2
alongside the `state.go` §5.4 rewrite. Three decisions are the item, and each is tested
where it bites:

- **Identity is the chip, so a rebind keeps the name.** Observing `44bd8d782618` at a
  second address updates the one row it already had; the name, the room and the cable
  flag ride along. Keying on the address was the version that orphaned a named orb the
  moment its lease moved and handed the orphaned name to the next board.
- **The cross-lock refuses and sometimes revokes.** A granted address whose current
  announcer is a different MAC is refused *and* revoked, so the next attempt is a fresh
  decision. Two live claimants are refused without revoking: a DHCP handover and an
  impersonation are indistinguishable at that instant, the old lease stops announcing
  within a TTL, and destroying a commitment every time a router moved a lease is how a
  safety rule gets switched off. If the committed board is not among the claimants,
  that is not a handover and the grant goes.
- **Liveness comes from announcements, with two hard edges.** The browse tick renews a
  bound grant so an orb in daily use is never cut off mid-afternoon. Only the tick may:
  `TestReadingTheCardDoesNotExtendAGrant` asserts a `GET /api/orbs` or `GET /api/fleet`
  leaves the timestamp alone, because a poll a page controls is not a liveness signal.
  And a grant aged past `ipGrantTTL` is deleted and cannot be revived by its own
  announcement — an mDNS MAC is broadcast unsigned, so treating a repeat of it as proof
  would let whoever seized the address restore a dial a human had already let expire.
  `grantCeiling` (30 days from the first commitment) bounds even a continuously renewed
  grant, which is what §5.3's "the token is what the hub keeps" will hang its permanent
  device token on.

Cable outranks wire, per §5.1's correlation rule: `MarkCable` is called from the
bootloader probe (`identity.go`, whose `info.MAC` is the serial descriptor the host can
read) and from the flash's eFuse read (`flash.go`, beside `setBoot`), and a cable-read
row's address cannot be moved by an announcement, only refused with a note saying why.
The watch path's confirm-the-IP step now calls `learnAs(ip, st.MAC)` rather than
`learn(ip)`, so the common case produces a bound grant rather than an unbound one.
`orbmdns.CanonicalMAC` is the single canonicalizer, which retires §5.2's complaint that
`macNorm` cannot collapse a separator-free MAC or refuse an empty one: the fleet never
calls `macNorm`, and two empty MACs cannot meet here.

Twenty mutations, each reverted from an in-memory copy and the whole set checksummed
afterwards, because an earlier harness run that died before its restore step left the
cross-lock deleted and the suite green: deleting the claim lookup, either revoke, the
`renew` MAC check, the staleness check, either persistence write, either route guard, or
the card's use of `mayDial` each turns the suite red, as does each store rule (the name
cross-lock, naming an unseen MAC, the cable privilege, the instance derivation,
liveness). Two mutations initially "survived" as compile errors rather than failures
and were rewritten to stay compilable; the last real gap they exposed was that nothing
pinned one board's Forget leaving another board's grant alive, and
`TestForgettingOneBoardLeavesTheOthersGrantAlone` closes it.

What is NOT verified: the credential does not exist yet, so the file holds no secret
and `state.go` says so precisely rather than pre-emptively — item 9 rewrites that
paragraph when it hands the hub a permanent token. And the contention and rebind
semantics are tested against the double's announcements, as every browse case here is;
the real orb's second radio is the same unobserved surface item 4 left open.
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
against a "subnet sweep" that no longer exists. Item 6 rewrote the `state.go` header rather than
pretending it had something to confess: the file now persists addresses, their MAC
bindings, and the operator's names and rooms, and the paragraph says exactly that and
still says no credential is stored, because that remains true until item 9's permanent
device token exists. It names the thing that must never appear there — the Wi-Fi
passphrase, which the orb owns (`orb_net.c:1819-1820`) — and states what item 9 must do
when it puts a real secret in the file: same 0600, never logged, never echoed to a page,
never copied into a bundle. Writing the stronger promise early would have made the
comment false, which is the failure mode this section exists to catch.
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

## FIX (2026-10-13) — provisioning join failures were silently discarded

**Symptom, reproduced on the bench.** A forced provisioning boot accepted a
`PROV` line with the real SSID and a wrong passphrase. The radio answered
`reason 202` and the device then did nothing at all: no log line past the
refusal, no failure counter, no retry, no rescue, no timeout. The AP kept
broadcasting with nothing behind it, `/api/v1/status` stayed frozen at
`state=provisioning`, and the session was unrecoverable without a reflash.

**Root cause.** `WIFI_EVENT_STA_DISCONNECTED` opened with
`if (s_provisioning) break;`, commented "AP-only mode: nothing to retry". The
comment is false on the APSTA boot path — that path is APSTA precisely so the
station can associate. The gate confused two different states:

| | Provisioning, nothing submitted | Provisioning, join submitted |
|---|---|---|
| Old gate | skip — correct | **skip — wrong: this is the user's feedback** |
| New gate | skip | run the ladder |

**Fix.** New pure predicate `prov_ap_idle(provisioning, joining)`
(`provisioning && !joining`) now gates the disconnect handler and feeds the third
argument of both `connect_attempt_is_moot` and `lease_watch_should_drop`. The
second gate was redundant but real: it would have swallowed the retry even after
line 1260 was fixed, so the log line would have appeared and the device still
would never have retried. Signatures unchanged. All three call sites were on the
`provisioning`-keyed path; `s_joining` is set by `NET_CMD_JOIN` and cleared on
lease, so an ordinary boot behaves exactly as before.

**Evidence.** Attempts 1 through 4 observed at 5/10/20 s backoff, matching
`retry_delay_s` exactly; zero `Guru`, zero `alloc eb`; the rescue arm correctly
did NOT fire (gated on `!s_ap_up`, AP already open — no second AP, no misleading
"credentials not working" provenance). Ordinary boot re-verified after the
change: `state connected`, `ap false`, `assoc_fails 0`, RSSI -62. Host suite
green, including the four new `prov_ap_idle` rows — `prov=1 joining=1 -> runs`
is the previously unreachable row.

**Test method note worth keeping.** The test overwrites the stored passphrase,
because the serial PROV path persists credentials before any lease is held.
`esptool read_flash` of `storage` (0xD60000, 0x6000) before the run and
`write_flash` after makes it reversible; that is what turned a device-bricking
bench test into a routine one. The underlying write-before-verify hazard is a
design item, not a test-harness item, and stays open.

**Still open after this commit.** The provisioning sojourn remains unbounded (no
`s_ap_timer` armed on that boot), and the credential write still precedes the
lease. Both belong in the spec. This commit only removes the blocker that made
the feature unsafe to design around.

## SPEC — reboot-to-provisioning, buildable form (2026-10-13)

Resolves design points 1, 2, 3, 7 and 8 from the note above. Unblocks the moment
the failure-path fix lands (done: `a2765b9`). Five decisions, in build order.

**D1. Provisioning is a boot-time mode. One entry path, the boot branch.**
Late promotion stays in the tree for the 8-failure rescue only. It is no longer a
door a human is supposed to rely on. This collapses points 2, 3 and 8: the flag
clears by booting, no AP-close endpoint is needed, and every manual door is
identical to the boot branch — the only path ever proven joinable by eye.

**D2. The intent flag lives in RTC slow memory, not NVS.**
`RTC_SLOW_MEM` survives `esp_restart()` and is wiped by a power cycle. That is
precisely the semantics point 2 asked for: a request that loses power before the
reboot lands cannot strand the orb in AP mode, because the flag dies with the
power. No expiry counter, no boot-count bookkeeping, no NVS wear, and nothing for
a botched OTA to leave behind. Shape: one `uint32_t` magic in a plain C global carrying `RTC_NOINIT_ATTR`
(`esp_attr.h:89`, section `.rtc_noinit`; present in the S3 linker script, and the
header states it "will keep its value after restart or during a deep sleep / wake
cycle" — which is exactly D2's requirement). Consumed (zeroed) by the boot
branch as it reads it, so one request equals exactly one provisioning boot.
Rejected: an NVS key — it survives power cycles, which turns any lost-write race
into a permanently AP-only device, the failure mode point 2 was trying to avoid.

**D3. Credential write moves to lease time. DONE — `c103fba`.**
Today `prov_reader_task` (serial PROV, write at `orb_net.c:2165`) and
`h_wifi_join` (config page, write at `orb_net.c:2468`) call `nvs_set_str` for ssid and pass BEFORE any association exists. One mistyped
passphrase therefore overwrites a working credential with a broken one, and the
orb is off its network until a human with a phone intervenes. The bounded sojourn
in D4 is worthless if the session it reboots back to has already destroyed the
credentials it would resume with. Required behaviour: the submitted pair is held
in `s_net_q`'s message and a static RAM copy only; `nvs_set_str` runs from
`IP_EVENT_STA_GOT_IP`, exactly once, when the network has proven the pair works.
A failed session then leaves flash untouched and the reboot resumes the old
network. The page must show "verifying" during the window, and must be told
plainly that a refusal leaves the previous network in force.

**As built (`c103fba`, 2026-10-13).** The pair waits in a net-task-owned pending
buffer (`s_pend_ssid`/`s_pend_pass`), set inside `NET_CMD_JOIN` and consumed by
the new `NET_CMD_STORE_CREDS`, which `on_ip()` posts only when
`lease_verifies_creds(pending, join_intent)` holds. `creds_commit()` is the sole
remaining writer of `ssid`/`pass`, so the invariant is one function, four call
sites fewer than the draft assumed. Two departures from the draft, both
deliberate: the timezone still writes at submit time (validated in full, cannot
lock anyone out, fixable from the LAN), and the no-queue fallback keeps
write-first, because with no radio task nothing exists to defer to and a lost
credential beats a lost join.

Bench proof, the exact sequence that cost the device earlier today: wrong
passphrase submitted over the config page, `storage` (0xD60000) dumped after,
md5 identical to the pre-test dump, and the orb rejoined its real network 5.2 s
after the next boot. Pinned by seven new host rows (`lease_verifies_creds` four,
`creds_erase_key` three, including NULL and empty-string).

**Consequence for D4.** This is what makes the bounded sojourn safe: rebooting
out of a failed recovery session now resumes a credential the session could not
destroy. D4 remains the last item that can strand a device, and it is now
downgrade-free to build.

**D4. Bounded sojourn, and the reboot-loop hazard that comes with it. DONE —
bench-proved this session.**
A provisioning boot that carries credentials arms the existing `s_ap_timer`
(`NET_CMD_AP_EXPIRED` already owns the expiry path). On expiry: `esp_restart()`,
never `ap_close()` — D1 explains why closing leaves a half state.
**The guard:** arm the timer ONLY when stored credentials exist. A factory orb with
no SSID that reboots on expiry returns to the same provisioning branch, opens the
AP, re-arms, and reboots forever. That device must keep the current indefinite
AP, exactly as it does today. The condition is therefore
`sojourn_timer_armed = s_provisioning && credentials_in_flash`, and the two boot
paths (unprovisioned / re-provisioning) stay observably different.
Sojourn length: 10 minutes, Kconfig `CONFIG_ORB_NET_PROV_SOJOURN_S`, default 600.
Long enough to fumble a passphrase twice on a phone; short enough that a
forgotten orb rejoins its network on its own before an owner notices it is away.

**How it landed (two pure predicates, one latch, one action).** `sojourn_arms()
= recovery_session && sojourn_s > 0` and `sojourn_reboots() = recovery_session &&
!sta_up` sit next to `prov_ap_idle` and are pinned by 8 new `host_tz_test.py` rows.
`s_prov_recovery_session` latches once at the boot branch. The expiry action runs
in the existing `NET_CMD_AP_EXPIRED` case ahead of the plain re-arm, calls
`orb_svc_note_clean_reboot()` before `esp_restart()` (same courtesy as
`POST /api/v1/reboot` — a planned exit must not bill the next boot a crash), and
writes nothing on the way out (D3 keeps the failure clean).

**The placement bug the bench caught, and the table could not.** The arm first sat
in the provisioning branch beside the latch, and that branch runs BEFORE
`esp_timer_create()` gives `s_ap_timer` a handle and before the queue the expiry
posts to exists. Live boot logged `prov sojourn timer does not exist` and ran the
whole session with no deadline — the feature would have shipped inert, invisible to
every truth-table row because those pin the predicate, not the call site. The arm
moved to after the timer and queue exist (post-`stage_probe("post-nettask")`).
Lesson: a pure predicate passing a table proves the DECISION, never that the CALL
SITE is reachable. Bench 2026-10-13.

**Bench evidence.** (1) Forced branch with credentials, sojourn 45 s: armed, retried
auth-fail at 5/10/20 s, then at T+46.3 logged `sojourn expired` and rebooted on
`rst:0xc (RTC_SW_CPU_RST)` — the intended exit. (2) Factory boot (storage erased,
same 45 s config): no `RECOVERY` line, no arm, no reboot across a 72 s window — the
loop the guard prevents. (3) Ship build (creds present, sojourn 600): takes the STA
branch, no `RECOVERY` line, sojourn inert, settles with a lease at 192.168.2.34.
Storage payload intact throughout (GTROUTER2 present; the boot auto-retry lease
wrote nothing, per D3).

**D5. Physical trigger is net-new code.**
Nothing samples the knob before `orb_net_start()` (`main.c:201`). Add a sample
after `knob_init()` (`main.c:151`): press held at boot for >= 3 s sets the D2 flag
and reboots, which then enters provisioning by the normal route. This is the only
trigger that works when the network is gone and no phone is on the LAN, and the
orb is wall-powered with the knob behind a closed case — so implement it, then
decide whether the case needs a hole. Do not describe it as existing until it is.

**Build order.** (1) D3, credential write at lease — **done, `c103fba`**.
(2) D4 sojourn +
credential-present guard — **done, bench-proved this session**. (3) D2 RTC flag —
**next**; the sojourn's D4 expiry path and D5's knob both set the flag D2 defines.
(4) boot-branch check of the flag (the one-line condition the bench build already
proved, and the same hook D4's guard keys off: force the provisioning branch while
`creds_in_flash` is true, and the sojourn arms). (5) D5 knob sample.
(6) hub-side intent state. Ship nothing before (1).

**Not covered here, unchanged by this feature:** the 8-failure rescue stays a late
promotion; `lv_*` in `/api/v1/status` stays open; the fleet's untracked
`sdkconfig` overrides stay the top OTA risk.

