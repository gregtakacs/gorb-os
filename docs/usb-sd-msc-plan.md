# USB Mass-Storage (TinyUSB) access to the enclosed SD card

**Status: SHELVED — designed, validated against hardware docs, not started.**
This is a complete, self-contained implementation plan. Pick it up cold and it
should be enough to build from. Nothing here is implemented yet.

---

## 1. Goal / user story

The microSD (TF) slot and both side buttons (PWR, BOOT) on the
Waveshare ESP32-S3-Touch-AMOLED-1.75 are **inside the enclosure** and not user
accessible. The only real user input is the **rotary knob**.

We want to:

1. Expose the SD card to a host PC **over the existing USB-C cable** (TinyUSB
   MSC), so theme files / assets can be drag-dropped onto the device without
   opening the case or ejecting the card.
2. Provide a **knob-driven menu option** that disables the MSC drive and hands
   the port back to **USB-Serial-JTAG**, so the port can be used for
   `idf.py flash` / monitor.

## 2. Why this exact design (the constraints that force it)

Verified from the Waveshare board docs, this repo's `sdkconfig`, and the
original Arduino `orb-os` SD code (`~/docker/Projects/orb-os/src/sdcard.cpp`,
`docs/ARCHITECTURE.md`).

| Fact | Detail | Source |
|---|---|---|
| One USB-C, native USB | Single USB-C wired to ESP32-S3 **native USB on GPIO19 (D−) / GPIO20 (D+)**. **No UART bridge chip** (CH340/CH9102 not in the board's feature list). | Waveshare docs; on-device: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, `UART_NUM=-1`, only `/dev/ttyACM0` exists |
| USJ vs TinyUSB | **Mutually exclusive** — same PHY, same D+/D−, one enumeration at a time. USB-Serial-JTAG is a **fixed-function hardware device** that "cannot be reconfigured to perform any function other than serial + JTAG". So console↔MSC is a **boot-time either/or, not a runtime mux**. | ESP-IDF USB-Serial-JTAG + USB-Device docs |
| Recovery trap | esptool needs the **ROM bootloader on USJ**. Reaching ROM normally needs the BOOT button (**enclosed**). TinyUSB's own CDC can't substitute (the ROM never speaks it). → If firmware ever comes up stuck in MSC with the menu unreachable, recovery would mean opening the case. The design below makes that **impossible**. | this plan |
| SD wiring | **SPI-mode only, 1-bit.** MOSI=**GPIO1**, SCK=**GPIO2**, MISO=**GPIO3**, CS=**GPIO41**. **D1/D2/card-detect NOT wired** → no 4-bit SDIO, no hot-plug sense. These GPIOs are free in this IDF port (`board_config.h` claims none of 1/2/3/41). | `orb-os/src/sdcard.cpp` + Waveshare schematic note |
| SD speed | Original orb-os **measured ~1.5 MB/s** on the card's SPI bus. Their verdict: SD is fine for install/staging, **never** for a streaming/hot path. | `orb-os/docs/ARCHITECTURE.md:194` |
| SPI host | LCD owns **`SPI2_HOST`**. SD must come up as **SDSPI on `SPI3_HOST`**. | `main/display/display.c:261` |
| Idle core | Only one busy task (`lvgl_task`, unpinned) → effectively occupies one core; the QSPI GDMA + knob work are ISRs. **Core 1 is idle** → pin the TinyUSB task there so MSC doesn't steal render cycles. | task topology analysis |
| Power topology | Board has an **AXP2101 PMU + MX1.25 Li-po header + PWR button**. If a battery is fitted, the S3 **keeps running through a USB replug** → a *persistent* MSC mode would not clear on replug. This is why the flag must be self-clearing (below), not toggled off from the menu. | Waveshare docs |

### Perf expectation (this is a convenience path, not a bandwidth path)

USB is Full-Speed (12 Mbps); Espressif's own S3 MSC numbers @8192 B FIFO are
**~0.925 MB/s read / ~0.928 MB/s write**. The USB link is the limiter (card is
~1.5 MB/s). A few-MB theme folder ≈ a few seconds. Fine for asset loading.


## 3. The safety invariant (the crux of the design)

> **Console is always the resting state. MSC is non-sticky and opt-in. Any
> reset returns the device to flashable console mode.**

Achieved with a **one-shot NVS flag that clears itself at the top of boot**,
before anything USB runs. It is impossible to get stuck in MSC, so the knob
menu is only ever needed to go *toward* MSC — never to get out of it. The
enclosed buttons never have to be reached.

## 4. Proposed design

### NVS key `usb.mode` (namespace e.g. `sys`), two values
- `0 = CONSOLE` (**default / resting**) → USB-Serial-JTAG console + flashing,
  exactly like today. (TinyUSB never claims the PHY → USJ auto-enumerates.)
- `1 = MSC_ONCE` → boot into TinyUSB MSC (SD-backed) **for this boot only**.

### Boot flow (in `app_main`, very early — before USB/peripheral init)
```
mode = nvs_get("usb.mode", default 0)
nvs_set("usb.mode", 0)        // CONSUME-AND-CLEAR FIRST, unconditionally
if (mode == MSC_ONCE && sd_mount_ok()) {
    start tinyusb composite { MSC(LUN0 = SD over SDSPI) , CDC-ACM } , task pinned to core 1
    start 10-min idle auto-revert timer -> esp_restart()   // belt-and-braces
    // optional splash: "USB drive active — copy files. Replug/PWR-cycle to exit."
} else {
    // do nothing special: USJ console enumerates automatically
}
```

### Rules that keep it safe
- **Mount-failure fallback:** if the SD card fails to mount, **do not** start
  MSC — boot to console instead. Never enumerate a broken/empty MSC LUN (the
  host can't repair that state; the device must).
- **Composite MSC + CDC-ACM** so we still get serial logs during the MSC
  window (logs and flashing are different needs; flashing still requires
  console mode).
- **10-minute idle auto-revert** → clear flag + `esp_restart()` into console,
  so a battery-powered device left in MSC can't sit there forever.
- **TinyUSB task → core 1** (set affinity in `tinyusb_config_t`).
- **Normal recovery = PWR-cycle** (not just USB replug) when a battery is
  fitted; plain replug suffices USB-only. Either way the self-clearing flag
  already guarantees console on next boot.

### Knob UI
Add a 4th router screen (e.g. `INPUT_SCREEN_USB`) in `input_router.c` using the
existing `set_screen()` + long-press cycle pattern (currently
color → clock → orb). On that screen, a knob **press** sets `usb.mode=MSC_ONCE`
then `esp_restart()`. Show status text: enable-drive instructions, expected
copy speed, and the exit gesture (replug / PWR-cycle).

## 5. Implementation steps (in order)

**(a) SD bring-up in ESP-IDF — independent, no USB touched.**
Port the SD mount using the ESP-IDF **SDSPI** host on **`SPI3_HOST`**, pins
MOSI=1/SCK=2/MISO=3/CS=41, 1-bit width, negotiate clock down
(20→10→4 MHz like the original). Use `esp_vfs_fat_sdspi_mount()` (or manual
`sdspi_host_init_device` + `sdmmc_init_slot` + `sdmmc_card_init`) for a
read-only FATFS listing logged at boot. De-risks "does the card even init",
which would change the plan. Mirror the original's speed-negotiation +
fallback behavior (`orb-os/src/sdcard.cpp`).

**(b) TinyUSB MSC against the SD card.**
Add managed dep `espressif/esp_tinyusb` (`idf.py add-dependency
"espressif/esp_tinyusb^2"` — pin the version; APIs differ v2 vs v3). Build a
**forced-MSC dev image** with an SD-backed LUN and confirm a host PC mounts the
drive and reads files. Check the component's actual API at build time — the IDF
v5.4 doc shows `tinyusb_msc_storage_init_sdmmc(&cfg)` while the esp_tinyusb v2.x
registry shows `tinyusb_msc_install_driver()` + `tinyusb_msc_new_storage_sdmmc()`
with `cfg.medium.card`. Set `CONFIG_TINYUSB_MSC_BUFSIZE=8192`. Add the
`TINYUSB_MSC_EVENT_*` storage callback (MOUNT/FORMAT_REQUIRED/FAILED).

**(c) The one-shot handshake + menu + composite + auto-revert.**
Wire `usb.mode` per §4, add the USB screen to `input_router.c`, MSC+CDC
composite, TinyUSB task on core 1, 10-min idle reboot-to-console.

**(d) Brick-resistance verification.**
From a running MSC session, **PWR-cycle** the board (with battery if fitted)
and confirm it lands back on `/dev/ttyACM0` as a console and **flashes**. Also
confirm SD-mount-failure falls through to console. This is the acceptance test
for the §3 invariant.

## 6. Open questions to resolve before starting

1. **Power topology:** is the device USB-powered, or is there a battery on the
   MX1.25 header? Determines the documented exit gesture (replug vs PWR-cycle)
   and whether the auto-revert timer is load-bearing. (Doesn't change step (a).)
2. **Accept 1-bit SPI SDSPI on `SPI3_HOST` (~0.9 MB/s via USB)?** Hardware
   forces this (no 4-bit wiring); fine for asset loading, not for streaming.
3. **Filesystem / LUN layout:** expose the raw card as one FATFS LUN (simplest,
   matches how orb-os themes are laid out), or a smaller dedicated partition.

## 7. Key references

- Waveshare board docs: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.75>
- esp_tinyusb component (MSC/SD LUN, composite, perf tables):
  <https://components.espressif.com/components/espressif/esp_tinyusb>
- ESP-IDF USB-Serial-JTAG guide (fixed-function, USJ-vs-OTG PHY, Limitations /
  USB Pin Reconfiguration):
  <https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-guides/usb-serial-jtag-console.html>
- ESP-IDF USB Device Stack (MSC storage init, examples incl.
  `tusb_msc`, `tusb_composite_msc_serialdevice`):
  <https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32s3/api-reference/peripherals/usb_device.html>
- Original SD code + measured throughput rationale:
  `~/docker/Projects/orb-os/src/sdcard.cpp`, `~/docker/Projects/orb-os/docs/ARCHITECTURE.md`
  (§"Pre-baked art", "Storage and persistence")

## 8. Relevant existing files in this repo

- `main/board_config.h` — pin map; add SD pin defines here (MOSI/SCK/MISO/CS = 1/2/3/41).
- `main/main.c` — `app_main`; the one-shot USB-mode boot decision goes here, early.
- `main/input/input_router.c` — screen enum + long-press cycle; add the USB screen.
- `main/knob/knob.h` — knob take_* primitives for the menu press gesture.
- `main/display/display.c` — owns `SPI2_HOST` (why SD must use `SPI3_HOST`).
- `main/idf_component.yml` — add `espressif/esp_tinyusb` dep.
- `partitions.csv` — unaffected unless a dedicated MSC FATFS partition is chosen (open Q3).

## 9. Related-but-separate conclusions already reached (don't re-litigate)

- The **orb_bg frame bank stays in flash rodata** (embedded via
  `target_add_binary_data`), *not* streamed from SD — SD's measured ~1.5 MB/s
  caps a 434 KB/frame animation at ~3.4 fps and the 8.66 MB bank doesn't fit
  the 8 MB PSRAM anyway. Flash is the only place all 20 frames live.
- SD's sensible role, if ever needed, is as an **install/staging source baked
  once into a data partition** (mirroring orb-os `theme_art_bake.cpp`), not as a
  per-frame read source.

