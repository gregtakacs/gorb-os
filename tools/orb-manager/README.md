# orb-manager

A single static Go binary that onboards, themes, and manages gorb-os orbs on
the LAN. It drives the flasher itself — no esptool, no Python, no stub loader.
The serial library is compiled in, the UI is embedded, and the firmware is
bundled inside. Users install nothing, need no ESP-IDF, and hold no build
directory: one file flashes an orb. The hub opens in its own window rather than
a browser tab.

It lives at `tools/orb-manager/` inside the gorb-os clone and drives the same
native flasher as the bash/Python bench tools (`tools/serflash.sh`), but it
deliberately drops their shared-bench discipline: no `.orb-target` device
binding, no `orb_bind` MAC/IP gate, no ARP table and no subnet sweep. A user owns the
orb on the cable they picked; the hub reaches it over USB, over an address they
give it, or over the address the orb announced itself.

## Why no esptool

esptool is a Python program. Shipping it means shipping an interpreter, a
pip environment, and the USB-permission dance for `idf.py`/`esptool`, which is
exactly the setup burden a "portable single binary" exists to remove. This
flasher talks the ROM bootloader protocol directly (`internal/esprom/`): SLIP
framing, the command codec, `USB_JTAG_SERIAL` download-mode entry, and the
`FLASH_BEGIN`/`FLASH_DATA` writes. The ROM-only path is deliberate — it drops
the version-2 plugin-stub loader, the largest chunk of protocol and the biggest
risk surface, and the USB-CDC link is the throughput floor anyway, so the stub's
baud tricks buy nothing here.

## Safety model

This is a production tool: it flashes and provisions **whatever orb you point it
at**. Pick a port from the picker and it acts on that port. It never refuses a
device for "not being the bound orb" — that was a dev-bench idea (one operator,
two orbs on one desk) and it has no business in front of a user who owns the one
orb on their own cable. What stands between the UI and the wire are safeguards
that protect the *operation*, not the operator's identity:

1. **One writer per port.** An exclusive lock per physical port
   (`LockPort` in `internal/orbbind/`, keyed to the canonical device path so a
   `/dev/ttyACMx` and its `/dev/serial/by-id/...` link can't each sneak past the
   other). A second flash or provision on the same cable is refused instead of
   corrupting the write mid-stream. In-process, the operation registry
   (`GET /api/ops`, `POST /api/flash/cancel`) makes the same claim per port and
   carries the cancel handle; it replaced a process-wide `flashing` flag that
   could not tell two cables apart, so an idle orb used to queue behind an
   unrelated one. The registry holds its entry for the whole operation, not for a
   flag update, and releases it even when the handler returns early.

2. **The local HTTP surface is authenticated, and the proxy is a gate, not a
   tunnel.** Binding to `127.0.0.1` is not a defence by itself: any page the browser
   visits can POST to `127.0.0.1:3779` without a CORS grant, because a cross-origin
   POST only needs permission to be *sent*. So every route that changes something —
   flash, cancel, provision, identify, firmware pick/reset, watch, and the whole
   `/orb/` device proxy, half of which writes to the orb — requires two things:

   - **This launch's token**, in the `X-Orb-Token` header or the `orb_token` cookie.
     `main()` mints 16 random bytes once per process; `handleIndex` writes it into
     the page it serves and sets the cookie `HttpOnly; SameSite=Strict`. A page that
     never loaded from this hub cannot produce it, and a cross-site form POST cannot
     carry the cookie.
   - **A same-origin check.** `Sec-Fetch-Site` must be `same-origin`/`same-site`, and
     any `Origin` or `Referer` presented must match the host asked for. A client that
     sends neither (curl, an agent) is not exempt from the token: that is the scripted
     path, and it passes `-H "X-Orb-Token: …"` after fetching `/` once.

   A refusal answers in the `{ok:false,error}` envelope every caller already parses,
   and names the reason. `/api/flash` and the rest are refused **before** the port
   claim, so a rejected request never reserves a cable.

   `/orb/<ip>/…` carries four further gates, because it is the hub's only route off
   the box: the address must be an **IP literal this hub learned** over USB or was
   handed by `POST /api/orb/allow` when the operator committed to it in the Manage
   card; the **fleet must not contradict that** — see below; the path and method must
   be in the **device route allowlist**, which mirrors the URI table in
   `main/net/orb_net.c` and `main/theme_app.c`; and the exchange is **byte-capped**
   (8 MiB down, 4 MiB up). The device's own `/api/v1/ota` is
   deliberately **not** in that allowlist: the hub's rule is that firmware comes from
   a validated bundle and the whole manifest is always written, so the bare-app path
   is left out of the browser's reach. The port is never taken from the URL, which is
   what keeps a learned address from becoming a port-scanning target.

   The fleet gate is the one item 6 added, and it can only ever **refuse** a dial the
   grant set allowed. The hub remembers each board by `{mac, name, room}`, keyed on the
   chip and never on the lease, because an address is something an orb has at a moment
   and a MAC is something an orb is. So a grant may name the board it was committed
   for, and before dialing the hub asks the fleet who announces that address today:

   - one board, the same board the grant named: dial.
   - one board, a **different** MAC than the grant named: refuse **and revoke**, so the
     next attempt is a fresh decision by a person rather than a retry of an old one.
   - **two** live boards claim it: refuse. A DHCP handover and an impersonation look
     identical in that instant, so the hub declines to pick a winner and keeps the
     commitment until the contention ages out. If the board the grant named is not
     among the claimants at all, that is not a handover and the grant goes.
   - **nobody** announces it: dial on the operator's word alone. That is the typed
     address and the wired-bench case; refusing it would make the hub unusable on a LAN
     it cannot browse, and the card labels such a grant `unverified` instead of
     pretending the hub checked it.

   Liveness comes from announcements: the browse tick renews a grant whose board is
   still announcing, so an orb in daily use is never cut off mid-afternoon. Only the
   tick may renew — `GET /api/orbs`, `GET /api/fleet` and the pushed
   `GET /api/orbs/stream` cannot, or any same-origin page
   could hold a dial open by polling. A grant that went quiet past `ipGrantTTL` is gone
   and **no announcement brings it back**: an mDNS MAC is broadcast unsigned, so
   repeating one must not resurrect a permission a person already let expire. A separate
   `grantCeiling` (30 days from the first commitment) bounds even a continuously
   renewed grant, so a permanent dial still needs a human now and then.

3. **Binding off loopback is a decision someone has to type.** `-addr` accepting a
   LAN address used to be enough on its own; now `-allow-remote` is also required,
   and the exposure is logged. The token still guards the mutating routes either way.

