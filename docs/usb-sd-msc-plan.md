# USB access to the enclosed SD card

**Status: SHIPPED as MSC disk sharing (§14). MTP is gone (§14).**

Read §14 first. It is the current state. §1–§11 are the MSC plan and its board
log: kept because they record the hardware findings that survive the class change
(one PHY, one switch, §6.1; the reconnect pulse, §11; the throughput ceiling, §9),
and because §10 is the record of what the first pass got wrong. §12 documents the
inversion to MTP while MSC still existed beside it; §13 supersedes its §12.1,
§12.4 and §12.6, which describe a two-mode build that no longer compiles; and §14
supersedes §13's headline, which shipped a class that never worked on hardware.

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

## 10. What shipped (D36, 2026-10-23) — deltas from the plan above

Built both ways (feature on and off) and host-verified on IDF v5.4 /
esp_tinyusb 3.x. Runtime copy throughput and host-mount behaviour are
unmeasured: no board is attached to this box.

- **NVS names.** Partition `storage`, namespace `orb.os`, key `usb_mode` (u8).
  The plan's `sys` / `usb.mode` would have been the only namespace in the tree
  that is not `orb.*`. Read-and-clear is unchanged, and is still the
  load-bearing safety property: an MSC boot that dies mid-copy comes back as a
  clock, not as a disk nobody asked for.
- **MSC only, no CDC-ACM.** The plan's composite descriptor is not built. A CDC
  port would restore the console at the cost of a second class behind the one
  task that must never block (§6.1). The console returns at reboot; the log
  during MSC mode goes to a void, the same way it does while a host flashes the
  board over USJ.
- **Exit gesture is the timer or the dial, never a replug.** A battery on the
  MX1.25 header keeps the S3 alive through a replug, so open question 1 resolved
  against replug: the revert timer ejects and reboots
  (`orb_jobs_reboot_soon`), and the menu's Eject row does the same on demand.
- **Boot flow moved later, deliberately.** The one-shot is consumed ahead of the
  storage ladder, so the boot can skip the mount on the strength of a flag it has
  already erased. The start is then *posted to the worker* after the display
  ladder rather than run inline in `app_main`: a raw card init behind a command
  timeout and a TinyUSB task creation are the two slowest things the mode does,
  and the dial must be drawing before either, because an MSC boot has no console
  and the row's state line is the only account of what happened.
- **Open question 3 answered: raw whole card, one LUN, no dedicated partition.**
  FATFS stays unmounted on our side for the whole mode —
  `orb_sd_hand_to_usb()` attaches the raw card and `mount_now()` refuses while
  the host owns it — which is what keeps the volume clean when the host
  enumerates it. We never format: `FORMAT_REQUIRED` logs a refusal and leaves the
  format to the host's own disk utility.
- **No new screen.** §4's knob UI is a *service row* in the menu rather than an
  entry in `input_router`'s screen enum: a row with options and no face, outside
  the app ledger's index span (`ORB_APP_MENU_BASE`), invisible to
  `orb_app_find_slug()` and to the carousel walk, and refused by
  `orb_app_activate()`. It gets no NVS fault words, no art bank and no carousel
  slot, which is the whole reason it is not a fourth behaviour kind.
- **esp_tinyusb 3.x API facts, re-derived the hard way.** Write-protect has no
  config field and no setter: the MSC class asks the weak
  `tud_msc_is_writable_cb(lun)` on every MODE_SENSE, so the application defines it
  strongly. There is no `tud_mount()`; `tinyusb_msc_storage_config_t` carries a
  mount point instead, and `TINYUSB_MSC_EVENT_MOUNT_COMPLETE` fires for a mount
  *and* for an unmount, so the only question that tells them apart is
  `tinyusb_msc_get_storage_mount_point()`. `TINYUSB_DEFAULT_CONFIG` is variadic
  and lives in `tinyusb_default_config.h`, not `tinyusb.h`.
- **One guard the plan did not have.** `orb_app_index_of()` answers a service row
  at `ORB_APP_MENU_BASE` (11), which `orb_svc.c`'s `ORB_SVC_ROWS` (16) leaves room
  for. Nothing enumerates services into the broker today, so `row_of()` now
  refuses by predicate instead of relying on nobody asking.
