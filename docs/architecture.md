# gorb-os architecture

The reference for what is in the tree, what each subsystem's contract is, and
where to add things. `README.md` is the front page; this file is the map.
Every claim here is dated against commit `7590feb` (v1.1.0, 2026-10-22). When
this file and a `docs/*-plan.md` disagree, the plan doc wins — plans carry the
owner's numbered rulings; this file only describes what those rulings built.

## What the device is

A wristless desktop orb: a round 466x466 AMOLED that runs themed, animated
clock faces, an installable-app platform behind a single broker surface, and
a plain-HTTP LAN API for provisioning, OTA, themes, and apps.
Silicon: Waveshare ESP32-S3-Touch-AMOLED-1.75 — ESP32-S3R8 (dual core @240
MHz), CO5300 AMOLED over QSPI, 8 MB OPI PSRAM, 16 MB flash, QMI8658 IMU on the
shared I2C bus, SPI-mode microSD socket, KY-040 rotary knob on the H2 header,
native USB-Serial-JTAG for console and flashing. `main/board_config.h` is the
pin source of record.

## Boot ladder (`main/main.c`, `app_main`)

Order is load-bearing; the comments in `main.c` carry the why for each rung.

1. `nvs_flash_init()` on the default `nvs` partition, with erase-recovery.
   Safe because user data does not live there (see Storage model).
2. `orb_theme_boot()` — resolve the theme and execute any staged install
   **before the display comes up**: an install rewrites the region the dial
   reads pixels from. Failure degrades to the fallback face, never a halt.
3. `display_init()` — SPI2 QSPI bus, CO5300 panel init, LVGL 9 bring-up.
4. App platform, in this exact order:
   `orb_app_install_resolve_boot()` (rebuild the card table from the apps
   partition — the image ships no apps) →
   `orb_svc_init()` (grant + enable words into RAM) →
   `orb_app_install_apply_enable()` → `orb_app_install_apply_code()` (wasm
   runtimes for enabled code apps) → `orb_app_init_all()` (builds every app's
   screen in registry-table order — table order IS boot order).
5. `orb_carousel_init()` — soft-fail: no name chip just degrades the knob walk.
6. `knob_init()`, `input_router_init()` — knob events route to the active
   screen; ~10 ms poll on the LVGL task.
7. `imu_init()` (only when `CONFIG_ORB_IDLE_MOTION_ENABLE`) and
   `idle_dim_init()` — last, because dim needs the panel and may poll the IMU.
8. The LVGL task starts (priority 5, 8 KB stack, 5–50 ms cycle).
   `idle_dim_render_drain()` runs at the top of every iteration: brightness
   writes decided elsewhere are applied on the flush task, never the decider's.
9. OTA rollback confirm — waits for two real panel flushes (max 6 s) before
   `esp_ota_mark_app_valid_cancel_rollback()`. A wedged display must not pass.
10. `orb_net_start()` (soft-fail) — after the LVGL task exists, so the radio
    can never present as a blank screen.
11. `orb_sd_init()` + `orb_app_scan_init()` (both soft-fail) — the socket has

## Subsystems under `main/`