The hub never sweeps a subnet. It never reads the ARP/neighbour table, never
shells out to `arp`/`arping`, and never dials a range hoping one address answers.
An orb's address comes from one of three places: what the hub already read over
its USB console — the lease the provisioning door captured, or the association
banner the flash logged — an IP the operator types, or the orb's own mDNS
announcement, which `internal/orbmdns` listens for on `_orb._tcp.local`. The
announcement fills the Find card's list and nothing else. A listed address is not
reachable: the proxy dials only addresses the operator committed to, and the card
says so per row (`dialable` reads the allowlist, it does not request an entry).
Every network call the hub makes is a dial to one address it was handed, through
the same-origin proxy.

For an operator who wants an extra identity check before a headless flash,
`orbflash -mac aa:bb:...` reads the chip's eFuse MAC and refuses on mismatch.
The web tool does not impose it; you act on the port you picked.


## Run it

```
make build                       # -> bin/orb-manager, bin/orbflash
./bin/orb-manager                # opens the hub in its own app window
```

That is the whole procedure. The firmware is compiled into the binary, so the
hub needs no ESP-IDF, no `build/` directory, no `git`, and no clone to flash an
orb. It reads one file: the firmware bundle it ships with.

To flash a different firmware — a downgrade, or a build newer than this binary —
click **Use my own firmware…** on the Onboard card and pick a `*.zip` bundle.
**Use built-in firmware** returns to the shipped one. `--firmware <bundle.zip>`
starts on one from the command line.

A bundle is a zip of `flasher_args.json` plus the images it names, under
`images/`. A picked bundle is a file from wherever the operator's browser was
last pointing, so `firmware.Load` treats it as untrusted input and refuses it
before anything reaches a port:

- **Size, in two places.** The compressed archive is capped at 32 MB, and the
  DECOMPRESSED total at 48 MB. The second number is the one a zip bomb is built
  out of: capping each entry at the archive limit was not equivalent, because the
  entries are summed into a map, so an archive of many high-ratio members expanded
  to entries x limit and the hub held all of it.
- **Member count**, capped at 32. A release is the manifest plus five images.
- **Project identity.** The `esp_app_desc_t` inside the app image announces its
  project, and a bundle announcing anything but `gorb-os` is refused — the usual
  cause being a stray ESP-IDF build picked out of a Downloads folder. An image with
  no descriptor at all is still accepted, and the card says "unknown firmware"
  rather than inventing a version.
- **Descriptor hygiene.** `project`, `version`, `idf` and the build stamp are bytes
  the file controls and they are rendered in the browser, so non-printable
  characters are dropped where they are parsed. The client escapes them again:
  `refreshBuild` renders every server string through `esc()`, and
  `TestFirmwarePanelEscapesEveryServerString` walks the template and fails on any
  interpolation of a server-supplied field that skips it — because the original bug
  was exactly one field (`who`, built from project and version) interpolated raw
  while its neighbours were escaped.

`make firmware` produces one from a build dir on the machine that has
the toolchain:

```
idf.py build && make firmware    # repacks internal/firmware/bundle/orb-firmware.zip
```

It is committed, which is what lets a plain `make build` work without ESP-IDF.
Run it whenever the firmware changes. `TestBuiltInBundleMatchesBuildDirWhenPresent`
fails if the committed bundle has gone stale against `ORB_BUILD_DIR`, and
`make test` sets that variable itself whenever `build/flasher_args.json` is
present — so a stale bundle now fails the ordinary suite instead of skipping.

Offsets are still never hardcoded: the manifest travels inside the bundle beside
the exact bytes it describes, so a plan cannot mix one build's offsets with
another build's images.

By default the hub opens in a **chromeless Chromium window** (`window.go`) — no
tabs, no address bar, taskbar entry named "Orb Manager" — instead of a browser
tab. It finds an installed Chrome, Chromium, Edge, or Brave and launches it with
`--app`; under WSL it also accepts the Windows-side `chrome.exe` and converts the
profile path through the Windows shell, so the window lands on your desktop. The
browser is never bundled: gorb-os ships a static binary you install nothing to
run, and it stays `CGO_ENABLED=0` because Chromium does the rendering. Closing
the window shuts the hub down, so no listener is left behind. That shutdown
follows the real window, not the process we launched: Chrome is single-instance
per profile and hands the URL to an already-running browser — the usual outcome
for `chrome.exe` under WSL — so the hub watches its own page in that browser's
DevTools target list and exits when the page goes. When no window can be
watched at all, the hub keeps serving rather than shutting down on a guess, and
says how to stop it.

The browser's own chatter stays out of your terminal. A `chrome.exe` started from
WSL prints dozens of unrelated named-pipe and GCM errors at startup. They go to
`orb-manager-chrome.log` in the temp dir, and the hub's startup line names that
path.

Three switches cover the exceptions:

| Flag | Effect |
|---|---|
| `--no-window` | serve only and print the URL — the bench, SSH, and CI path |
| `--browser=browser` | old behaviour: a normal tab in the default browser |
| `--browser=/path/to/chrome` | name the binary instead of autodetecting |
| `--firmware=<bundle.zip>` | flash a firmware bundle instead of the one built into the binary |
| `--repo=<clone root>` | serve the Theme Editor from the live clone instead of the copy built in |
| `--addr=host:port` | bind somewhere other than `127.0.0.1:3779`; needs `--allow-remote` |
| `--allow-remote` | permit `--addr` to name a non-loopback host, which the hub otherwise refuses at startup |

Chromium only, deliberately: the Theme Editor is a Blink canvas app whose file
dialogs and `localStorage` asset store are exercised in Chrome, so the hub is
hosted in the same engine and `/composer/` opens inside that window. No Chromium
on the host, or an unreadable profile directory, falls back to the default
browser and the hub still works as a plain web app.

The three hub cards:

- **Onboard** — pick a serial port, click **Identify this orb**, then **Flash
  orb**. The hub always writes the whole `flasher_args.json` manifest: which halves
  to write is bench knowledge, and the app-only reflash lives on in
  `orbflash -mode app`. A successful flash runs the post-flash onboarding probe; the
  boot verdict the flash captured picks the branch (AP card vs. connected panel).

### Identifying an orb

**Identify this orb** answers the two questions that make two cables
distinguishable: which orb is this, and what is it running.

```
/dev/ttyACM1: gorb-os 8eb4bf0-dirty
  mac        44:bd:8d:78:26:18
  built      Oct 12 2026 01:58:41
  elf sha256 d5fd8bb9d
  idf        v5.4
  lan        192.168.2.34
```

Two design facts worth knowing, both measured against live hardware rather than
assumed:

- **The ROM will not give up the version.** `MEM_READ` and `SPI_READ` (op
  `0x10`) both answer `[1,5]` on an ESP32-S3 without a RAM stub, so flash bytes —
  and therefore the running app's `esp_app_desc_t` — cannot be read over the
  ROM. The 2nd-stage bootloader prints that same descriptor on the console
  roughly 0.7 s after a cold boot, which is where the answer comes from.
