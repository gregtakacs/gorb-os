# gorb-os

**Greg's Orb OS** — ESP-IDF firmware for the Waveshare
ESP32-S3-Touch-AMOLED-1.75 board (ESP32-S3R8, 466x466 CO5300 AMOLED via QSPI,
8 MB OPI PSRAM, 16 MB flash). Current: `v1.1.0` (`version.txt`).

A rewrite of the Arduino **Orb OS** project (https://github.com/gregtakacs/orb-os,
locally `~/docker/Projects/orb-os`) onto ESP-IDF + LVGL 9. Comments that cite
`orb-os/src/...` are pointing at that upstream on purpose: this repo is
`gorb-os`, the upstream stays `orb-os`.

## Current state

The orb is a themed, connected device, not a clock sketch:

- **Dial**: animated themed clock faces from FORMAT 3 `.orbtheme` bundles,
  resident in an 8 MB `assets` partition (single-slot, Migration 1). A base
  face renders when no theme is resident. Themes install from the config
  page, the hub, or the microSD drop zone — never via a firmware reflash.
- **App platform**: installable `.orbapp` containers behind one frozen
  broker surface (`orb_svc`) with a per-app grant gate; compiled-in C apps
  and wasm3 guests meet the same contract. Factory pair: the clock and an
  HSB color editor. Knob-driven carousel and menu.
- **Networking**: WiFi STA with first-boot SoftAP provisioning (captive-portal
  aware), SNTP with the timezone as device config, the orb's name as device
  config, LAN OTA into A/B slots with rollback, mDNS `_orb._tcp`, a gzipped
  PWA config page on the device, and a 34-route HTTP API.
- **Power**: idle dim with IMU motion wake (QMI8658).
- **Tooling**: `tools/orb-manager` — a single Go binary hub that flashes,
  provisions, and themes orbs over USB/LAN with no esptool and no IDF — and a
  bench suite of 26 host-compiled parity tests. See
  [docs/architecture.md](docs/architecture.md); live status and rulings:
  [docs/app-platform-plan.md](docs/app-platform-plan.md).

## Hardware

| Component | Details |
|-----------|---------|
| MCU       | ESP32-S3R8 dual-core @240 MHz |
| Display   | CO5300 AMOLED, 466x466, QSPI (CS=12, RST=39, SCLK=38, D0-D3=4/5/6/7, TE=13) |
| I2C       | SDA=15, SCL=14 (shared bus: touch, IMU, RTC, PMIC; the firmware drives the QMI8658 IMU today) |
| IMU       | QMI8658 6-axis — motion wake |
| Knob      | KY-040 on header H2 (A=18, B=17, SW=16) — the primary input |
| microSD   | SPI mode (MOSI=1, SCK=2, MISO=3, CS=41), no card-detect line |
| Flash     | 16 MB — A/B app slots + 8 MB theme bank + PWA + NVS + app ledger (`partitions.csv`) |
| PSRAM     | 8 MB OPI |
| USB       | Native USB-Serial-JTAG (flash + console over USB-C) |

## Prerequisites

- ESP-IDF v5.4 (installed at ~/esp/esp-idf)
- Source the environment before building:

      source ~/esp/esp-idf/export.sh

- USB-C cable connected to the board (flashes via the native USB-Serial-JTAG peripheral)

## Build & Flash

    # one-time per shell
    source ~/esp/esp-idf/export.sh

    idf.py set-target esp32s3   # already configured
    idf.py build

    idf.py -p /dev/ttyACM0 flash            # full flash incl. partition table
    idf.py -p /dev/ttyACM0 flash monitor    # + console

Two bench paths avoid raw `idf.py flash` once a board is set up:

    tools/serflash.sh        # app + otadata only, bound-target + stamp audited
    tools/orb-manager        # the hub: flash/provision/theme over USB or LAN

`serflash.sh` deliberately pushes only app + otadata, so a **changed
`partitions.csv` needs one `idf.py flash`** (or a hub re-flash) to reach the
board. The `assets` theme default (`assets/orb_default.img`) is flashed by
`idf.py flash` too — re-bake it with `tools/rebake_bundle.py` +