| Dir | Contract |
|---|---|
| `display/` | SPI2 QSPI bring-up + LVGL. Panel driver is the managed `espressif__esp_lcd_co5300`. TE pin (GPIO13) read; QSPI stays at the vendor 40 MHz — 80 MHz shears rows on this board. `DISPLAY_BENCH` compile flag gates frame timing. |
| `gfx/` | The compositor. `orb_sched` decides frame cadence; `orb_layers` is the app layer table (LVGL-free header, host-compiled as shipped); `orb_gfx` the drawing primitives. |
| `clock/` | `clock_view` the themed dial; `clock_base` the face shown when no theme is resident (§26 slice 1 — separate TU so the two cannot share state); `clock_themes` the theme registry glue. |
| `art/` | The FORMAT 3 theme stack. `art_bank` mmaps the `assets` partition once at boot and never unmaps (LVGL descriptors cache pointers into it). `orb_theme` the store: boot resolution, staged installs, single-slot flip (Migration 1). `orb_bundle` the FORMAT 3 reader — the parse mirror of `tools/orbbundle.py`. `orb_clock3` the clock section's projection. `orb_elems` the (class, slot, frame)→pixels seam every themed app reads through. Zero-copy by design: parse and refuse, never clamp. |
| `assets/` (in `main/`) | `orb_assets.c` — the asset registry, the one file that knows what a slot IS. Compiles without LVGL so host suites can link it directly. |
| `app/` | The platform: registry, manifest reader, install catalog, SD scan, wasm tier, broker, jobs, carousel. Detail below. |
| `net/` | `orb_net.c` — everything HTTP and WiFi in one TU: STA + provisioning SoftAP (with captive-portal probes + DNS responder), SNTP on lease (2 servers, `LWIP_SNTP_MAX_SERVERS=3`), timezone + name as NVS device config, LAN OTA into the A/B slots, the whole `/api/v1` surface, and the pre-gzipped PWA config shell served from the `www` partition. Request bodies are parsed by a hand-rolled bounded scanner (`json_find`) — deliberately not cJSON; it is the host-tested seam. `orb_mdns` advertises `_orb._tcp`. |
| `sd/` | `orb_sd` the SDSPI FAT mount (retries on demand via `POST /api/v1/sd/rescan` — no hot-plug sense); `orb_files` the file helpers. |
| `knob/` | KY-040 quadrature + press, GPIO edge ISRs. |
| `input/` | `input_router` — the one place that decides which screen gets a knob event. Turn swaps apps, push opens, hold returns to the menu. |
| `imu/` | QMI8658 on the shared I2C bus (probes both strap addresses). Used for motion wake. |
| `power/` | `idle_dim` — dimming decisions at its own priority; the issued brightness write is drained and applied by the LVGL task (boot ladder step 8). |
| `color/` | The HSB picker — a compiled-in app, and the model for what an app iface looks like. |

## The app platform (`main/app/`)

- **Registry** (`orb_app.c`): the table of apps IS boot order. `main.c` names
  no app. One compiled row lives here since D35 — the shell's own options page,
  tagged `ORB_APP_KIND_SHELL`. The image ships no apps (D36): the clock and the
  HSB editor are ordinary card apps the hub seeds onto the card and installs
  through the ordinary doors, so a row that can be removed is a row the owner
  owns. `tools/make_factory_apps.py` writes those two hub-side seed containers.
- **Manifest** (`orb_app_manifest.c`): the `.orbapp` reader. Carries slug,
  entry point, `period_ms` floor, declared settings, and the declared grant
  list. `behaviour_kind` picks the iface: `ORB_BEHAVIOUR_COLOR_PICKER`,
  `ORB_BEHAVIOUR_WASM`, … (append-only enum).
- **Install** (`orb_app_install.c`): the `apps` partition catalog — manifest
  copies + geometry, survives a card pull. Enable/failed/grant words in NVS.
  Slot index = registry position, pinned in the catalog, and the key for
  every per-app NVS namespace.
- **Broker** (`orb_svc.c/h`): the entire surface an app may call (ruling D15).
  One frozen function table: `now`/`tz_label`/`tz_offset`, `power_request`,
  `cfg_get/set(+_str)`, `http_get`, `bank`, `layer_add/set/touch/del`,
  `job_post`, `set_beat`, the `theme_*` family, `status`. Every entry checks
  the grant bitmask **before** doing anything, and counts calls + denials per
  app (visible in `/api/v1/status`). Threading: entries run on the LVGL task;
  the config entries may block on NVS, so apps must reach them through
  `job_post`.
- **Caps** (`orb_svc.h`): `TIME CONFIG UI ART` are free and always on —
  enforced by not being revocable. The owner-granted four: `POWER NET
  SD_READ THEMES`. Declare-at-install is the answer (ruling D19); the dial's
  ladder is the door that closes them again. Enum numbers are the NVS grant
  word layout: append, never reorder.
- **Settings quota**: 2 KB per app (`ORB_CFG_QUOTA_BYTES`) against the 24 KB
  `storage` partition; each app gets its own NVS namespace `orb.app<slot>`,
  so a bug cannot address another app's keys.