- **So a probe costs a reboot.** The orb restarts, its screen blanks for a second
  or two, and it re-joins Wi-Fi. Nothing here is automatic: the hub probes only on
  that click, one port at a time, caches the answer, and takes the same per-port
  lock a flash does. The identity is invalidated by a flash of that port, so a
  re-probe reports the new firmware rather than repeating the old.
  The cache is keyed on the **cable**, not the tty name: an answer records the USB
  serial the host could see behind the port when the probe ran, and it is only
  handed back while that serial is still what is in the socket. Unplug orb A from
  `/dev/ttyACM0`, plug in orb B, and the picker says "not identified" instead of
  going on asserting A's version — which matters because that label is a claim
  about the firmware an operator is about to overwrite. An answer also expires
  (`identityTTL`) and is dropped when its port disappears from the enumeration, so
  an orb that OTA'd or was reflashed elsewhere is not described forever by a reboot
  this hub paid hours ago. Boot verdicts are keyed by MAC for the same reason: two
  orbs flashed in a row each keep their own, where a single slot lost the first.

The MAC beside an *unprobed* port is free, by contrast: it comes from the USB
serial-number descriptor via sysfs (Linux) or SetupAPI (Windows), which for this
peripheral is the chip MAC. No port is opened and no control line is touched.

Only devices the host can attribute to the Espressif USB-JTAG peripheral
(`303a:1001`) may be probed. `EnterDownloadMode` toggles DTR/RTS, which would
reset an Arduino or an FTDI breakout sharing the bench, so a port whose device the
OS cannot name is refused with an explanation rather than reset on a guess.

### Newer or older

The card compares the firmware ready to flash against what the orb runs and says
`upgrade`, `downgrade`, `same`, or `same-version`, colouring a downgrade so it
cannot be mistaken for an upgrade. A git describe like `8eb4bf0-dirty` carries no
ordering, so the decision uses the two facts that do: the **build timestamp** for
order and the **ELF digest** for identity. Equal describes with different
timestamps report `same-version` — a rebuild is not a release — and anything
missing a usable stamp reports that it cannot tell, rather than pushing a user to
replace a working orb on no evidence. Uploading a different bundle re-runs the
comparison against every cached identity without probing again.
- **Theme Editor** — the watch-composer at `/composer/`. With a gorb-os clone
  reachable it is served from `tools/watch-composer`, so edits to the composer
  appear on reload with no rebuild; with no clone, as on every end user's machine,
  the copy compiled into the binary is served instead (`make composer` refreshes
  it, and `TestEmbeddedComposerMatchesSource` fails if it drifts). The card is a
  plain link into it; the hub adds a "← Orb Manager" link to get back (an app
  window has no back button) and, when React never arrives from the CDN, a line
  saying so rather than a blank page. Both are added at serve time; neither copy
  of the composer file is ever modified.
  The editor needs React, ReactDOM and Babel and 36 font families, and it normally
  reaches for them at `unpkg.com` and `fonts.googleapis.com`. `make vendor-composer`
  commits local copies under `web/composer/vendor/` — with them compiled in, the hub
  rewrites those references at serve time and the editor opens with no network at
  all. The vendored `support.js` is never edited: it reads a `window.__resources`
  URL→local map the hub injects ahead of it, which is the hook it was built with.
  Each vendored file carries a sha256 and the SRI the composer demands, recorded in
  `MANIFEST.json`; `make vendor-check` re-verifies both and runs as part of
  `make check`, so a hand-edited React fails the build instead of blanking the page.
- **Manage** — point at one orb by the IP provisioning reported (or one you look
  up on your router) and it dials that single address through the same-origin
  proxy to show a live status panel. Reboot, timezone, and Wi-Fi actions go
  through that same proxy. No subnet scan, no ARP table.
  - **Theme card.** List the orb's SD themes, `Use` one, restore the built-in
    face, `Re-read card`, delete one (it asks first — the firmware's delete is a
    plain unlink and there is no undo from here), and push a `.orbtheme` exported
    from the Theme Editor straight to the orb. The hub never writes that file to
    its own disk. Every one of these is a call to
    `internal/orbnet/themes.go`, the single module that dials the device theme
    API. A theme's identity is the `name` baked inside its container; the orb
    files it under `theme_id(name)` = `sha256(name)[:12]`, and that 12-hex `id`
    is the only handle `install` and `delete` accept, because the device has no
    route that takes a name. The card shows the name and sends the id. There is
    no name box on an upload: the name is already in the file, and the hub reads
    it out of the header to tell you where the theme will land.
  - **USB console (read-only).** Opens the port, forwards what the running app
    prints for a bounded window, closes. Nothing is written to the wire and the
    orb is not reset — this is `tools/cap.py` for someone with no Python and no
    clone. It registers as a port operation, so it refuses to share a cable with
    a flash, appears in `/api/ops`, and is cancellable.
  - **Remembered fleet.** The LAN card names each board by its MAC: type a name and a
    room and press **Remember**, and the label follows that chip across every DHCP
    lease, reboot and re-flash. Two live boards may not answer to one name — the hub
    refuses the second and says which MAC already holds it — because an orb the operator
    cannot name apart is an orb a later credential would be pointed at the wrong device
    for. The instance name is derived from the low three bytes of the MAC, so two boards
    really can derive the same one; such rows are marked `shared` rather than left to
    look identical. **Forget** drops the board and revokes the address it held, unless
    another live board still claims that address.
  - **Remembered orb.** The address the hub last had an orb at — learned over
    USB or typed in — comes back on the next launch, so the address is typed once
    rather than every session, along with the MAC it was committed for and the
    fleet's names and rooms. It lives in `state.json` under the OS config dir
    (mode 0600, beside the app-window profile) and holds **addresses, MACs, names and
    rooms**: no launch token, no Wi-Fi passphrase, no SSID, because the orb owns the
    passphrase (`orb_net.c` keeps it in NVS) and a second copy in a config dir is a
    second place it leaks. A corrupt file reads as "nothing remembered", never as a
    dial list; only IPv4 literals are accepted into it, so a hostname in that file
    cannot make the hub resolve something, and a version 1 file still loads (its
    grants come back unbound, labelled as such).

## Headless flasher

`orbflash` drives the identical `esprom.Session` without the web UI, for scripted
bench use. It flashes the same embedded bundle, so it needs no build dir either:

```
make orbflash
./bin/orbflash -mode full -port /dev/ttyACM0                    # whole manifest
./bin/orbflash -mode app  -port /dev/ttyACM0 -mac 44:bd:8d:60:80:88
./bin/orbflash -mode full -port /dev/ttyACM0 -firmware ./some-bundle.zip
```

The default `-mode probe` only enters download mode, syncs, reads the chip id and
eFuse MAC, then hard-resets back to the app. `-mac` applies the eFuse gate even
headless; a mismatch aborts before erasing.

## Endpoints