- **Descriptor strings are the component's defaults** ("Espressif Device"). A
  product string of "orb" is a one-line `sdkconfig.defaults` change, left undone
  because it is not what this feature was for.

---

## 11. First board run (2026-10-24): the hand-over needs a bus pulse

Step 2 of §10's validation list got as far as the row reading "drive ready" with
nothing on the host — not in Windows, and so not in its WSL guest either. Cause:
`orb_usb_msc_start()` reported its own success and nothing else. Two shipped
changes, both in `orb_usb_msc.c`:

- **`reconnect_pulse()`, called last in the start ladder.** The port enumerated
  at boot as the USB-Serial-JTAG device with D+ already pulled up.
  `tinyusb_driver_install()` moves the shared FSLS PHY to the USB-OTG controller
  through `usb_new_phy(USB_PHY_CTRL_OTG, USB_PHY_TARGET_INT)` — RTCCNTL
  `sw_usb_phy_sel`, see `usb_wrap_ll_phy_enable_external` — and the pull-up
  survives the move. A device stack that only comes up in device mode therefore
  shows the host no gap on the line, the host keeps the JTAG device instance it
  made at boot, and the MSC descriptor is never read. `tud_disconnect()` →
  150 ms → `tud_connect()` is what makes the takeover visible. `dcd_init()` ends
  with a disconnect/connect pair of its own, but the two are back-to-back
  register writes: microseconds, far under the ~100 ms Windows debounces.
- **The row now says what it knows.** `TINYUSB_DEFAULT_CONFIG(NULL)` threw away
  the event callback, so ATTACHED and DETACHED were dropped and "drive ready"
  claimed an enumeration that had not happened. `h_usb()` takes them: the row
  reads **"drive up, no host"** until SET_CONFIGURATION arrives and **"drive
  ready"** after it, and the revert window re-arms from full at that moment
  rather than running down while nobody holds the card.

Two facts recorded for the next read of §6.1, which claimed the hand-over needed
no code: the console dies when the PHY mux moves, by hardware, and
`usb_serial_jtag_driver_uninstall()` must NOT be called on the way in —
`usb_serial_jtag_read_bytes()` has no NULL guard, and `orb_net.c`'s
`prov_reader_task` can be parked inside a blocking read on the ring buffer that
call frees.

## 12. D37: the same seam serving files instead of sectors

The first hardware pass answered the plan's question and invalidated the plan's
premise. The owner's use is pushing theme and app kits on and deleting old ones —
interactive, write-dominant, both directions — with no case for reading a file off
the card at all. MSC is the wrong tool for that: it unmounts the volume it exports,
which starves the dial of the themes it is drawing, and it has no delete. MTP is the
right one. §4.3's rejection ("MTP would need a DB, a second owner of the card, a
task and a larger RAM bill") was argued against the wrong premise: esp_tinyusb's
MTP class **has** no database, and the second owner it implies is not a second
owner but the *same* owner with two doors into one mount. That is the whole
correction, and it is why this is a build-time choice rather than a second feature.

### 12.1 The inversion, in one table

| | MSC (§1–§11) | MTP (D37) |
|---|---|---|
| What the host gets | one LUN, raw sectors | the files under `/sd` |
| Mount for the window | unmounted, and re-unmounted by every probe | mounted, and held mounted |
| Console | dead (§6.1) | dead (§6.1) — unchanged |
| Writes | refused by default | the point of the exercise |
| Host sees a change the orb made | only after re-plug | on the next paint |
| Entry / exit | reboot in, reboot out | reboot in, reboot out — unchanged |
| Drive letter | yes | **no** |

The row is the same row (`usb-drive`, "USB Drive", one service-tier slot,
`ORB_SVC_ROWS` slack untouched). `choice ORB_USB_MODE` in `main/Kconfig` decides
which class backs it, and each module's own `#if` keeps the other an empty
translation unit. A composite build is refused structurally, not by convention:
MSC handing a host raw sectors of the volume MTP is writing through FatFs is two
owners of one filesystem, which is the exact thing §8's seam exists to prevent.
Default is MTP. `ORB_USB_MSC_REVERT_S` became the shared `ORB_USB_REVERT_S`.

### 12.2 Two seams, opposite directions

D36 added `s_usb_owns` to `orb_sd.c`: while set, `mount_now()` refuses. D37 needs
the inverse flag, not the same one under a new name — an MTP window that reused
`s_usb_owns` would have `mount_now()` refuse the very mount it serves from:

