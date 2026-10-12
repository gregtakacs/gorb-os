# orb-manager

A single static Go binary that onboards, themes, and manages gorb-os orbs on
the LAN. It drives the flasher itself — no esptool, no Python, no stub loader.
The serial library is compiled in. The UI is embedded. Users install nothing,
and the hub opens in its own window rather than a browser tab.

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
   corrupting the write mid-stream. A process-wide flag makes it a belt to that
   brace.

The hub does no LAN discovery. It never reads the ARP/neighbour table, never
shells out to `arp`/`arping`, and never sweeps a subnet. An orb's address is
known only from what the hub already read over its USB console — the lease the
provisioning door captured, or the association banner the flash logged — or from
an IP the operator types. Every network call the hub makes is a dial to one
address it was handed, through the same-origin proxy.

For an operator who wants an extra identity check before a headless flash,
`orbflash -mac aa:bb:...` reads the chip's eFuse MAC and refuses on mismatch.
The web tool does not impose it; you act on the port you picked.


## Run it

```
make build                       # -> bin/orb-manager, bin/orbflash
./bin/orb-manager                # opens the hub in its own app window
```

`orb-manager` auto-detects the clone root (`git rev-parse --show-toplevel`) so it
finds `build/` and the embedded Theme Editor's source on its own. Point it
elsewhere with `--build <dir>` or bind another address with `--addr host:port`.

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

Three switches cover the exceptions:

| Flag | Effect |
|---|---|
| `--no-window` | serve only and print the URL — the bench, SSH, and CI path |
| `--browser=browser` | old behaviour: a normal tab in the default browser |
| `--browser=/path/to/chrome` | name the binary instead of autodetecting |

Chromium only, deliberately: the Theme Editor is a Blink canvas app whose file
dialogs and `localStorage` asset store are exercised in Chrome, so the hub is
hosted in the same engine and `/composer/` opens inside that window. No Chromium
on the host, or an unreadable profile directory, falls back to the default
browser and the hub still works as a plain web app.

The three hub cards:

- **Onboard** — pick a serial port and a flash mode, watch the streaming log and
  progress bar. `full` writes the whole `flasher_args.json` manifest; `app`
  writes only the app plus otadata, leaving bootloader/partitions/assets. After
  either mode, a successful flash runs the same post-flash onboarding probe; the
  boot verdict the flash captured picks the branch (AP card vs. connected panel),
  not the flash mode.
- **Theme Editor** — serves the repo's watch-composer at `/composer/`, so edits
  to the composer appear on reload with no rebuild.
- **Manage** — point at one orb by the IP provisioning reported (or one you look
  up on your router) and it dials that single address through the same-origin
  proxy to show a live status panel. Reboot, timezone, and Wi-Fi actions go
  through that same proxy. No subnet scan, no ARP table.

## Headless flasher

`orbflash` drives the identical `esprom.Session` without the web UI, for scripted
bench use:

```
make orbflash
./bin/orbflash -mode full -port /dev/ttyACM0 -build ../build   # whole manifest
./bin/orbflash -mode app  -port /dev/ttyACM0 -build ../build -mac 44:bd:8d:60:80:88
```

The default `-mode probe` only enters download mode, syncs, reads the chip id and
eFuse MAC, then hard-resets back to the app. `-mac` applies the eFuse gate even
headless; a mismatch aborts before erasing.

## Endpoints

| Route | Purpose |
|---|---|
| `GET  /api/status` | platform, build stamp, repo/build dirs |
| `GET  /api/ports`  | serial ports enumerated from the host |
| `GET  /api/build`  | the flash manifest resolved from `flasher_args.json` |
| `POST /api/flash`  | flash the selected port, streamed as newline-delimited JSON events |
| `POST /api/watch`  | post-flash onboarding probe: boot verdict, then confirm the USB-learned IP, NDJSON |
| `POST /api/provision` | wired (serial) provisioning round-trip: boot scan + one PROV line + PROV-ACK, NDJSON |
| `GET/POST /orb/<ip>/api/v1/...` | same-origin proxy to an orb's device API |

`/api/flash` streams `{"log":...}`, `{"progress":{...}}`, `{"mac":"..."}`
(eFuse MAC, emitted as soon as the ROM answers), `{"done":...}`, and
`{"error":...}` lines; the UI's `streamFlash` parses them into the log and bar.
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
api.go               the /api/* JSON handlers (status, ports, build)
flash.go             POST /api/flash: port lock + NDJSON stream + boot verdict (with USB lease)
watch.go             POST /api/watch: boot verdict + confirm the USB-learned orb IP
provision.go         POST /api/provision: wired (serial) provisioning round-trip
proxy.go             same-origin /orb/<ip>/api/v1/... device-API proxy
composer.go          serves the repo's watch-composer at /composer/
cmd/orbflash/        headless face of the same flasher
cmd/serprov/         headless wired-provisioning round-trip tester (bench gate)
internal/esprom/     the native ROM flasher (serial, SLIP, commands, plan, session, boot console, serial-provision)
internal/orbbind/    per-port flock (LockPort) only — no orb identity, no bench state
internal/orbnet/     single-orb status client (no sweep, no ARP)
web/                 the embedded hub UI (index.html, app.css, app.js)
```

## Development

```
make test      # go test ./... — SLIP framing, block checksum, SHA256 recalc,
               # MAC decode, FLASH_BEGIN 5-word framing
make vet       # go vet ./...
make fmt       # go fmt ./...
make run       # rebuild + open the app window against 127.0.0.1:3779
               # add --no-window to serve without a window (SSH, CI, bench)
make flash PORT=/dev/ttyACM0   # headless full flash of that port
```

Requires Go 1.27 (see `go.mod`). The only external dependency is
`go.bug.st/serial`, a pure-Go serial library vendored into the binary. App-window
mode adds none: it drives a Chromium the host already has, so the binary stays
`CGO_ENABLED=0` and cross-compilable.

## Caveats

- The Theme Editor loads React 18, Babel, and Google Fonts from CDNs. The
  composer needs internet on first paint; the manager itself does not.
- USB-Serial/JTAG does not tick the RTC/SWD watchdogs, so a long erase trips
  them and resets the chip mid-write. `Session` disables both before flashing
  (esptool's `disable_watchdogs()` equivalent).
- The ESP32-S3 ROM does not implement `FLASH_END`; session teardown is a physical
  hard reset via RTS, matching the no-stub path.
- Serial flashing needs permission on `/dev/ttyACM*` (add the user to `dialout`).
  Port enumeration is tested on Linux; the Windows/macOS enumeration code paths
  are present but untested on hardware.