| Route | Purpose |
|---|---|
| `GET  /api/status` | platform, hub stamp, firmware version and source |
| `GET  /api/ports`  | ports as `{"ports":[{port,attributed,hint,usbMac?,probed,...}],"count":N}`, with each cached identity inlined |
| `POST /api/ports/probe` | identify one named port — reboots the orb; explicit, never implicit |
| `GET  /api/build`  | the active firmware: source, version, sha256, and its resolved image list |
| `POST /api/firmware` | upload a bundle (multipart field `firmware`); validates it, then makes it active |
| `POST /api/firmware/reset` | return to the firmware compiled into the binary |
| `POST /api/flash`  | flash the selected port, streamed as newline-delimited JSON events |
| `POST /api/flash/cancel` | `{"port"}` — ask the flash on that port to stop. It ends at the next 1 KB block boundary, never mid-block, and leaves the orb in its bootloader rather than running a partial image |
| `GET  /api/ops`    | the port operations live now: `{"ops":[{port,asked,kind,started,seconds}]}`, where kind is `flash`, `provision` or `identify` |
| `POST /api/watch`  | post-flash onboarding probe: boot verdict, then confirm the USB-learned IP, NDJSON |
| `POST /api/provision` | wired (serial) provisioning round-trip: boot scan + one PROV line + PROV-ACK, NDJSON |
| `GET/POST /orb/<ip>/api/v1/...` | same-origin proxy to an orb's device API, gated on a learned address and the device route allowlist |
| `POST /api/orb/allow` | `{"ip", "mac"?}` — register the address the operator just committed to, which is what lets `/orb/` dial it; the optional MAC is the board the grant is then cross-locked to |
| `GET  /api/fleet` | the remembered fleet — `{mac, name, room}`, cable and contention state, and the learned addresses with whether each is bound |
| `POST /api/fleet/set` | `{"mac", "name", "room"}` — name a board the hub has actually met; token-guarded, and refuses a second live claim on a taken name |
| `POST /api/fleet/forget` | `{"mac"}` — drop a board and revoke the address it held; token-guarded |
| `POST /api/orb/forget` | `{"ip"}` — drop an address from the dialable set without restarting the hub |
| `GET  /api/orb/known` | the remembered address, the learned set, and where the state file lives |
| `POST /api/theme/list` | `{"ip"}` — the orb's SD themes plus card state, mount, active face, and the last refused install. Each row carries the `name` and the `id` the orb derived from it |
| `POST /api/theme/install` | `{"ip","id"}` — queue an install by the 12-hex id `list` returned; the device reboots itself when the copy verifies |
| `POST /api/theme/default` | `{"ip"}` — restore the built-in face |
| `POST /api/theme/delete` | `{"ip","id"}` — remove a theme from the card by id (the UI confirms first) |
| `POST /api/theme/rescan` | `{"ip"}` — re-read the card without rebooting |
| `POST /api/theme/upload` | `?ip[&overwrite=1]`, body = the raw `.orbtheme` — push a theme; never written to the hub's disk. No name: the orb files it under the name inside the container, and `overwrite` is the answer to the 409 it raises when a different theme already holds that name |
| `POST /api/console` | `{"port","secs"}` — NDJSON of what the running app prints; read-only, and a port operation |

Every route above that CHANGES something (`/api/flash`, `/api/flash/cancel`,
`/api/provision`, `/api/watch`, `/api/ports/probe`, `/api/firmware`,
`/api/firmware/reset`, `/api/orb/allow`, and all of `/orb/`) also demands this
launch's token and a same-origin presentation; see Safety model. The read routes
(`/api/status`, `/api/ports`, `/api/build`, `/api/ops`) stay open, because the page
polls them and a GET cannot change an orb.

Every route wraps its payload in `{ok,data}`. The port list sits under
`data.ports`, not as a bare array, because `app.js` destructures that field. The
firmware panel reads `data.present`, `data.images`, `data.source`, `data.version`,
`data.project` and `data.sha256` — `images` as `[{offset,name,bytes}]`, empty
rather than null when the manifest lists nothing, because the UI takes `.length`
on it. A rejected upload answers `{ok:false,error}` and leaves the built-in
firmware active: picking the wrong file cannot disable the Flash button. Two tests in `api_test.go` read
those field names off `app.js` and fail if a handler stops sending them, which is
how the port picker's blank dropdown got through once.

`/api/flash` streams `{"log":...}`, `{"progress":{...}}`, `{"mac":"..."}`
(eFuse MAC, emitted as soon as the ROM answers), `{"done":...}`, and
`{"error":...}` lines; the UI's `streamFlash` parses them into the log and bar.
While a flash runs, the card shows what `/api/ops` says is live on that cable and
reveals a Cancel button that posts `/api/flash/cancel`.

**Nothing here is uncancellable, and no cancellation corrupts a flash.** Every
flash, provision and identify runs under a context seeded from its own HTTP
request, so closing the panel is a cancellation, and `esprom` checks that context
in the packet-read loop and between flash blocks. It never checks *inside* a
block: the ROM takes a whole 1 KB block or rejects it, so stopping at a boundary
leaves a flash the next full flash overwrites cleanly, while stopping mid-block
would leave the ROM waiting for data that never arrives. A cancelled session does
not reset the chip — it stays in the bootloader, and the stream says so instead of
reporting a timeout. Quitting is the same story: closing the app window or pressing
Ctrl-C waits (up to `opQuitBudget`) for live operations to land before releasing
the listener, because `http.Server.Shutdown` waits only 3 s and main returning then
ends the process mid-write, which is exactly how an orb is left half-flashed. A
second Ctrl-C cancels the live operations and quits. Two cables may be busy at
once: the operation registry is keyed by canonical port path, replacing the
process-wide `flashing` flag that used to make an idle orb wait for an unrelated
one. `POST /api/flash/cancel` and `GET /api/ops` are that registry's face.
When the flash holds the port across its own reset it also streams the boot
console as `{"log":"console: ..."}` lines and records a boot verdict keyed by
that MAC: the firmware's one-shot `no stored network — provisioning AP
"orb-XXXX"` banner, or its association banner. This is the only reliable capture
point — USB-Serial-JTAG has no EN wire, so a running app ignores a later RTS
pulse, and IDF drops console output whenever no host holds the port.
`/api/watch` takes the same `{"port","mac"}` pair back and consumes that verdict:
it ends with one `{"result":...}` line — `mode:"ap"` naming the open provisioning
AP a never-provisioned orb raised (and, as `networks`, the boot-scan list the
flash read off the console), `mode:"lan"` with the IP the orb announced on its own
console, or `mode:"no_ip"` when the orb joined but never printed an address the
hub could read. The LAN case dials exactly the one address it learned over USB (or
was handed as `{"ip":...}` after a serial provision) and confirms it answers on
`/api/v1/status`; it never consults the neighbour table or sweeps a range. Pass
`"skip_ap":true` to jump straight to that confirm — used after a serial provision,
where the AP verdict is already spent and the orb is joining.

## Provisioning: two doors

An unprovisioned orb (empty `storage` NVS) opens an open provisioning SoftAP and
waits. Two independent ways lead in; both end in the same firmware
persist-then-join path, and neither erases the shared `storage` partition, so the
`orb.settings` (theme) and `orb.idle` (dim) namespaces survive either door.

1. **The AP/HTTP door (phone).** Join the orb's `orb-XXXX` AP from a phone or a
   laptop on that network and use the on-device config page. The hub can drive
   the same pair — `GET /api/v1/wifi/scan` + `POST /api/v1/wifi/join` — through
   the proxy when it can reach the AP.
2. **The serial door (wired, `/api/provision`).** For a hub that is *only* cabled
   to the orb and has no route to `192.168.4.1`. On first boot the firmware runs
   a scan and logs each result as a `PROVSCAN` console line (surfaced through the
   boot verdict → `/api/watch` → the UI's picker), then blocks on the
   USB-Serial-JTAG console's stdin for one line:
   `PROV <hex-ssid> <hex-pass> <hex-tz>` (`-` = empty/skip; fields are hex so an
   SSID with spaces or punctuation can never be truncated). The line MUST end in
   CRLF, not a bare `\n` — the USB-Serial-JTAG console's cooked-mode `fgets` only
   returns a line on a CR, which is why a terminal's Enter works but an
   LF-only writer silently never gets a `PROV-ACK`. The firmware decodes
   it and writes its **own** `ssid`/`pass`/`tz` NVS keys, then joins — so the
   flasher and the ROM are never involved in the credential write. The UI's
   "Provision over USB" button calls `POST /api/provision {"port","ssid","pass","tz"}`,
   which reboots the orb into its app on the held port, reads the scan, writes the
   PROV line, and confirms the firmware's `PROV-ACK`. It then keeps the console
   open for a few more seconds to catch the firmware's post-DHCP banner
   (`associated: config page at http://<IP>/`), so the orb's LAN address comes
   back over the same USB lead — `/api/provision` returns it as `result.ip` and
   `/api/watch` dials that one address to confirm it, with no ARP or sweep.