- **Jobs** (`orb_jobs.c`): the worker context for blocking app work.
- **wasm tier** (`orb_app_wasm.c` + `components/wasm3/`, vendored with
  `PROVENANCE.md`): one generic behaviour module, N guest instances. A guest
  gets its entry points CALLED by the registry and calls the broker BACK
  through imports bound from its granted caps — the same contract a compiled
  app meets, at the same line. There is no per-app C code for a wasm app.
- **SD drop zone** (`orb_app_scan.c`): `/sd/apps/` is scanned beside the card;
  installs cross into the `apps` partition from there.
- **Carousel** (`orb_carousel.c`): the knob-driven app switcher chrome (name
  chip on the top layer, built once).

    no card-detect line, so mounting is a ladder that can take seconds per
    failed rung. It belongs after the dial is drawing.

## HTTP API (`main/net/orb_net.c`, one `uris[]` table)

Plain HTTP, port 80 (`CONFIG_ORB_NET_HTTPD_PORT`). No auth on the device: the
provisioning AP is LAN-isolated by design, and STA mode assumes a trusted LAN.

| Group | Routes |
|---|---|
| Pages (PWA shell from the `www` partition, pre-gzipped) | `GET /` `/themes` `/apps` `/timezone` |
| Status | `GET /api/v1/status` — fw stamp (from `esp_app_desc_t`), name, time/sync, heap, LVGL pool, per-app call/deny counters, OTA `img` states |
| Device config | `GET POST /api/v1/config` (zone table + set), `POST /api/v1/name` (32 printable-ASCII bytes; the DEVICE owns this string, the hub keeps a copy) |
| WiFi | `GET /api/v1/wifi/scan`, `POST wifi/join` (SSID+pass+tz+name in one body), `wifi/ap`, `wifi/mode` (persistent reachability mode; reboots) |
| Firmware | `POST /api/v1/ota` (A/B with rollback), `POST /api/v1/reboot`, `POST /api/v1/reprov` (one-shot return to the provisioning AP, creds intact) |
| Apps | `GET /api/v1/apps`; `POST /api/v1/apps/install` `enable` `uninstall` `clear` `factory` |
| SD / themes | `GET /api/v1/sd`; `POST sd/app` `sd/theme` `sd/bundle` `sd/delete` `sd/rescan` |
| Debug only (`CONFIG_ORB_DEBUG_HTTP`) | `GET /api/v1/screen` `/api/v1/fb` — framebuffer grabs for the bench |
| Captive-portal probes | `GET /generate_204` `/gen_204` `/hotspot-detect.html` `/connect/test.php` `/favicon.ico` |

Handler registration is checked, not ignored: exceeding `max_uri_handlers`
logs and fails loudly rather than silently 404-ing the last route. Discovery
is mDNS `_orb._tcp` (`main/net/orb_mdns.c`).

## Storage model (`partitions.csv`)

16 MB flash. The two invariants at the top of the file are law:

1. **Retention**: app settings and WiFi credentials live on the separate
   `storage` NVS partition (namespaces `orb.net`, `orb.settings`,
   `orb.grants`, `orb.apps`, `orb.app<slot>`), never in esp_wifi's
   `nvs.net80211`. The boot erase-recovery can only ever eat the driver's
   cached profile + PHY cal.
2. **The S3 cannot OTA into the slot it runs from**: `ota_0`/`ota_1` at 2 MB
   each, code only. No `factory` partition on purpose — the bootloader seeds
   otadata to `ota_0`.

| Partition | What lives there |
|---|---|
| `nvs` (0x9000) | esp_wifi's own — disposable |
| `otadata` / `phy_init` | boot bookkeeping |
| `ota_0` / `ota_1` (2 MB each) | app images, A/B with rollback |
| `assets` (0x420000, 8 MB) | the ACTIVE theme slot (Migration 1: one slot replaced the A/B pair). Header +0, geometry +0x1000, art banks +0x10000. mmap'd by `art_bank.c`. Lookup is always by LABEL. |
| `www` (0xC20000) | raw pre-gzipped PWA shell bytes, served `Content-Encoding: gzip` |
| `storage` (0xD60000, 24 KB) | all app/config NVS — the retention partition |
| `apps` (0xD80000, ~2.5 MB) | the install ledger: catalog rows, manifest copies, geometry blobs. Art payloads do NOT live here. |