## Versioning

Every image carries a stamp in `esp_app_desc_t.version`, composed in exactly
one place — `tools/orb_version.sh` — and read by everything else:

    v<MAJOR>.<MINOR>.<PATCH>[-<prerelease>]+<7-char SHA>[-dirty]

`version.txt` at the repo root holds the declared part, one line, no comments
(cmake's `file(STRINGS)` folds every line into the value). The metadata after
the plus is provenance and cannot be typed by hand: the SHA names the commit
the tree was at, and `-dirty` says the tree carried uncommitted edits when
cmake ran. A stamp is a git fact, never an aspiration.

Bump `version.txt`, then:

    idf.py reconfigure && idf.py build

The reconfigure is not optional. cmake stamps at configure time and caches the
result, so `idf.py build` alone recompiles your edits and keeps the old stamp —
an image carrying new code under an old version. `tools/serflash.sh` reads the
stamp back out of `build/gorb-os.bin` and refuses the flash when it disagrees
with the working tree. Widths are a hard limit: `esp_app_desc_t.version` holds
31 bytes, so each component caps at two digits. Check the composer after
touching it:

    tools/orb_version.sh --self-test    # 14 assertions, ~2 s, no idf.py needed

`docs/firmware-versioning.md` carries the grammar, the arithmetic and the four
read-out surfaces.

## Host-side checks

Pure functions — the config-body parser, the timezone math, the manifest and
bundle readers, the grant gate — are lifted verbatim out of shipped `.c` files
and checked on the bench with host gcc before anything is flashed:

    python3 tools/host_tz_test.py       # ~1 s, no idf.py needed
    python3 tools/host_svc_test.py
    ls tools/host_*_test.py             # the full family (26 suites)

Formats keep a three-way rule: the Python writer, the shipped C reader, and
the parity suite move in one commit or the test fails. The convention, the
suite map, and the bench discipline are in
[docs/architecture.md](docs/architecture.md).


## Project structure

    main/          firmware: boot ladder, display, gfx compositor, clock, art
                   (FORMAT 3 stack), app platform, net, sd, knob, input, imu,
                   power, color — one dir per subsystem; contracts and the
                   boot order live in docs/architecture.md
    components/    vendored wasm3 interpreter (PROVENANCE.md tracked)
    managed_components/  lvgl/lvgl ^9, espressif/esp_lcd_co5300 (idf_component.yml)
    assets/        orb_default.img (the flashed FORMAT 3 slot default) + source art
    partitions.csv 16 MB layout — retention + A/B invariants documented inline
    docs/          plan/decision records (app-platform-plan.md is the master)
                   + architecture.md, the deep version of this map
    tools/         bakers, format writers/readers, host suites, serflash,
                   orb-manager (the Go hub), watch-composer (the dial composer)
    tests/         shared fixtures for the host suites

## Roadmap

- [x] Analog themed dial + base face; FORMAT 3 bundles, single 8 MB slot
- [x] WiFi: STA + SoftAP provisioning, LAN config page, LAN OTA with A/B rollback
- [x] SNTP with the timezone as device config; the orb's name as device config
- [x] Input: rotary knob + input router + carousel + menu (IMU motion wake)
- [x] Theming: bench-baked bundles, install from page/hub/SD, no reflash
- [x] App platform: `.orbapp` containers, broker + grants, wasm3 tier, SD drop zone
- [x] Hub: `tools/orb-manager` single-binary flash/provision/theme
- [ ] Capacitive touch (the panel has it; the firmware does not drive it yet)
- [ ] USB Mass-Storage (TinyUSB) access to the SD card — **shelved**, full
      design in `docs/usb-sd-msc-plan.md`

## License

MIT — see [LICENSE](LICENSE). Upstream Orb OS is also MIT, so the port
provenance (source art under `assets/sources/`, ports noted in file headers)
is license-compatible.

`tools/mk_slot_image.py` if it is absent (the build prints the instruction).