Caveat, stated plainly: the serial door sends the passphrase over the wire in hex
— obfuscated, not encrypted. That is acceptable only because it runs over the
user's own USB lead to a physically-adjacent device during onboarding. It is not
a path for a remote or shared link; use the AP/HTTP door there. The serial door is
for an *unprovisioned* orb only — a device that already has credentials boots as a
station, never opens the stdin reader, and must be re-configured through its HTTP page.

The proxy only forwards the `/api/v1/` namespace to a bare IPv4 literal, so it
cannot become an open proxy or a path-traversal foothold.

## Layout

```
main.go              flag parsing, embeds web/, routes, app-window wiring, run log
window.go            app-window mode: Chromium discovery, --app launch, window-lifetime shutdown
window_devtools.go   DevTools page-target watch: shutdown follows the window through Chrome's single-instance hand-off
api.go               the /api/* JSON handlers (status, ports)
firmware.go          the active firmware bundle, its upload/reset handlers, and the Onboard panel payload
identity.go          port identity: the probe cache, /api/ports and /api/ports/probe, the newer/older verdict
flash.go             POST /api/flash: port lock + NDJSON stream + boot verdict (with USB lease)
watch.go             POST /api/watch: boot verdict + confirm the USB-learned orb IP
provision.go         POST /api/provision: wired (serial) provisioning round-trip
theme.go             POST /api/theme/*: the Manage card's theme surface, gated on a learned address and a legal theme id
console.go           POST /api/console: read-only USB console, registered as a port operation
state.go             remembered orbs (state.json under the OS config dir: addresses, their MAC bindings, names, rooms)
fleet.go             the fleet surface: mayDial's cross-lock, the browse fold, GET /api/fleet, POST /api/fleet/{set,forget}
roster.go            the one-list merge: TTL-cached USB sweep behind a seam, MAC-joined LAN+cable rows, plug-change digest
ops.go               the per-port operation registry, /api/ops and /api/flash/cancel
security.go          the launch token, the origin check, the learned-address set, the device route allowlist
proxy.go             same-origin /orb/<ip>/api/v1/... device-API proxy
discover.go          the mDNS listener: candidate list, GET /api/orbs (merged by roster.go),
                     the pushed GET /api/orbs/stream, and the upkeep tick.
                     It listens and lists; a listing never dials and never grants, and
                     internal/orbmdns cannot import an HTTP client to make it otherwise
composer.go          serves the repo's watch-composer at /composer/, and the vendored
                     React/Babel/fonts offline copies at /composer/vendor/
cmd/orbflash/        headless face of the same flasher
cmd/orbvendor/       one-time fetcher: CDN scripts + font subsets -> web/composer/vendor/
                     with a sha256/SRI MANIFEST; -verify checks the committed tree
cmd/serprov/         headless wired-provisioning round-trip tester (bench gate)
internal/esprom/     the native ROM flasher (serial, SLIP, commands, plan, session, boot console, serial-provision)
                     identity.go: ProbeOrb and the boot-log parser; usbclass_*.go: per-OS device attribution
internal/firmware/   the bundle: pack, load, verify, and the embedded orb-firmware.zip it ships with
cmd/orbpack/         release-time packer: build dir + manifest -> the committed bundle
cmd/orbprobe/        headless "what is on this cable", same probe the Identify button runs
internal/orbbind/    per-port exclusive lock (LockPort): flock on Unix, an
                     unshared CreateFile handle on Windows
internal/orbnet/     single-orb status client (no sweep, no ARP), and the device theme API
internal/orbfleet/   who the hub believes is behind the addresses it will dial: the MAC-keyed store, the one-name-one-board cross-lock, cable sightings outranking announcements, and liveness by announcement rather than by wall clock
internal/orbmdns/    the hub's side of the browse: a strict RFC 1035/6762 decoder, one
                     multicast socket per eligible interface, and the identity checks a
                     candidate must pass before it is listed. It imports no HTTP client,
                     no orbnet and no orbfake — imports_test.go parses its own import
                     declarations to keep that true, because a browser that could dial
                     turns a listing into an SSRF pivot. Its socket is built with raw
                     setsockopt, not net.ListenMulticastUDP: the stdlib sets no
                     SO_REUSEADDR before bind (so avahi and systemd-resolved cost it the
                     port) and forces IP_MULTICAST_LOOP off (so a same-host browse can
                     never be answered). It also keeps its own copy of the 12-hex id and
                     instance-derivation rules rather than importing orbnet's.
internal/orbfake/    the theme/console/mDNS device double: the HTTP and SSE half is
                     written from main/net/orb_net.c and the advertiser from
                     main/net/orb_mdns.c, each against that file's bytes rather than
                     against this repo's reader. Both the Go suite and cmd/orbfake run
                     this one copy. Keep it that way: the
                     first double was transcribed from the hub's own client, so it answered
                     ?name=, agreed with the bug, and stayed green while every real orb
                     400-ed an install. It keeps its own copy of the 12-hex id rule and the
                     container read for exactly that reason — drift between the two copies
                     is a failing test, not a shared assumption.
cmd/orbfake/         runs that double as a listening server for a human driving a real hub,
                     and as an orb on the LAN with -advertise: announce, answer queries on
                     udp/5353, goodbye on stop. Its log line splits what it read from what it
                     answered, and separates our own defect from a neighbour's traffic —
                     `read=22 answered=2 broken=0 neighbours=20` is a healthy run on a live
                     LAN, while a non-zero `broken` is a bug here. Judge it from outside with
                     tools/bench/browse_mdns.py, which browses with python-zeroconf rather
                     than with this repo's listener: see "Listening for an orb" under Development.
web/                 the embedded hub UI (index.html, app.css, app.js)
```