Gotchas pinned in the CSV comments: blank SubType columns must stay PRESENT
(gen_esp32part.py parses positionally); offsets are 64 KB-aligned so
`esp_partition_mmap` wastes no MMU entries; a slot smaller than the image
turns every upgrade into `ESP_ERR_OTA_VALIDATE_FAILED`; and `tools/serflash.sh`
pushes only app + otadata, so a changed partition table needs one full
`idf.py flash` at 0x8000 to reach the board.


## Theme pipeline

Art is baked on the bench; the device never decodes PNG or JSON.

- **Source of record is a PNG frame sequence** (or GIF with `--bg-gif`). A GIF
  re-quantises to 256 colours before the bake sees it (~68/255 per-channel
  loss vs ≤7 from PNG); a baked bank is final RGB565 output and is never a
  re-bake input.
- `tools/bake_clock.py <theme_dir>` produces the FORMAT 2 clock bank;
  `tools/watch-composer/` is the browser dial composer whose export is a
  FORMAT 3 `.orbtheme`.
- A `.orbtheme` bundle (`tools/orbbundle.py` writes it, `main/art/orb_bundle.c`
  reads it): 128-byte header, geometry at +128, N fixed 40-byte section
  records, self-describing blobs, art banks sector-aligned at the end. One
  file holds N per-app sections addressed by slug.
- A device slot is laid out DIFFERENTLY from a card bundle. The installer
  rebases at run time (`orb_bundle_rebase()`); `tools/mk_slot_image.py` does
  the same move offline to produce a flashable default.
  `tools/rebake_bundle.py` is the FORMAT 2→3 reverse path.
- The flashed default is `assets/orb_default.img`, written into `assets` by an
  `esptool_py_flash_target_image` rule in the top-level `CMakeLists.txt`.
  `idf.py flash` writes it too; `tools/serflash.sh` does not.
- **Three-way rule**: for every format, the Python writer, the shipped C
  reader, and the host parity suite move in ONE commit or the test fails.
  `tools/host_orb_bundle_c_test.py` feeds the shipped C reader the writer's
  own bytes under host gcc and demands identical verdicts AND identical
  refusal sentences.
- Always check the baker's `--dump-dir` `stale_NN.png` before flashing: it
  must be solid black. Any lit pixel is the panel's canvas holding another
  frame's art.

## Host test discipline (`tools/host_*.py`, 26 suites)

Pure functions — parsers, install decisions, geometry math, timezone offset,
the grant gate, the carousel ladder — are lifted **verbatim** out of shipped
`.c` files (no copies, so nothing can drift), compiled with host gcc against
small shims, and driven with hand-authored byte fixtures. No board, no
idf.py, seconds per suite. Start with:

    python3 tools/host_tz_test.py             # net parser + tz math
    python3 tools/host_svc_test.py            # the broker gate + quota
    python3 tools/host_orbapp_test.py         # manifest format
    python3 tools/orb_version.sh --self-test  # the version composer

The bench suites (`tools/bench_*.sh`) run on hardware and follow the detached
launch rule in `.clinerules`.

## Tooling

- **`tools/orb-manager/`** — the hub: one static Go binary that onboards,
  flashes, provisions, themes, and manages orbs on the LAN. Its own ROM
  bootloader protocol (`internal/esprom/`): SLIP framing, `USB_JTAG_SERIAL`
  download-mode entry, `FLASH_BEGIN`/`FLASH_DATA` — no esptool, no Python, no
  IDF on the user's machine. Embeds its web UI and the firmware bundle.
  Safety: one writer per port (lock keyed to the canonical device path), an
  operation registry with cancel, and a token-authenticated local HTTP
  surface (`X-Orb-Token` / `orb_token` cookie minted per launch) — loopback
  alone is not a defence against cross-site POSTs.
  `tools/orb-manager/lonely/` is a separate tiny ESP-IDF project: the image a
  wiped orb gets instead of a boot loop ("Flash me" bounce; no WiFi, no NVS,
  no command door) so the chip always enumerates as an ordinary CDC device.
