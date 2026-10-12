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
   plus `sha256sums.txt`. OUTSTANDING.
3. Implement `usbclass_darwin.go` via `ioreg` so macOS can attribute `303a:1001`
   and Identify stops refusing. OUTSTANDING. Untested on hardware until a Mac is in
   front of it; `crosscheck` proves only that it compiles.

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

## 3. Deliberately NOT changed

- The safety model stays "you own the cable". No `.orb-target` binding and no
  MAC gate return, per README "Safety model". Iteration 2 restricts who may ask,
  not which orb you may act on.
- No subnet sweep, no ARP, no mDNS. Every network call stays a dial to one
  address the hub learned or was handed.
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
- Iteration 6 (outstanding): `make release` (four platforms + `sha256sums.txt`) and
  `usbclass_darwin.go`. Neither was started.
- Hardware: nothing in iterations 0-5 was run against a cable. The cancel path and
  the console read both drive real ROM/CDC timing and need one live run on orb-B
  before either is called verified. The theme card was verified against
  `cmd/orbfake`, which reproduces `h_sd`'s shapes but is not the firmware.