## Development

```
make test       # go test ./... — SLIP framing, block checksum, SHA256 recalc,
                # MAC decode, FLASH_BEGIN 5-word framing. Arms the bundle
                # staleness guard automatically when build/flasher_args.json
                # exists, so a committed bundle that has drifted from the tree
                # fails here instead of skipping.
make check      # the whole ladder: test (guard armed) + vet + crosscheck + vendor-check
make vet        # go vet ./...
make fmt        # go fmt ./...
make firmware   # re-pack the embedded firmware bundle from this clone's build dir
make composer   # re-copy the Theme Editor into web/composer/ for the shipped binary
make vendor-composer  # fetch React/Babel + font subsets into web/composer/vendor/
                # (needs internet; idempotent, so a re-run produces no diff)
make vendor-check     # verify that committed tree against its manifest and SRI pins
make run        # rebuild + open the app window against 127.0.0.1:3779
                # add --no-window to serve without a window (SSH, CI, bench)
make flash PORT=/dev/ttyACM0   # headless full flash of that port
make windows    # cross-compile bin/windows-amd64/{orb-manager,orbflash}.exe (console: prints to the terminal)
make windows-gui # the one-window hub exe (PE subsystem 2: double-click shows only the app window)
make win-test   # run the Windows test binaries natively, through WSL interop
make crosscheck # go vet every supported GOOS/GOARCH, tests included
make release    # cross-compile the darwin/linux/windows artifact set into
                # bin/release/<name>-<os>-<arch>/ + sha256sums.txt, no network needed;
                # the Windows hub ships windows-gui, the flasher stays console
```

### What the client cannot be checked with here

`web/app.js` has no JavaScript parser available on the machine this suite runs on —
no node, deno, bun, or d8 — so it is never handed to a real grammar. The suite does
what it can without one, in `jscheck_test.go` and `ui_test.go`:

- grouping characters must balance once comments, string literals, template literals
  and regex literals are stripped;
- the checker itself is tested against seven fragments (a dropped brace, a dropped
  paren, an unclosed template, a regex holding a quote, a comment holding braces, a
  division that is not a regex) so it cannot silently pass everything;
- every `$("#id")` the client uses must exist in `index.html`, which is the failure
  mode that makes a button do nothing with no error anywhere;
- the functions and endpoints each card depends on must still be present by name.

That is a smoke check, not a parser, and it will not catch a semantic JS error. If a
JavaScript engine is available to you, add `node --check web/app.js` to `check` and
delete this paragraph.

`make crosscheck` is not decoration. The port lock was written against
`syscall.Flock`, which has no Windows counterpart, and the hub simply had never
been compiled for Windows — so a binary that could not exist looked exactly like
a platform that was fine. Vet type-checks `_test.go` files too, so this covers
the suite as well as the shipped code.

### Listening for an orb, with no orb and no new dependency

Discovery adds no third-party library: `internal/orbmdns` and the advertiser in
`internal/orbfake` are stdlib `net` plus `syscall` setsockopt, so `make vendor-check`
and `go.mod` stay exactly as they were. Two options the stdlib does not offer decide
whether discovery works at all, which is why the sockets are built by hand in
`mcastsock_unix.go`, `mcastsock_windows.go` and `mdnssock_unix.go`:

- `SO_REUSEADDR` **before** bind. Without it the group port belongs to whoever got
  there first, and on a desktop that is avahi and systemd-resolved, not the hub.
- `IP_MULTICAST_LOOP` on. The stdlib's `ListenMulticastUDP` turns it off, so the
  advertiser's own announce never reaches the browser in the same process group and
  every test of the loop has to be done across two machines.
- `SO_REUSEPORT` deliberately not set: it load-balances a multiply-bound port, and
  the hub's own per-interface sockets would each get a slice of one orb's four records.
- The bind address is the wildcard `0.0.0.0`, not the group. A group-bound socket is
  delivered only datagrams destined for `224.0.0.251`, which is what
  `net.ListenMulticastUDP` gives you on Windows. The hub binds the wildcard so it also
  hears anything addressed to its own LAN IP, and so one socket shape works on every
  platform the file is mirrored across.

Run the loop on one host — the fake as the orb, a stranger's stack as the judge:

```
make manager                                        # bin/orb-manager
go build -o /tmp/orbfake ./cmd/orbfake
/tmp/orbfake -advertise                             # announces, answers, goodbyes on stop
../bench/browse_mdns.py --iface 192.168.2.48 \
    --expect-ip 192.168.2.34 --expect-mac 44bd8d782618   # python-zeroconf, not our browser
curl -s localhost:3779/api/orbs                     # the hub's own card, tokenless read
curl -sN localhost:3779/api/orbs/stream             # the same card, pushed; one frame on open
```

`browse_mdns.py` exits non-zero unless PTR, SRV `port=80`, the TXT `mac=` and the A
record all agree with the values esptool reported for that board, and it is the
independent reader the item is judged by — our browser agreeing with our advertiser
would prove nothing. The fake's stop line is the other half of the evidence:
`read=22 answered=2 broken=0 neighbours=20`. `broken` is a packet that should have
been answered and was not, so a live LAN at `broken=0` is the healthy result;
`neighbours` is avahi, CUPS and printers talking to themselves, which is expected
traffic and not a defect of ours. A non-zero `answered` with zero `broken` is what an
outside browser querying the fake looks like.

The hub is a listener only. It sends no query: an orb re-announces itself every 60 s
(`MDNS_ADV_REFRESH_S` in `main/net/orb_mdns.c`, pinned against the hub's copy by
`TestCheckinCadenceMatchesFirmware`), so a hub that starts after an orb's join waits
at most one interval, and a hub on a box that cannot originate multicast loses nothing
at all — receive is the whole job. `TestBrowserNeverSends` fails the package if a write
path reappears inside `internal/orbmdns`.

The Orbs card follows that rhythm instead of sampling it. The browser holds
`GET /api/orbs/stream` open; the browser package fires `OnChange` when its cache moves,
the cable watch fires the same wake when its plug digest moves,
and the stream re-answers when the *card* would read differently — so a check-in that
changed nothing pushes nothing, an orb that arrives or a board that gets plugged in pushes
one frame, and the ages on
screen move on the 25 s heartbeat. A frame body is byte for byte what `GET /api/orbs`
answers, so the page has one renderer (`applyFound`) and patches rows in place by MAC
rather than rebuilding the card under someone mid-word. There is no Refresh button: it
re-read a cache the hub was already about to push, and with the hub originating nothing
it could not speed the LAN up. What it cannot do is renew a grant by being read — the
stream calls the same `orbCard` the poll does, and neither reaches `reconcileFleet`.

