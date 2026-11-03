# gorb-os

**Greg's Orb OS**: firmware for a round desktop display, the "orb", and the desktop
program that looks after it. An orb is a Waveshare ESP32-S3-Touch-AMOLED-1.75 board
(466×466 AMOLED) with a rotary knob. It shows animated watch faces you design yourself
and runs small installable apps.

- **Animated clock faces**, designed in the Theme Editor and sent to the orb over Wi-Fi.
- **Apps**, sandboxed WebAssembly modules with a permission for each thing they touch,
  installable from the desktop or the orb's microSD card.
- **Orb Manager**, one executable for Windows, macOS and Linux. It flashes a blank board,
  puts it on Wi-Fi, installs apps and themes, and updates the firmware. There is nothing
  else to install.

## Start here

| You want to | Read |
|---|---|
| set up and use an orb | [docs/user-guide.md](docs/user-guide.md), then download Orb Manager from [Releases](https://github.com/gregtakacs/gorb-os/releases/latest) |
| write an app | [apps/README.md](apps/README.md). Apps can live in their own repo, built against the SDK each release attaches |
| build the firmware or the hub | [BUILDING.md](BUILDING.md) |
| fix something or add a feature | [CONTRIBUTING.md](CONTRIBUTING.md) |
| cut a release | [BUILDING.md § Cutting a release](BUILDING.md#cutting-a-release) and [RELEASING.md](RELEASING.md) (signing) |
| find your way around the code | [docs/architecture.md](docs/architecture.md) |

Bugs and feature requests go in [Issues](https://github.com/gregtakacs/gorb-os/issues/new/choose);
questions and show-and-tell go in [Discussions](https://github.com/gregtakacs/gorb-os/discussions).

## Hardware

| Component | Details |
|-----------|---------|
| Board     | Waveshare ESP32-S3-Touch-AMOLED-1.75 |
| MCU       | ESP32-S3R8, dual core at 240 MHz, 8 MB OPI PSRAM, 16 MB flash |
| Display   | CO5300 AMOLED, 466×466, QSPI (CS=12, RST=39, SCLK=38, D0–D3=4/5/6/7, TE=13) |
| Knob      | KY-040 on header H2 (A=18, B=17, SW=16): the orb's input |
| IMU       | QMI8658 on the shared I2C bus (SDA=15, SCL=14): wakes the display when moved |
| Audio     | ES8311 codec and NS4150B amplifier: clock ticks and chimes |
| microSD   | SPI mode (MOSI=1, SCK=2, MISO=3, CS=41); shared over USB as a disk |
| USB       | native USB-Serial-JTAG on the USB-C port: flashing, console, SD sharing |

The panel's touch layer is present but not driven yet. `main/board_config.h` is the pin
source of record.

## What is in the tree

```
main/                 the firmware (ESP-IDF v5.4 + LVGL 9): display, compositor, theme
                      store, app platform and wasm3 host, Wi-Fi, HTTP API, OTA, SD, USB, audio
apps/                 the apps (one directory each) and the app SDK (apps/sdk/)
  clock/editor/       the Theme Editor, which ships inside the clock app
tools/orb-manager/    Orb Manager, the Go desktop hub, with the firmware, apps and
                      default theme it carries
tools/                format writers and readers, bakers, the host test suites, bench tools
components/           vendored wasm3 (PROVENANCE.md)
docs/                 architecture, the user guide, the living design ledgers; finished
                      plans in docs/history/
tests/                fixtures shared by the host suites and the editor (a real composer kit in tests/fixtures/av-kit/)
```

## Quick build

```sh
make check                      # every check that needs no board
make hub                        # tools/orb-manager/bin/orb-manager (Go only)
. ~/esp/esp-idf/export.sh && idf.py build    # the firmware
```

[BUILDING.md](BUILDING.md) has the prerequisites and the full workflow.

## Versions

The firmware's version is `version.txt` (`vMAJOR.MINOR.PATCH`). The image stamp adds the
commit, as `v1.2.6+05f54c6`, plus `-dirty` for an uncommitted tree
([docs/firmware-versioning.md](docs/firmware-versioning.md)). Each app carries its own
`apps/<slug>/version.txt`, independent of the firmware. A release tag names the firmware
version, and its Orb Manager carries that firmware.

## Licence

MIT. See [LICENSE](LICENSE). gorb-os began as a rewrite of the Arduino
[Orb OS](https://github.com/gregtakacs/orb-os) (also MIT) onto ESP-IDF. Comments that
cite `orb-os/src/...` point at that upstream. Third-party licences for what Orb Manager
ships are in `tools/orb-manager/packaging/THIRD_PARTY_NOTICES.txt`.