- `orb_sd_share_begin()` / `_end()` / `_held()`, beside the existing claim.
- `orb_sd_remount()` returns `ESP_ERR_INVALID_STATE` while held, checked on its
  own line with its own reason. A rescan issued over WiFi mid-copy unmounts the
  volume under MTP's open handles, and the host's next read lands on a card that
  has been re-enumerated underneath it.

The guard sits *before* the card lock, keeping D36's ordering: a route that blocked
on the lock first would look like a hung card rather than a claimed one, and the
lock serialises the claim transitions against the mount-table mutations anyway.

### 12.3 No console, and still no way to ask for one back

§6.1 is unaffected by the class: the PHY handover is silent on the line, the host
re-reads the descriptor only after `reconnect_pulse()`, and the console returns at
the next boot and not before. What MTP adds is one option MSC did not have and does
not get — a `CONFIG_TINYUSB_PM` power-management task, which is the component's own
route to bus events, not a console. It stays off.

### 12.4 Liveness: the signal changed, and the timer got better

`TINYUSB_EVENT_ATTACHED` is raised from `tud_mount_cb` (`tinyusb.c:62`) — MSC's
mount callback. It never fires for MTP, so the state D36 derived from events is now
derived from two class-independent things:

- **`tud_mounted()`** (`_usbd_dev.cfg_num != 0`, set by `SET_CONFIGURATION`, cleared
  on detach). The honest answer to "is there a host", and it is *sampled*, not
  signalled: the component defines `tud_mtp_command_received_cb` strongly
  (`mtp/tinyusb_mtp_transport.c:277`), so there is no per-operation hook to take.
  The sample point is `orb_usb_mtp_host_up()`, called from the row's `opt_value`,
  so the poll rate is the menu's own repaint — at least once a second while the
  mode is up.
- **Suspend and resume**, which fire for any class. Resume re-arms the revert
  window; suspend deliberately does not. That asymmetry is the improvement over
  D36: a laptop with its lid shut lets the orb go, and one that wakes and resumes
  the copy gets the whole window back. `ORB_USB_REVERT_S`'s help text carries it.

Still no re-arm on a data transfer, for the same reason as D36: a copy that resets
its own watchdog never expires, which is the bug §7's timer exists to prevent.

### 12.5 Two hazards MTP creates that MSC could not