`GET /api/orbs` is one merged list, not two doors the operator has to reconcile by
hand. The LAN half is the browse; the cable half is a TTL-cached sweep of this
machine's USB ports, and the join key is the canonical base MAC — the descriptor
serial the host reads free, never an address. A board on both carries `via:
["lan","usb"]`, its tty in `ports`, and stays dialable on its LAN address; a board on
a cable only sits on the list undialable, `ip` empty, refusal naming its port.
`identity` says who vouches for the mac: `confirmed` (a hub read it off the chip —
stickiest and store-backed), `descriptor` (the host cable saw it), `claimed` (the
packet says so), `unknown` (an unattributed port, listed under its tty path so two
unknowns cannot collide). The hybrid rule: a descriptor-only sighting shows and
expires with the plug and never reaches `state.json`; the store gains a row only from
a probe, a flash, a console verdict, or an announcement. `cable` (chip-confirmed,
sticky) and `onCable` (in this sweep) stay distinct fields — an orb you flashed
yesterday is not tethered today. A failed enumeration keeps the last good bench and
reports `cableError` instead of showing an empty list. `/api/ports` stays raw: the
Onboard picker needs the non-orb ports too.

Requires Go 1.27 (see `go.mod`). The only external dependency is
`go.bug.st/serial`, a pure-Go serial library vendored into the binary. App-window
mode adds none: it drives a Chromium the host already has, so the binary stays
`CGO_ENABLED=0` and cross-compilable.

## Testing on Windows

The hub is one static, `CGO_ENABLED=0` binary, so the Windows build comes from
this same WSL shell — no Windows SDK, no second machine to compile. Run it there
to test the paths WSL cannot reach: native COM enumeration, the Windows profile
dir, and a Chrome started without the interop launcher in between.

```
make windows                          # -> bin/windows-amd64/orb-manager.exe (console)
make windows-gui                      # -> bin/windows-gui/orb-manager.exe (one window, no console)
cp bin/windows-gui/orb-manager.exe /mnt/c/Users/<you>/Downloads/
```

The release and `windows-gui` exes are PE subsystem 2 (`-H windowsgui`), so a
double-click opens the app window and nothing else: no black terminal beside it.
Closing that window shuts the hub down the same way Ctrl-C does — a live flash
finishes first. A subsystem-2 exe launched with no console has nowhere to print,
so its log, stdout and stderr append to `%TEMP%\orb-manager.log`, beside the
browser log; the first lines of that file name its own path. Run from `cmd.exe`
or Windows Terminal and it prints to the terminal like the console build — the
flag suppresses the allocated window, not an inherited console. `make windows`
stays console on purpose: `win-test` and the bench flows execute it and read its
stdout.

Copy it to `C:` rather than running it from `\\wsl.localhost`: Windows marks
files fetched from another host, and SmartScreen questions an unsigned `.exe`
once. `Unblock-File .\orb-manager.exe` in PowerShell clears that.
Discovery on Windows needs one firewall allowance. The hub joins the mDNS group and
*listens* on UDP 5353; a fresh unsigned `.exe` doing that is blocked inbound, so the
Orbs card stays empty and a live orb never appears until you open the port. Answer **Allow
access** to the first-run prompt, or add the rule up front (elevated PowerShell), scoped
to this one program on Private networks:

```powershell
New-NetFirewallRule -DisplayName 'orb-manager mDNS' -Direction Inbound -Action Allow `
  -Protocol UDP -LocalPort 5353 -Program 'C:\Users\<you>\Downloads\orb-manager.exe'
