# Building gorb-os

This repo builds four things, and each one feeds the next:

```
 main/  ──idf.py build──▶  firmware image ─┐
                                           │  make -C tools/orb-manager firmware
 apps/<slug>/ ──orbsdk──▶  <slug>.orbapp ──┤  (committed into tools/orb-manager/)
                                           │
 apps/clock/editor/ (inside clock.orbapp) ─┘
                                           ▼
                tools/orb-manager ──go build──▶  orb-manager (one executable:
                                                 firmware, apps and default theme inside)
```

The hub compiles in a **committed** firmware bundle, shelf apps and default theme. So
building the hub needs only Go: no ESP-IDF, no clang, no network. You need the other
toolchains only for the part you change.

## What to install

| To work on | You need |
|---|---|
| the hub (`tools/orb-manager`) | Go 1.27.2 or newer (`go.mod`) |
| apps (`apps/`) | Python 3.10+, clang and lld with the wasm32 target (clang 21 is what the shelf is baked with) |
| the firmware (`main/`) | ESP-IDF v5.4, target `esp32s3` |
| the host test suites (`tools/host_*_test.py`) | Python 3, gcc, Pillow and NumPy |
| the theme editor tests | Node 20+, plus Python with Pillow and NumPy |

On Debian or Ubuntu:

```sh
sudo apt install build-essential python3 python3-pil python3-numpy clang lld
# Go: https://go.dev/dl/   Node: https://nodejs.org/   (or your package manager)
# ESP-IDF v5.4: https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/get-started/
```

On macOS, Homebrew's `llvm` and `lld` provide a wasm-capable clang (set
`ORB_CLANG=$(brew --prefix llvm)/bin/clang`). On Windows, use WSL.

## The one command

```sh
make check
```

This runs every check that needs no board: the host suites, the app/SDK checks, the
hub's Go suite (with vet and a type-check of every release platform), and the editor's
node suites. Two host suites read a configured firmware tree, so run `idf.py build` once
first. CI runs the same targets on every pull request. Each piece runs on its own
too: `make check-host`, `check-apps`, `check-hub`, `check-editor`.

## The hub

```sh
make hub                                  # tools/orb-manager/bin/orb-manager
make -C tools/orb-manager run             # build and open the app window
make -C tools/orb-manager run ARGS=--no-window   # serve on 127.0.0.1:3779, no window
```

`web/` is embedded at compile time, so after a UI edit you rebuild and reload
(`make -C tools/orb-manager reload` hard-reloads the open window). The hub's own
README, [tools/orb-manager/README.md](tools/orb-manager/README.md), covers its
architecture, flags and endpoints.

## Apps

```sh
make apps                                 # re-bake the shelf into tools/orb-manager/apps/
python3 apps/sdk/orbsdk.py build apps/flights    # compile one app, no packing
python3 apps/sdk/orbsdk.py install apps/flights --orb 192.168.1.50
```

An app's bytes are tied to its stamp. If a change alters an app's container, bump
`apps/<slug>/version.txt`. `make apps` refuses to overwrite a container whose bytes
changed under the same version. [apps/README.md](apps/README.md) is the app developer's
guide, and it also covers writing apps outside this repo.

The clock's theme editor ships inside `clock.orbapp`. After editing anything in
`apps/clock/editor/`:

1. Regenerate the extracted component (the snippet in
   [apps/clock/editor/_selftest/README.md](apps/clock/editor/_selftest/README.md)).
2. Run `make check-editor`.
3. Bump `apps/clock/version.txt`, then run `make apps`.

## Firmware

```sh
. ~/esp/esp-idf/export.sh                 # once per shell
idf.py build                              # build/gorb-os.bin and friends
idf.py -p /dev/ttyACM0 flash monitor      # flash over the board's USB-C and watch the log
```

The image's version is `version.txt` plus the commit, stamped at configure time by
`tools/orb_version.sh`. After bumping `version.txt`, run `idf.py reconfigure` before
building, or the image keeps the old stamp.
[docs/firmware-versioning.md](docs/firmware-versioning.md) has the details.

Ways to get a build onto a board:

- `idf.py flash` writes the bootloader, partition table, otadata and app. The theme
  slot stays empty: the hub sends the default theme when it sets the orb up.
- The hub's **Flash firmware → A file from this computer** takes a bundle zip or a
  whole-flash `.bin`.
- `tools/serflash.sh` writes the app slot only, after checking the board's MAC against
  this clone's `.orb-target`. It is the bench tool, and a changed `partitions.csv` still
  needs one full `idf.py flash`.
- In WSL there is no serial port until you attach it with usbipd-win.
  [docs/wsl-usb-reattach.md](docs/wsl-usb-reattach.md) gives the commands.

To make the hub carry a new firmware, re-pack its bundle from your build and commit it:

```sh
make -C tools/orb-manager firmware        # build/ -> internal/firmware/bundle/orb-firmware.zip
```

The hub suite refuses a bundle that disagrees with a `build/` directory beside it, so a
stale bundle cannot slip past `make check` on a machine that has built the firmware.

## The default theme

The theme every new orb receives is `tools/orb-manager/themes/skeleton.orbtheme`, baked
by the editor's own export path, with the preset's sounds (the classic tick, Westminster
quarters and the hour bell). To change it, bake with
`apps/clock/editor/_selftest/bank3-drive.html?dial=skeleton&name=Skeleton&appv=<clock version>`
in Chrome, save the result as `skeleton.orbtheme`, then install it with
`python3 tools/make_default_theme.py <file>`. `--check` re-verifies the committed copy, and
refuses one that carries no sound; `make check-apps` runs it.

## Automated builds on GitHub

Two workflows live in `.github/workflows/`:

| Workflow | Runs on | Does |
|---|---|---|
| `ci.yml` | every pull request and push to `main` | the four jobs below; a red one blocks the merge |
| `release.yml` | a pushed tag `v*`, or by hand | builds, signs and packages the hub for Windows, macOS and Linux, plus the app SDK, as a **draft** release |

`ci.yml`'s jobs:

- **apps and SDK**: `make check-apps` with clang 21, then the SDK tarball is unpacked
  and used to build a fresh app from the template outside the tree
- **orb-manager**: `make check-hub`
- **theme editor**: `make check-editor`
- **firmware build and host suites**: `idf.py build` in the `espressif/idf:v5.4`
  container, then `make check-host` in the same tree. The suites need what a configured
  firmware tree has: `build/config/sdkconfig.h` and ESP-IDF's NVS generator. The image is
  kept as a run artifact.

## Cutting a release

A release is the hub executable for three platforms, with the current firmware, the shelf
apps and the default theme built in, plus `orb-sdk-<version>.tar.gz` for app developers.

1. **Firmware.** If `main/` changed since the last release, bump `version.txt`, run
   `idf.py reconfigure && idf.py build`, check it on a board, then run
   `make -C tools/orb-manager firmware` and commit the bundle.
2. **Apps.** If an app changed, bump its `version.txt`, run `make apps`, and commit.
3. **Check.** Run `make check`, and make sure CI is green on `main`.
4. **Tag.** `git tag v1.2.6 && git push origin v1.2.6`. The tag must equal `version.txt`.
   A hub-only rebuild of the same OS takes a suffix (`v1.2.6-2`).
5. **Publish.** When the run finishes, open the draft under Releases, choose Edit, and press
   **Publish release** at the bottom of the edit page.

[RELEASING.md](RELEASING.md) covers signing (Apple Developer ID and notarization, Azure
Artifact Signing), the secrets each needs, and the unsigned fallback. `make dist` builds
the same archives locally, unsigned.