**`max_files`, and it is a real one.** The mount config's `.max_files` caps open
files for the whole VFS registration, and MTP's handlers `fopen()` into that same
registration from the TinyUSB task while the dial reads. Four slots was a budget
for one user; a host copying one file while browsing one directory now spends two
against a dial that can need four, and the failure is `EMFILE` on a theme that is
plainly present. Raised to 10 in `orb_sd.c`, with the reasoning in the comment
there rather than a number that looks arbitrary in review.
`CONFIG_FATFS_FS_LOCK=0` (already the build's value) is what makes this a slot
ceiling rather than a sharing violation — and it is also why a host can `unlink` a
file the dial is mid-read of. That survives because every `/sd` path opens, reads
or writes, and closes inside one call; it is why live-editing a file the dial is
parsing is unsupported rather than merely inadvisable.

**`esp_vfs_fat_info()` is on the TinyUSB task.** `GetStorageInfo` stats the base
path, which takes FatFs's per-volume object. That is a lock the TinyUSB task takes
while the card lock may be held by an upload. Neither path takes the other's lock,
so there is no ordering violation, but it is a new coupling and a hang here looks
like a dead host connection rather than like a lock.

### 12.6 Built three ways, and the console question answered again

Built all three configurations with `-Werror=all`:

| Configuration | Result |
|---|---|
| `ORB_USB_MODE_MTP` (shipped default) | builds; `CONFIG_TINYUSB_MTP_ENABLED=y` and `CONFIG_TINYUSB_MSC_ENABLED` **off**, so the descriptor carries no dead MSC interface |
| `ORB_USB_MODE_MSC` | builds, behaviour unchanged after the `ORB_USB_REVERT_S` rename |
| `ORB_USB_MODE_NONE` | builds, no row, `orb_usb_mtp_boot_claim()` stub answers false |

`tools/host_app_registry_test.py`, `host_menu_test.py` and `host_sd_test.py` pass.
The first two link a stub row, which is the point: they test the registry's
treatment of the tier, not the class behind it.

One build-config trap worth recording: `CONFIG_TINYUSB_MSC_ENABLED` is not reset by
the `choice`, because `select` only forces a symbol *on*. Switching an existing
`sdkconfig` from MSC to MTP leaves MSC enabled unless it is cleared by hand, and
the result is a composite descriptor whose MSC LUN has no driver — a disk that
answers nothing. `# CONFIG_TINYUSB_MSC_ENABLED is not set` in `sdkconfig`, or delete
`sdkconfig` and let `sdkconfig.defaults` regenerate.

### 12.7 On the orb — the console caveat is unchanged

The mode ends the console, so start the monitor and flash in one command before
arming it: the arming press happens on the dial, the reboot that follows takes the
console away, and the row is the only interface from then until the window closes.
The expected sequence on a build with `ORB_USB_MODE_MTP`:

```
# Menu → USB Drive → press "Restart to share files"
#   I orb_usb_mtp: armed for the next boot (ESP_OK)  → reboot in 2 s
# next boot:
#   W orb_usb_mtp: file-share boot claimed — /sd mounts as normal
#   I orb_sd: mounted <cid> SDHC 29.7GB @40000kHz   ← the card is LIVE, not claimed
#   W orb_usb_mtp: serving /sd over MTP — the card stays mounted, …
#   I orb_usb_mtp: host has configured the device — files are reachable
#   row reads: host connected, 597 s left
#
# Windows: "orb" under Portable Devices, NOT under This PC. If it is missing
# entirely, unplug and re-plug: the first enumeration after the PHY handover is
# still the JTAG one (§6.1) and the pulse does not always win that race.
#
# Row now reads "Stop sharing, restart" — press it:
#   W orb_usb_mtp: sharing stopped — restarting normally so the console comes with it
```

There is no `orb` CLI in this repo, and the config page has no USB card yet. Both
are worth adding and neither changes the mode: the row is the floor, and it works
with no WiFi, no host and no host-side tool.

### 12.8 The owner's checklist, rewritten for files

1. **One mode per build, structurally.** Build with `ORB_USB_MODE_MSC` and the MTP
   module is an empty translation unit; the reverse holds. No composite binary can
   be shipped by accident.
2. **Push a kit and watch it appear.** Copy `foo.orbtheme` into `themes` over MTP.
   It appears in the theme list on the next paint with no rescan, and
   `GET /api/v1/fs/themes` returns it over the round trip it took before.
3. **Pull the cable mid-copy — the one this mode exists to be better at.** Yank the
   cable during a large upload. The orb must still be mounted and functional, and
   must never show a half-written kit: a host write lands in a temp file and is
   `rename`d into place (`mtp/tinyusb_mtp_write.c:114`), the same guarantee
   `orb_theme.c`'s header-last commit gives from the other side. The old version of
   an overwritten file is the one still present. MSC could not make this promise.
4. **Delete a theme the dial is displaying.** The dial falls back to the default
   with the words it already uses for a card it cannot read — no crash, no hang.
5. **`max_files` under load.** Copy a directory while the theme page is open and a
   photo kit is loading. The failure to watch for is `EMFILE` on a file that is
   plainly present: that means 10 was not enough, and the number in `orb_sd.c` is
   what changes — not a retry loop bolted onto the theme loader.
6. **The unmount refusal holds.** While the mode is up, `POST /api/v1/sd/rescan`
   answers `mounted:false` and the log says why; after it exits the call works
   normally. A rescan that *succeeded* mid-window is a bug in
   `orb_sd_share_held()`, and the one bug here that can corrupt a card.
7. **The timer, three ways.** Idle with no host: reboots at the cap. Host
   configured, then the laptop sleeps: reboots, because suspend does not re-arm.
   Host configured and copying slowly past the cap: survives, because resume
   re-armed it.
8. **No console, same as MSC.** The row is the only interface during the window and
   works with no WiFi and no host present.
9. **Power cut mid-window.** Comes back as a normal boot with a clean mount — the
   one-shot is read-and-clear.
10. **No card in the socket.** `orb_usb_mtp_start()` answers `ESP_ERR_NOT_FOUND`
    and the row reads `failed: no card in the socket`. D36 could export an
    unformatted card; this mode cannot and should not.

### 12.9 Why this stays optional, and what it is not

`ORB_USB_MODE_NONE` remains the right setting for a sealed unit shipping without a
card: the mode's value is the card, and an orb with nothing in the socket gains a
menu row and a PHY that can be moved off the console for no reason.

Not a general-purpose file server, and not a way to edit a kit while the dial is
drawing from it — §12.5. The declared-source rules in `orb_files.c` stay: they bound
what an *app* may reach, and were never a reason to hide an owner's own tree from an
owner's own file manager. Both limits are in the Kconfig help, the one place someone
choosing between the three modes actually reads.

---

## 13. D38: MSC deleted, and so was the timer

Two changes, both reductions. MSC's code is out of the tree, and the window has no
timeout. What ships is one USB device class, armed by a press and ended by a reboot.

### 13.1 Why MSC left rather than stayed selectable

§12 opened a `choice ORB_USB_MODE` with three arms and a default. A choice you are
never supposed to change is a maintenance liability wearing the clothes of a
feature: two source files, one shared timer, one `orb_sd` seam that only one arm
used, and a matrix of three builds to keep compiling for one class that ships.
MTP does the owner's job — push kits, delete kits, pull a log — while the card
stays mounted. MSC did the opposite of that job. So the arm was deleted rather
than defaulted: `main/usb/orb_usb_msc.c`, its header, `orb_sd_hand_to_usb()`,
`orb_sd_take_back_from_usb()`, `orb_sd_usb_owns()`, the raw SDSPI attach path and
`ORB_USB_MSC_WRITE_PROTECT` are all gone. What remains in `orb_sd.c` is
`orb_sd_share_begin/end/held()`, one bool.

Deleting the class beats disabling it in three places that matter:

- **The composite binary cannot be built.** The hazard in §12.1 was MSC exporting
  raw sectors of the volume MTP writes through FatFs. A `choice` prevented it by
  menu discipline; with one class in the tree it is prevented by the source list.
- **`orb_sd.c` has one ownership model again.** The file spent D36 and D37
  carrying two flags with opposite meanings (`s_usb_owns` refuses mounts,
  `s_share_held` refuses unmounts). One of them was about a mode nobody uses.
- **No dead interface.** esp_tinyusb's MSC class is `select`-driven, and a stale
  `sdkconfig` carried over from the D36 build keeps `CONFIG_TINYUSB_MSC_ENABLED=y`
  through a reconfigure, because nothing selects it any more and `select` never
  clears what it did not set. `sdkconfig.defaults` now pins
  `# CONFIG_TINYUSB_MSC_ENABLED is not set` for that reason. It is not a comment.

### 13.2 Why there is no revert timer

D36 shipped a countdown and D37 kept it. Both were justified by the same sentence:
there is no protocol for asking a host to hand the device back. That argument was
sound for MSC and useless here, because the thing the timer protected — a card the
orb cannot reach — does not exist in a mode where the orb keeps using the card the
whole time. An MTP window costs the orb nothing except the console.

The console is the whole cost, and a timer cannot pay it. GPIO 19/20 feed one FSLS
PHY that USB_WRAP multiplexes (§6.1), so the console returns at a boot and at no
other moment. A timeout expiry is therefore just a reboot nobody asked for, and a
surprising one: it ends a copy in progress on the theory that the owner walked
away. The agreement this mode runs on is the opposite and simpler: the port is a
console unless the owner explicitly asked otherwise, the ask is one NVS byte that
the armed boot reads and erases, and any reboot ends it. Press the row, or cycle
power. An orb left sharing in a drawer returns the next time it reboots.

Two consequences worth stating plainly:

- `h_usb()` and the suspend/resume re-arm logic went with it, so
  `TINYUSB_DEFAULT_CONFIG()` now installs with no event handler. The event that
  re-arming needed was the reason a handler existed.
- `orb_usb_mtp_timeout_secs()` and `orb_usb_mtp_revert_left_s()` are gone from the
  API, and the row's value line no longer prints a countdown. The line still calls
  `orb_usb_mtp_host_up()`, and that call is still the only thing sampling
  `tud_mounted()` — §12.4's finding that the row's repaint is the poll stands.

### 13.3 The row is named for what the host gets

`usb-drive` / "USB Drive" was tolerable while the build might be an MSC drive. With
one class it is a lie that costs an owner an hour: Windows lists an MTP device under
Portable Devices and never under This PC, so the row promises a disk letter that
cannot appear. It is now `usb-files` / "USB Files", the note line reads "Shares
files, not a disk", and the labels are "Restart to share files" / "Cancel the next
boot" / "Stop sharing, restart". The symbol is `orb_app_usb_files_row`. The service
tier it sits in is unchanged, and so is `ORB_SVC_ROWS` slack.

### 13.4 Build matrix, now two cells

| Build | Result |
|---|---|
| `CONFIG_ORB_USB_MTP=y` (shipped default, in `sdkconfig.defaults`) | MTP device, armed one boot at a time by the dial |
| `CONFIG_ORB_USB_MTP` unset | `orb_usb_mtp.c` is one function returning false; the port is only the console |

Both build clean under `-Werror=all` on IDF v5.4 / esp_tinyusb 3.x. The row appears
in the menu only in the first.

`host_svc_test` grew a service-row case while this landed: `orb_svc.c` refuses menu
rows by kind (`orb_app_is_menu_only()`) rather than by bounds, because
`orb_app_index_of()` answers `ORB_APP_MENU_BASE` (11) and `ORB_SVC_ROWS` is 16, so
an unguarded service index aliases a real app's grant row. The test hands its fake
service that index on purpose. Invert the guard and "refusing it leaves the slot it
aliases alone" fails.

### 13.5 What is left to verify on hardware

§12.8's checklist still stands, minus every line about a countdown and about the
MSC arm. In priority order:

1. Arm from the dial (Menu → USB Files → "Restart to share files"), then flash and
   monitor in one command — `idf.py flash monitor` — because the armed boot's
   console is gone and anything less loses the log.
2. Windows: expect it under Portable Devices, never This PC. Browse `/sd`, push a
   `.orbtheme`, delete one, and watch the dial's theme page change on the next
   paint without a re-plug.
3. Pull the cable mid-copy. This is the check that justifies the whole mode:
   SendObject lands in a temp file and is `rename()`d into place, so the card must
   show the old kit intact and no half-written one. MSC could not make this
   guarantee at all.
4. Press "Stop sharing, restart" and confirm the reboot returns the console and
   that the next boot is an ordinary one (the claim is already erased).
5. Confirm `sdkconfig.defaults` reached the shipped image:
   `grep TINYUSB_MSC sdkconfig` must print the `is not set` line.

### 13.6 Still open, unchanged by this pass

The MTP `DeviceInfo` Version field is the literal `"orb-os"` because the tree has no
build-time version macro. Wiring one in is a separate change with a wider blast
radius than a USB mode, and it is worth doing for the config page as much as for
---

## 14. D38: back to MSC, because MTP never enumerated a store

§13 shipped MTP and reported the design arguments, which were sound. The class
did not work. Reverted to MSC on 2026-10-24, on the owner's instruction, having
accepted the cost §12 used as its argument against MSC.

### 14.1 The failure, measured

On firmware `v1.1.0+b4482b7`, the first PTP command of any session stalls the bulk
pair, on both hosts, with a correctly formed container:

```
>> OpenSession op=0x1002   container = 20000000 0100 0210 50000000 df9b5713 ...
   OUT ok: 32 bytes accepted          (declared len=32, 32 bytes written)
   IN#0 FAIL USBError errno=32        (bulk IN stalled)
```

`mtp-detect` reaches the same conclusion by its own route: `PTP_ERROR_IO: failed to
open session`, then `LIBMTP PANIC`. Two unrelated clients failing identically put
the fault on the device. The storage was registered the whole time — the boot log
says `MTP storage registered: id=0x00010001 path=/sd total=31255953408` — so this
was never a missing volume.

Windows is exonerated and was the thing §13 blamed. The UMDF host starts,
`wpdmtsp.dll` loads, every PnP and Power operation completes `0x0`, DeviceSetup
Manager writes 35 properties, and the devnode reports `CM_PROB_NONE`. There is no
`WPDBUSENUM` child because the transport never answered a command, not because the
host declined to publish one. The `0xC00000BB` entries in the admin log are
`STATUS_NOT_SUPPORTED` on optional IRPs and are normal at that layer.

### 14.2 Two defects in the vendored class, neither fixed here

1. **No path back from a stall.** `MTP_REQ_RESET` is the recovery request, and
   `mtp_device.c:349-360` clears the endpoint stalls then returns `true` from the
   SETUP branch without calling `tud_control_xfer()` or `tud_control_status()`. The
   status stage never completes: measured as `RESET -> timeout at 2501 ms`. One
   command error therefore ends the mode until re-enumeration, and re-enumeration
   needs a reboot, because the class has the console's pins.
2. **No functional descriptor.** `TUD_MTP_DESCRIPTOR` (`usbd.h:322-330`) emits
   interface plus three endpoints and no type `0x30` PTP functional descriptor,
   confirmed in the 39-byte config on the wire, and this driver accepts commands
   only on bulk OUT. Canonical PTP clients use the class control pipe, which is why
   libmtp fails differently from Windows rather than for a different reason.

Both are upstream (`espressif/esp_tinyusb` ^3.0.0, a registry component, gitignored
and regenerated), so they are reported rather than patched here. An instrumented
build was considered and rejected before writing it: `CFG_TUSB_DEBUG_PRINTF` is
`esp_rom_printf` (`tusb_config.h:138`), which bypasses the `ESP_LOG` hook that feeds
the HTTP log ring, and the console is muxed away in device mode — so raising
`CONFIG_TINYUSB_DEBUG_LEVEL` produces logs with no reader.

### 14.3 Three things §13 asserted that hardware contradicted

- **"The heap starves the first Windows request."** The object table is static BSS,
  so `heap_internal_largest` has no bearing on it. The number that mattered (12 kB
  largest block at idle) was never the constraint.
- **"The arm persists."** It does not, and correctly so: `orb_usb_mtp.c:133` records
  it as one read-and-cleared NVS word, so `usb.state: off` after a reboot is the
  design working.
- **"`preprocess_cmd` double-encodes DeviceInfo."** It does not.
  `MTP_DEVICE_INFO_STRUCT` is `TU_ATTR_PACKED` with every array sized to its exact
  content, so `sizeof()` is the wire size and the four appended strings complete
  the dataset to spec.

### 14.4 What shipped instead

`main/usb/orb_usb_msc.c` exports the whole card as one writable LUN. The owner's
reasoning is the load-bearing part and is now recorded in the Kconfig help: the
orb has no runtime use for the card, so the host may have all of it.

- `choice ORB_USB_MODE` in `main/Kconfig` replaces the plain bool. The two modes are
  a choice rather than two checkboxes so a composite binary — a raw LUN beside an
  MTP device writing the same volume through FatFs — cannot be built at all.
  `CONFIG_ORB_USB_MTP` and `CONFIG_ORB_USB_MSC` are derived from it, so no call site
  moved. Verified in the linked image: 21 MSC functions, 0 MTP functions.
- `orb_sd_hand_to_usb()` is the inverse seam of `orb_sd_share_begin()`. It unmounts
  if the boot ladder got there first, attaches the card raw, and sets the flag that
  makes `mount_now()` refuse for the rest of the boot. The boot claim sets that flag
  ahead of the storage ladder, which is the one behavioural difference from the MTP
  claim: an MSC boot must not mount the volume it is about to export.
- The raw attach mirrors IDF's own `esp_vfs_fat_sdspi_mount` down to
  `sdmmc_card_init` and stops there (`vfs_fat_sdmmc.c:356-378`), across the same
  20/10/4 MHz ladder. The step that is easy to omit: the handle
  `sdspi_host_init_device()` returns **replaces** `host.slot` before the card is
  asked anything.
- `tud_msc_is_writable_cb()` is defined strongly, answering yes. The component's
  weak definition answers read-only, and a write-protected stick is not the feature.
- `auto_mount_off` is set. Left alone, the component remounts the volume for the
  application on USB detach, which is one automatic route back to two owners.
- The row is unchanged in slug and address (`usb-files`) and now reads **USB Drive**,
  because it is the same feature with a different class behind it.

### 14.5 Owed, not yet done

Built clean for both `esp32s3` and the `esp32` stub path, but **not yet run on a
board**: the bench device was wedged in the MTP failure at the time of the change.
The validation list §10's step 2 still owes, and the first pass should record
whether the host mounts the existing FAT volume without being prompted to format
it, which is what `do_not_format` claims.

MTP.