```

With the port open and the wildcard-bind socket in `mcastsock_windows.go`, an orb's
check-in lands and keeps landing — the firmware re-announces every 60 s, so no reboot
and no IP box is involved; the row appears within a minute of the hub starting and
sustains itself. This is the same behaviour Linux and macOS have. If a live orb still
does not appear after this, the next suspect is the access point converting or
dropping multicast over Wi-Fi, which is on the network, not the hub.

### Ship path for a non-technical Windows user (not yet built)

A bench run and a first-day user run differ in three places, all measured here. The
`windows-gui` build already fixes the first: double-clicking the release `.exe` opens
one app window, no console, no PowerShell. Two remain, and neither is a code change —
both are distribution artifacts this repo does not yet produce.

1. **The manual firewall step.** Inbound UDP 5353 is unavoidable: the orb's
   self-refresh announce is unsolicited inbound, and Windows Defender Firewall drops it
   for a program with no allow rule. A first `bind(5353)` pops the OS "Allow access"
   dialog, which creates the right rule scoped to the current profile — so the *manual*
   `New-NetFirewallRule` above is a bench shortcut, not a user step. It only fails on a
   Public profile or an unsigned `.exe` the OS declines to prompt cleanly for. The
   install-time fix: the installer adds the rule as an elevated custom action running as
   SYSTEM, so the user sees no firewall UI at all. Verify the true first-run prompt on a
   fresh profile; this box has rules already and no clean VM to clear them on.
2. **SmartScreen.** "Windows protected your PC" fires because the downloaded `.exe` is
   unsigned and carries Mark-of-the-Web. Authenticode signing clears it, and the
   installer wraps the single signed binary so there is one artifact to sign:
   - **Azure Trusted Signing** — about \$10/month, cloud cert signed through `signtool`
     after a one-time org identity check. The cheap modern path; the intended default.
   - **OV or EV cert** (DigiCert, SSL.com, GlobalSign) — \$100–\$400/year. EV clears the
     SmartScreen reputation gap immediately; an OV or brand-new Trusted Signing cert
     still trips SmartScreen until clean downloads accrue. Tell the first users once.
   - Self-signed is free and useless here: it is trusted only where the root is installed.
   Installer choice: **Inno Setup** (one signed `setup.exe`, simplest) or **WiX** (signed
   `.msi`, the format enterprises and SmartScreen favor). Install to `Program Files` for
   a stable path, add a Start Menu shortcut, and add the firewall rule as the SYSTEM
   custom action above. Then the shipped experience is: download one signed installer,
   run it elevated, double-click one shortcut — no terminal, no firewall prompt, no
   "More info → Run anyway".

Until an installer exists, `make release` ships the loose `.exe` and the user does the
Unblock-File and Allow-access steps by hand. That is the gap the signing task closes;
nothing in this repo blocks it.

Give Windows the cable first. A port attached to WSL is invisible to Windows, and the
picker has nothing to list — it says `(no serial ports found)`, correctly. Release it
from inside WSL, then check Device Manager shows the `USB JTAG/serial debug unit` as a
COM port:

```
tools/orb-manager/scripts/wsl-usb.sh unwatch   # only if a watcher is running
tools/orb-manager/scripts/wsl-usb.sh detach
```

A bare `usbipd detach --busid <busid>` is not enough once an auto-attach watcher is
registered: the watcher re-dials the port within seconds. The script stops the watcher,
detaches, and waits to confirm the node stayed gone. `docs/wsl-usb-reattach.md` is the
full runbook — what survives a reboot, the board table, and the failure modes.

No `-build`, no clone, no git on `PATH`: the firmware is inside the `.exe`. Start
headless and check the two things the browser cannot tell you:

```powershell
cd C:\Users\<you>\Downloads
.\orb-manager.exe -no-window
Invoke-RestMethod http://127.0.0.1:3779/api/status   # platform : windows
Invoke-RestMethod http://127.0.0.1:3779/api/build    # source   : built-in
Invoke-RestMethod http://127.0.0.1:3779/api/ports    # ports    : COM5 ...
```

Those three are reads and need nothing. A scripted POST does: fetch `/` once to
receive this launch's token, then present it. The value changes every time the hub
starts, so it cannot be baked into a script.

```bash
TOKEN=$(curl -s http://127.0.0.1:3779/ | sed -n 's/.*window\.__ORB_TOKEN__ = "\(.*\)".*/\1/p')
curl -s -X POST http://127.0.0.1:3779/api/flash/cancel \
  -H "Content-Type: application/json" -H "X-Orb-Token: $TOKEN" \
  -d '{"port":"/dev/ttyACM0"}'
```

`/api/build` enumerates the five images from the embedded bundle and reports the
firmware's own version and sha256 — from the binary, not from a directory.
`/api/ports` answers `{"ports":[]}` on a machine with no COM ports at all; an
empty list is a real answer, not a failure. `/api/status` reporting something
other than `windows` means you are looking at a WSL hub still holding the port.

Then the window:

```powershell
.\orb-manager.exe
```

Chrome is not on `PATH`, so discovery reads the standard install roots
(`%ProgramFiles%`, `%ProgramFiles(x86)%`, `%LocalAppData%`) after the image-name
lookup. If the hub says `app window unavailable` and opens a tab instead, name
the binary: `-browser "C:\Program Files\Google\Chrome\Application\chrome.exe"`.
The startup line names the browser log — `orb-manager-chrome.log` in `%TEMP%` —
where Chrome's own named-pipe and GCM complaints go instead of your terminal.
Closing the window must end the process: `Get-Process orb-manager` should find
nothing.

Nothing needs the clone. The firmware is compiled in, and so is the Theme Editor,
so a bare `.\orb-manager.exe` serves every card from `C:\Users\<you>\Downloads`
with `repoDir` empty and `git` absent from `PATH`. Add `-repo` only to edit the
composer against the live source — then your edits to `tools/watch-composer/`
appear on reload instead of coming from the copy built into the binary. Quote the
UNC path: the shell collapses an unquoted `\\` to `\`, and the hub reports the
mangled path it tried. Go reads the WSL filesystem over UNC fine:

```powershell
.\orb-manager.exe -repo '\\wsl.localhost\Ubuntu\home\<you>\Projects\gorb-os'
```

To flash a firmware that is not the one built in, click **Use my own firmware…**
and pick a bundle zip. It is validated before it becomes active, and the card
names the file, the version read out of the image, and its sha256, so you can see
what will actually land on the orb. **Use built-in firmware** undoes it.

To run the unit tests on Windows, install Go there and use PowerShell inside
`tools\orb-manager`: `go test ./...`. Eight tests skip: they drive a `#!/bin/sh`
stub browser, which `CreateProcess` cannot exec. The Windows-only halves have
their own coverage that does run — the exclusive port lock, `COM5` versus
`\\.\COM5` collapsing to one lock, lock filenames free of characters Windows
rejects, `%LOCALAPPDATA%` through the real shell, and the install-root browser
discovery above.

From WSL you can also exercise the Windows binary without leaving the keyboard,
because WSL runs `.exe` files natively:

```
make win-test                     # build and run the Windows test binaries
make win-test TEST_ARGS=-test.v   # name-by-name
```

Run it from this package directory, not the repo root and not `/tmp`: the Theme
Editor tests resolve the clone relative to the working directory and skip when it
is not the package dir. `make` locates the Go toolchain itself, so the command
works in a shell where bare `go` is not on `PATH`.

## Caveats

- The Theme Editor's only external dependencies are React 18, ReactDOM, and Babel
  from `unpkg.com` (pulled by `support.js`) and the Google Fonts stylesheets the entry
  page links to. With the vendor tree compiled in — the committed default — the hub
  rewrites all of them to `/composer/vendor/` and the editor needs no network at all.
  A build made without it — a tree where `web/composer/vendor/` was deleted, or one
  where `make vendor-composer` was never run — still needs internet on first paint,
  and the hub then names that failure on screen instead of leaving a blank page.
  `make vendor-composer` produces the tree; `make vendor-check` proves it still
  matches its manifest and the composer's SRI pins.
- USB-Serial/JTAG does not tick the RTC/SWD watchdogs, so a long erase trips
  them and resets the chip mid-write. `Session` disables both before flashing
  (esptool's `disable_watchdogs()` equivalent).
- The ESP32-S3 ROM does not implement `FLASH_END`; session teardown is a physical
  hard reset via RTS, matching the no-stub path.
- Serial flashing needs permission on `/dev/ttyACM*` (add the user to `dialout`).
  Port enumeration is tested on Linux; the Windows/macOS enumeration code paths
  are present but untested on hardware.
- The macOS port classifier (`internal/esprom/usbclass_darwin.go`) asks `ioreg -p
  IOUSB -l -x` and reads `IOCalloutDevice` off the serial client, then the
  `idVendor`/`idProduct`/`USB Serial Number` of the device above it. It is
  compile-checked for both darwin arches and its parser is unit-tested on Linux
  (`usbclass_ioreg_test.go`), but no Mac was available to capture a real `ioreg`
  dump from: the property names come from IOKit as pyserial reads them, and the
  fixtures are hand-authored to Apple's plist DTD. So the negative cases are the
  trusted ones. Until someone runs the hub beside an orb, treat the positive branch
  as unconfirmed — the cost of it being wrong is a refused probe with the message
  "this host cannot identify the device behind the port", never a reset of the wrong
  device. Confirming it takes one `ioreg -p IOUSB -l -x > dump.xml` beside an orb and
  one read of `GET /api/ports`, which must show `"attributed": true` and a `usbMac`.
- Maintainers: `support.js` pulls its scripts from a fixed list of CDN URLs, and
  `TestComposerHasNoUnknownExternalDependencies` fails if that list ever grows to a
  fourth script. That failure is the alarm, not a bug — add the new URL to
  `cmd/orbvendor` and re-run `make vendor-composer`, or the editor silently goes
  back to needing internet for it.