- **`tools/serflash.sh`** — bench serial flash. Never positional: the port
  comes from `.orb-target` (`tools/orb_bind.sh`), the MAC is confirmed before
  writing, a flock refuses a second writer, the version stamp in the built
  `.bin` is audited against the working tree, and the last two images per orb
  are kept in `/tmp`.
- **`tools/watch-composer/`** — browser dial composer; exports FORMAT 3.
- **`tools/orbapp.py`** — `--inspect` / `--disclose` a `.orbapp` container.
- Versioning: `tools/orb_version.sh` is the single composer of the
  `esp_app_desc_t.version` stamp (`v<MA>.<MI>.<PA>[-pre]+<sha>[-dirty]`,
  31-byte hard cap). `version.txt` holds the declared part. cmake stamps at
  CONFIGURE time — bump, then `idf.py reconfigure && idf.py build`, or the
  image carries new code under an old stamp. Full grammar:
  `docs/firmware-versioning.md`.


## Kconfig surface (`main/Kconfig`)

Notable, with the `sdkconfig.defaults` values: `ORB_NET_ENABLE=y`,
`ORB_NET_HTTPD_PORT=80`, `ORB_NET_AP_FALLBACK=y`, `ORB_NET_MEM_DEBUG=y`,
`ORB_DEBUG_HTTP=y` (bench door), `ORB_SD_ENABLE=y` (20 MHz SPI, 30 s retry),
`ORB_IDLE_DIM_*`, `ORB_IDLE_MOTION_*`, `ORB_APP_POWER_MAX_S`,
`ORB_ART_ARENA_MARGIN_KB`. Compiler: release/-O2 — the LVGL software render,
not QSPI, is the hot path. The LVGL pool is pinned to 24 KB
(`CONFIG_LV_MEM_SIZE`) with the measured-demand rationale in
`sdkconfig.defaults`: peak ~19.8 KB; if a screen pushes it past ~21 KB LVGL
returns NULL and the widget fails silently, so re-measure via the
`lv_mem_monitor` numbers in `/api/v1/status` before growing it. FatFS needs
`LFN_HEAP` or theme-kit names get 8.3-mangled. A new Kconfig `default y` does
NOT materialize into an existing local `sdkconfig` — track the value in
`sdkconfig.defaults` or the handler silently compiles out.

## Extending the project

1. **Read the plan docs first.** `docs/app-platform-plan.md` is the master:
   §0 carries phase status and the ruling log (D-numbers); rulings bind the
   code, and code comments cite them. New decisions get new numbered sections
   there.
2. **New subsystem:** a dir under `main/`, registered in
   `main/CMakeLists.txt` `SRCS` with a why-comment (that file is a log of
   reasons, not a list), and its `REQUIRES` entry when it pulls an IDF
   component. Respect task affinity: widgets are touched on the LVGL task
   only; blocking work goes through `orb_jobs`.
3. **New endpoint:** an `h_*` handler in `orb_net.c` + one row in `uris[]`.
   Keep any body parsing a pure bounded scanner like `json_find` so a host
   suite can lift it, and pin the negative cases there (escaped quotes,
   control bytes, overlong values, decoy keys).
4. **New app:** author a `.orbapp` (manifest declares settings, grants,
   `period_ms` floor). A wasm guest needs no per-app C code —
   `orb_app_wasm.c` is one iface for N instances. A compiled-in app follows
   `color/` and registers through `orb_app.c`'s table; since D36 the only
   compiled row is the shell, and every other app — the clock and the picker
   included — arrives as a container through the hub's library.
5. **New theme/dial:** never hand-write art bytes — compose in
   `tools/watch-composer/` or bake with `tools/bake_clock.py`, then install
   through `/themes`, the hub, or the SD drop zone. Touch a format's writer,
   reader, and parity suite in one commit (three-way rule).
6. **Validate before flashing:** the relevant `tools/host_*.py` suites, then
   `idf.py build`; flash via `tools/serflash.sh` (stamp-checked) or the hub.
7. **Version any shipped image:** bump `version.txt`, `reconfigure`, build.

12. `orb_svc_boot_done()` — moves the boot-reset record from ARM to RUN.
