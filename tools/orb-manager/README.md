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
binding, no `orb_bind` MAC/IP gate, no ARP or subnet discovery. A user owns the
orb on the cable they picked; the hub reaches it over USB or an address they
give it.

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

   `/orb/<ip>/…` carries three further gates, because it is the hub's only route off
   the box: the address must be an **IP literal this hub learned** over USB or was
   handed by `POST /api/orb/allow` when the operator committed to it in the Manage
   card; the path and method must be in the **device route allowlist**, which mirrors
   the URI table in `main/net/orb_net.c` and `main/theme_app.c`; and the exchange is
   **byte-capped** (8 MiB down, 4 MiB up). The device's own `/api/v1/ota` is
   deliberately **not** in that allowlist: the hub's rule is that firmware comes from
   a validated bundle and the whole manifest is always written, so the bare-app path
   is left out of the browser's reach. The port is never taken from the URL, which is
   what keeps a learned address from becoming a port-scanning target.

3. **Binding off loopback is a decision someone has to type.** `-addr` accepting a
   LAN address used to be enough on its own; now `-allow-remote` is also required,
   and the exposure is logged. The token still guards the mutating routes either way.

The hub does no LAN discovery. It never reads the ARP/neighbour table, never
shells out to `arp`/`arping`, and never sweeps a subnet. An orb's address is
known only from what the hub already read over its USB console — the lease the
provisioning door captured, or the association banner the flash logged — or from
an IP the operator types, which the Manage card registers with the hub before the
proxy will dial it. Every network call the hub makes is a dial to one address it
was handed, through the same-origin proxy.

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
`images/`. `make firmware` produces one from a build dir on the machine that has
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
  that click, one port at a time, caches the answer for the session, and takes the
  same per-port lock a flash does. The identity is invalidated by a flash of that
  port, so a re-probe reports the new firmware rather than repeating the old.

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
- **Manage** — point at one orb by the IP provisioning reported (or one you look
  up on your router) and it dials that single address through the same-origin
  proxy to show a live status panel. Reboot, timezone, and Wi-Fi actions go
  through that same proxy. No subnet scan, no ARP table.

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
| `POST /api/orb/allow` | `{"ip"}` — register the address the operator just typed, which is what lets `/orb/` dial it |

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
proxy.go             same-origin /orb/<ip>/api/v1/... device-API proxy
composer.go          serves the repo's watch-composer at /composer/
cmd/orbflash/        headless face of the same flasher
cmd/serprov/         headless wired-provisioning round-trip tester (bench gate)
internal/esprom/     the native ROM flasher (serial, SLIP, commands, plan, session, boot console, serial-provision)
                     identity.go: ProbeOrb and the boot-log parser; usbclass_*.go: per-OS device attribution
internal/firmware/   the bundle: pack, load, verify, and the embedded orb-firmware.zip it ships with
cmd/orbpack/         release-time packer: build dir + manifest -> the committed bundle
cmd/orbprobe/        headless "what is on this cable", same probe the Identify button runs
internal/orbbind/    per-port exclusive lock (LockPort): flock on Unix, an
                     unshared CreateFile handle on Windows
internal/orbnet/     single-orb status client (no sweep, no ARP)
web/                 the embedded hub UI (index.html, app.css, app.js)
```

## Development

```
make test       # go test ./... — SLIP framing, block checksum, SHA256 recalc,
                # MAC decode, FLASH_BEGIN 5-word framing. Arms the bundle
                # staleness guard automatically when build/flasher_args.json
                # exists, so a committed bundle that has drifted from the tree
                # fails here instead of skipping.
make check      # the whole ladder: test (guard armed) + vet + crosscheck
make vet        # go vet ./...
make fmt        # go fmt ./...
make firmware   # re-pack the embedded firmware bundle from this clone's build dir
make composer   # re-copy the Theme Editor into web/composer/ for the shipped binary
make run        # rebuild + open the app window against 127.0.0.1:3779
                # add --no-window to serve without a window (SSH, CI, bench)
make flash PORT=/dev/ttyACM0   # headless full flash of that port
make windows    # cross-compile bin/windows-amd64/{orb-manager,orbflash}.exe
make win-test   # run the Windows test binaries natively, through WSL interop
make crosscheck # go vet every supported GOOS/GOARCH, tests included
```

`make crosscheck` is not decoration. The port lock was written against
`syscall.Flock`, which has no Windows counterpart, and the hub simply had never
been compiled for Windows — so a binary that could not exist looked exactly like
a platform that was fine. Vet type-checks `_test.go` files too, so this covers
the suite as well as the shipped code.

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
make windows                          # -> bin/windows-amd64/orb-manager.exe
cp bin/windows-amd64/orb-manager.exe /mnt/c/Users/<you>/Downloads/
```

Copy it to `C:` rather than running it from `\\wsl.localhost`: Windows marks
files fetched from another host, and SmartScreen questions an unsigned `.exe`
once. `Unblock-File .\orb-manager.exe` in PowerShell clears that.

Give Windows the cable first. A port attached to WSL through `usbipd` is invisible
to Windows, and the picker has nothing to list — it says `(no serial ports
found)`, correctly. Release it, and check Device Manager shows the
`USB JTAG/serial debug unit` as a COM port:

```
usbipd detach --busid <busid>
```

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

- The Theme Editor loads React 18, Babel, and Google Fonts from CDNs
  (`unpkg.com`, `fonts.googleapis.com`), so the composer needs internet on first
  paint; the manager itself does not. With that fetch blocked the page would be
  blank and silent — the hub now names the failure on screen instead.
- USB-Serial/JTAG does not tick the RTC/SWD watchdogs, so a long erase trips
  them and resets the chip mid-write. `Session` disables both before flashing
  (esptool's `disable_watchdogs()` equivalent).
- The ESP32-S3 ROM does not implement `FLASH_END`; session teardown is a physical
  hard reset via RTS, matching the no-stub path.
- Serial flashing needs permission on `/dev/ttyACM*` (add the user to `dialout`).
  Port enumeration is tested on Linux; the Windows/macOS enumeration code paths
  are present but untested on hardware.