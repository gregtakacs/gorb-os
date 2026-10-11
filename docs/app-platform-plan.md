# gorb-os app platform — OS/app split, storage model, measurement protocol

Status: **decided contract, no code changed yet.** Written 2026-10-11 at commit
`a911468`; the owner ruled on every open point the same day (§0.1). P1 may start.

Working copy: **`/home/greg/docker/Projects/gorb-os-2`, branch `dev2`** (owner's
call, 2026-10-11). The sibling `../gorb-os` clone stays on `main` and is two
tooling commits ahead (`c32244a`, `4dc371a`), both confined to
`tools/watch-composer/` and an appended `.clinerules` section, so every firmware
line citation in this file resolves the same in both trees. `/tmp/serflash.sh` now
builds from this tree's `build/`.

It answers the three questions the owner asked:

1. Which part of the current clock implementation is the OS, and which is the clock app?
2. Can every enabled app's art sit in one shared store so the knob carousel stays lively?
3. What does running apps "not natively" cost in frames?

On question 2 the ruling is sharper than the original framing. Rotating between
**installed apps whose art is already resident** must not reboot: that is a pointer
swap. **Loading new art** — a theme, an install — does reboot, and the owner accepts
that as the cost of a commit (C1, D9).

Every number is read off a file or a build artifact in this tree, and the source is named with it. Two numbers cannot be given yet; they are marked MEASURE, and producing them is what P1 exists for.

---

## 0. Decision record

| # | Decision | Choice | Why |
|---|---|---|---|
| D1 | App execution model | Declarative packages, interpreted by compiled-in behaviour modules | The pixel path is priced in hardware (section 5): an interpreter there is fatal, an interpreter over decisions is free |
| D2 | Apps ship code | No. The card carries data; a new behaviour kind is a firmware build | `theme_schema.py:23` states the rule already: adding a kind the device does not know is one skipped row in a log line, not a corrupt dial |
| D3 | Escape hatch for app-authored logic | Deferred wasm3 (about 60-100 KB, apps written in C) against the same `orb_svc_t` table | ESP-IDF 5.4 ships no user-space ELF loader; MicroPython (about 1.5-2 MB) does not fit a 2 MB slot with the image at 1,476,128 B |
| D4 | Native loadable code | Rejected for now | No loader anywhere in `~/esp/esp-idf/components`; it needs relocatable objects, a hand-rolled PIC loader, a pinned ABI, and the S3 gives no isolation for PSRAM data. Its one unique payoff is app-authored per-pixel painting, and D1's painter registry covers that cheaper |
| D5 | Switching an app | No reboot to rotate between resident apps. Reboot stays on the commit path: loading new art, i.e. install | section 2 |
| D6 | Where packages live durably | New `apps` data partition for catalog, manifests and geometry; art banks in three re-sliced `assets` slots; sector-granular, written off the visible path | An erase suspends the cache. `main.c:62-71` exists precisely so nothing erases under a live dial |
| D7 | Where art lives while running | **Mechanism A (map many)** is the default now the page maths works for two flagships; M2/M3 confirm on the board | Both flagships plus `.rodata` cost 125 of 256 DROM pages (`orb_theme.h:48-55`, `build/gorb-os.map`); PSRAM was never the only option, it was the unmapped one |
| D8 | Working copy | `gorb-os-2`, branch `dev2`, at `a911468` | Owner's call. `/tmp/serflash.sh` repointed at this tree so a flash here cannot silently re-send the sibling's image |
| D9 | App installs keep A/B? | **No.** One slot, write, read back and CRC, then write the header, then reboot | Owner ruling C1. A/B survives a torn write by keeping the *other* side bootable; the owner accepts that a torn app install costs that app and a re-copy of the card file instead, and section 2 keeps the header-last rule that makes a torn bank read as absent rather than corrupt |

### 0.1 Owner's rulings, 2026-10-11

These are decided. An implementer who disagrees edits this table, not the code.

| Ref | Decision | Ruling |
|---|---|---|
| B1 | Faces per app | **One face per app.** Options are exposed through OS hooks into the app's options menu |
| B2 | Who owns the theme picker | The **clock app** supplies it via the same options-menu hooks; the OS renders, the app decides |
| B3 | Where grants are approved | **On the dial.** The PWA approval screen comes later |
| B4 | Where uninstall lives | The **OS options menu**, not inside the app |
| B5 | How many apps | **Cap 8 installed**, hard, no repartition |
| C1 | Art residency, A/B, capacity | **No A/B for apps.** Reboot on load is expected and committed. Provision **two flagship apps**, revisit the rest once those two exist |
| C2 | Fault handling | NVS fault counter; boot into the clock; mark the app `failed` with a reason shown in APPS |
| C3 | Provenance | Trust the card. Reserve a `sig` word at a fixed header offset so signing later does not move `GEOM_ABI` |
| C4 | Compatibility gate | `abi_min`/`abi_max` instead of `min_os_build`; out-of-range refuses with a named reason |
| C5 | Carousel identity | **Text only.** No art tile |
| C6 | Behaviour-kind registry | Compiled into the OS, one table, append-only IDs, never renumbered |
| C7 | Suspended apps | `service()` yes, `tick()` no; refresh on reveal |
| C8 | Uninstall the clock | Not offered. The clock is the permanent homing screen the long-press returns to, replacing the hardcoded `INPUT_SCREEN_CLOCK` |
| C9 | Setting types | `int`, `enum`, `toggle` **and `string`**, frozen now |
| C10 | Card writes | No `sd_write`. Apps persist through NVS inside their quota |
| C11 | Tooling | `tools/orbapp.py` writes the container; upload lands `/sd/apps/<slug>.orbapp.part` then renames, exactly the theme path |
| C12 | Arena exhaustion | Evict least-recently-visible, log it |

---

## 1. The line, drawn at today's files

Today the OS reaches into the clock by name. `input_router.c` hardcodes `INPUT_SCREEN_CLOCK` and calls `clock_view_screen()`; `orb_net.c:2261` hardcodes `to=clock|menu|hue`; `main.c:93-95` calls three `*_init()` in a fixed order. That is the coupling this split removes.

### 1.1 Stays in the OS (never ships on the card)

| Area | Code to keep | Why it cannot be app data |
|---|---|---|
| Boot ladder | `main.c` whole, incl. the pre-`display_init()` install order | an install must not race a paint |
| Panel and flush | `display/display.c`, `co5300.c`, `display_mark_rows*`, `display_commit_row`, `display_fb_mirror`, the bench counters | hardware, TE timing, DMA |
| Compositor | from `clock_view.c`: `fill_sprite_dsc`, `fill_plate_dsc`, `make_layer`, `layer_spr`, `sprite_bytes`, `sprite_drawable`, `paint_ctx_init`, `paint_row_ctx`, `paint_row`, `blend_px`, `mix_16_16`, `trigo_sin`, `rot_sin_cos`, `swept_rect_box`, `swept_box`, `area_union`, `mark_rows`, `l4_active`, `request_commit`, `mark_all_layers`, `clock_layer_commit`, `map_blend`, the `s_row_snap` flush gate | the measured 0.22 ms/row and 185-232 ns/px live here (section 5) |
| Delta-bank playback | `s_canvas`, `gif_apply_frame`, `build_bg_dscs`, `show_plate_frame`, `bg_frame_now`, `bg_rate`, `gif_tick_ms`, `gif_mode` | keyframe-plus-rect playback is not clock-specific |
| Frame scheduler | `tick_timer_cb`, `tick_base_ms`, `hand_beat_ticks`, the deadline ladder, `report_fps_if_due`, and today's `clock_view_set_bounce` (rename: suspend/resume) | the LVGL task is the one animation clock and the only widget context |
| Art bank and mmap | `art/art_bank.c` whole | the MMU budget is device-global (section 3) |
| Package manager | `orb_theme.c`: header parse, `slot_verify_pixels`, `copy_into`, `adopt`, `orb_theme_boot/stage_load/unstage/scan/delete`, the upload trio; plus `orb_sd.c` mount/lock/`orb_sd_name_ok`/path builders | verify-then-commit, the card lock, the A/B discipline **for themes** (apps drop A/B per D9 and P6 reconciles the paths) |
| Input | `knob/knob.c`, `input/input_router.c` rewritten as a registry | the one knob, the two-level gate, `knob_row_step` |
| Power and motion | `power/idle_dim.c`, `imu/imu.c` | policy owner; apps request, never write (section 4) |
| Time service | `orb_net.c` SNTP, the tz table, `orb_net_time_synced()` | one clock authority; exposed as `orb_time_now()` (section 4) |
| Radios, HTTP, OTA | `orb_net.c` STA/SoftAP/httpd/OTA/rollback | no app may own the radio or the boot slot |
| Menu shell | `menu/menu_view.c` row pool, cursor clamp, `orb_menu` worker, service/reap path | the OS's own UI (section 7) |
| Fallback | the compiled-in `assets/clock_assets.{bin,h}` pair, `orb_theme_geo.h` macro indirection | a board with a dead card still tells the time |

### 1.2 Becomes the clock app (`apps/clock/`)

| Area | Code to move |
|---|---|
| Dial art | the whole pixel bank: `assets/clock_assets.bin`, every `clk_sprite_t` |
| Scene and geometry schema | `clk_theme_t`, `clk_bg_rect_t`, `clk_comp_t`, `clk_aux_t`; the `THEME_FIELDS`/`COMP_FIELDS`/`AUX_FIELDS` wire lists; `theme_schema.py` SOURCES/KINDS/FLAGS; `GEOM_ABI` |
| Time to hand math | `hand_angles` (hour/min/sec mapping, sweep phase, railway, `second_sweep`/`tick_rate` policy), `s_bt_ctr`, `s_bt_frac` |
| Tourbillon | `tourbillon_advance`, `tourb_chords`, `tourb_stamp_pose`, `tourbillon_ink_mark`, `tourb_mark`, frame-bank selection |
| Complications | `comp_value`, `comp_frame_index`, `comp_strip_positions`, `comp_strip_digits`, `comp_slot_show/hide`, `comps_update`, `comp_create_slots`, `comp_bucket`, `complications_create_bucket`, `comp_touch_box`, `comp_add_jobs`, and the calendar/astronomy constants (`MOON_SYNODIC_S`, `MOON_NEW_MOON_S`, `comp_iso_week`, `comp_year_leap`) |
| Sub-dial hands | `aux_create`, `aux_update`, `aux_value_1000`, `aux_angle_10`, `aux_insert_at`, `aux_add_jobs` |
| Layer ordering policy | the z-stack discipline of `clock_layer_commit` (1636-1700), submitted as a layer list rather than coded into the compositor |
| Its own settings | tick rate and sweep, the `--matte` bake policy, `CLOCK_L4_MARK_STATIC_BOXES` |
| Its own UI | the THEME menu page and the `/themes` PWA: that is the clock app's installer |
| Toolchain | `bake_clock.py`, `prep_watch_kit.py`, `kit_check.py`, `theme_schema.py`, `orbtheme.py`, `tools/watch-composer/` |

### 1.3 Also an app, and the cheaper proof the ABI works

`color/color_picker.c` is a screen with an armed state and knob input and no OS tether beyond the router. It is the second registered app and the first non-clock one. `orb_net.c:2261`'s `to=clock|menu|hue` becomes a slug lookup and `INPUT_SCREEN_COUNT` disappears.

### 1.4 New OS code that does not exist yet

| Module | Owns |
|---|---|
| `main/app/orb_app.c/h` | manifest reader, the `apps`-partition catalog, per-app state (installed / enabled / active / order / `failed`), install with reboot, uninstall from the OS page, the residency arena owner, the C2 fault counter |
| `main/app/orb_svc.c/h` | the `orb_svc_t` table, the grant check, the per-app call counter, the NVS quota check |
| `main/gfx/` | the compositor, delta bank and frame scheduler extracted from `clock_view.c` |
| `apps/clock/` | the clock app, built as one package plus a compiled-in fallback |

The ABI is a small struct of function pointers, all called on the LVGL task, which is what lets the wasm3 tier arrive later without touching apps:

    typedef struct orb_app orb_app_t;
    struct orb_app_iface {                 /* every call runs on the LVGL task */
      esp_err_t (*activate)(orb_app_t *, const orb_svc_t *);
      void      (*suspend)(orb_app_t *);   /* park the tick; service() still runs (C7) */
      void      (*resume)(orb_app_t *);    /* clean repaint + restart the tick (C7) */
      void      (*tick)(orb_app_t *, uint32_t now_ms);      /* visible apps only */
      bool      (*armed)(orb_app_t *);
      void      (*set_armed)(orb_app_t *, bool);
      void      (*input)(orb_app_t *, int32_t delta, bool pressed);
      void      (*service)(orb_app_t *);   /* reap its own worker jobs, even parked */
      /* Options, one face per app (B1). The OS renders the page; the app answers
       * the rows. This is also how the clock app supplies its theme picker (B2):
       * the OS owns the list, the app owns what the list means. */
      int       (*opt_count)(orb_app_t *);
      const char *(*opt_label)(orb_app_t *, int idx);
      const char *(*opt_value)(orb_app_t *, int idx);   /* rendered after the label */
      void      (*opt_inc)(orb_app_t *, int idx, int32_t steps);
      void      (*opt_sel)(orb_app_t *, int idx);       /* the press, if it acts */
      void      (*opt_arm)(orb_app_t *, bool armed);    /* gate the knob like a screen */
      const orb_menu_page_t *(*menu)(orb_app_t *);      /* NULL = no page, common */
    };

`suspend` keeps `service()` alive and stops `tick()` (C7), so a weather app parked
behind the dial can still finish an HTTP refresh it posted, but cannot repaint or be
counted against the frame budget. Nothing may repaint from `service()` — the OS drops
those marks and logs it, because a hidden app committing rows is precisely the
cadence collapse the fps findings measured.

Fault handling is the OS's, not the app's (C2): an abort inside any hook above is
caught at the panic handler, the fault counter for that app id goes up in NVS, and
the next boot comes up on the **clock** with the app marked `failed` and its reason
shown in the APPS page. Two consecutive faults mark it failed permanently until the
owner reinstalls. Because the S3 gives no MPU protection for PSRAM data, this is a
reboot boundary, not a recoverable error, and the ABI deliberately has no
`on_crash` hook an app could be trusted to run.

---

## 2. Reboot on commit, no reboot on rotate

Two operations were conflated until ruling C1. Loading **new** art is an erase-and-rewrite of the region the dial reads pixels from, and it ends in a reboot — `main.c:62-71`'s install-before-`display_init()` ordering is exactly that, and the owner accepts the dark screen it costs. Rotating between **already-resident** apps is a widget and descriptor rebuild, and must not reboot.

`art_bank.h:20-22` deliberately never unmaps, because the LVGL image descriptors built in `clock_view.c` cache raw pointers into the mapping. Those descriptors are already rebuilt on every activation (`build_bg_dscs`, `fill_sprite_dsc`, `comp_create_slots`, `aux_create`), so an app swap is a pointer and widget rebuild, not a flash operation. With both flagship banks resident at once (section 3.3) the swap needs no flash I/O at all.

| Operation | Touches flash? | Reboots? | Allowed on | Target |
|---|---|---|---|---|
| activate / switch (resident) | no | **no** | LVGL task, in the router's poll | one full-page commit, about 150 ms |
| activate / switch (evicted) | read only, re-mmap | no | worker, then swap | under 400 ms, never blocks the knob |
| install from card | yes, erase and write | **yes** | boot time, before `display_init()`, like a staged theme | refuse over budget, name the reason |
| uninstall, from the OS options menu | yes, erase | no, once nothing resident aliases it | worker task | confirm first, like `orb_theme_delete()` |
| firmware OTA | yes | yes | unchanged: boot slot + rollback | unchanged |

Why activation still cannot be allowed to write, priced: a 3.9 MB install is about 960 sectors, and an erase suspends the cache while the SPI bus is busy, which stalls every flash-resident read including LVGL's rendering code. Un-probed builds of the shipped dial measure 133.6-138.2 ms mean flush, and the accepted carriage recipe 123.3 ms at 7.41 median fps (`docs/fps-budget-findings-2026-10-10.md:23-24,107,114`); a 4 KB sector erase is in that same order. A live install turns a 7 fps dial into a stuttering one for seconds, which is the failure the pre-`display_init()` install order was written to prevent.

**What dropping A/B for apps gives up (D9), and what bounds the damage.** The theme path's two slots exist so an interrupted install leaves the *other* side intact and the dial still boots something recognisable (`orb_theme.h:24-28`; bench-proven at `.clinerules:444-458`, including the pending-theme ladder that gives up after 3 attempts and falls back to the baked-in bank). One slot has no such spare, so an install torn by a power cut or a pulled card leaves a bank with nothing to fall back to *inside that slot*. Three rules recover most of it without a second slot, all already written in `orb_theme.c`:

1. write payload and geometry first, read them back and CRC them **before** the header is written;
2. the header carries `serial` and is the only thing that makes a bank live, so a torn install has no header and its slot reads as **absent**, never as corrupt art;
3. a boot that resolves to no valid bank falls back to the compiled-in `assets/clock_assets.{bin,h}` pair, and the app is marked `failed` with its reason (C2) rather than retried forever.

So the accepted failure is bounded: a bad install costs the owner that one app and a re-copy of the card file, not the dial, and it can never show half of one theme and half of another because a half-written bank has no header and so never becomes the live one. Themes keep their A/B path as shipped; this ruling covers the app container, and P6 reconciles the two install paths rather than running both indefinitely.

## 3. Storage: the new partition, the re-sliced art bank, and where art lives

### 3.1 Measured facts

| Item | Value | Source |
|---|---|---|
| MMU page size | 64 KB | `build/config/sdkconfig.h:854` (`CONFIG_MMU_PAGE_SIZE=0x10000`) |
| MMU pages per region / regions | 256 / 2 | `soc/esp32s3/include/soc/mmu.h:18` |
| DROM address window | `0x3C000000..0x3E000000` | `soc/esp32s3/include/soc/soc.h:168-169` |
| App `.flash.rodata` (the shared claim on that window) | 298 KB = 4a7c8 | `build/gorb-os.map` |
| App `.flash.text` | 1.00 MB = 10054c | `build/gorb-os.map` |
| App image in a 2 MB slot | 1,476,128 B | `build/gorb-os.bin` |
| One flagship theme | 3,858,874 B = 59 pages of 64 KB | `assets/clock_assets.bin`, `stat` |
| `assets` partition | 8 MB at 0x420000, two A/B slots at 0x0/0x400000, cap 4,128,764 B (0x3F0000) each | `partitions.csv`, `orb_theme.h:48-55,296` |
| `www` partition | 1.25 MB at 0xC20000 | `partitions.csv` |
| `storage` (NVS) | 24 KB at 0xD60000, shared with the WiFi credentials | `partitions.csv` |
| Unallocated flash tail | 2.60 MB (0x29a000, ends 0xD66000 of 16 MB) | `partitions.csv` comment, verified by arithmetic |

| Per-app art footprint, flagship | 3,924,410 B (3,858,874 art + a 64 KB header block) = **60 pages** | arithmetic on the two rows above |
| Two flagships mapped together | 120 pages + `.rodata`'s 5 = **125 of 256** | arithmetic; leaves 131 pages of headroom |
| Two flagships in `assets` | 7,848,820 B of 8,388,608, **539,788 B spare** | arithmetic; the partition is the binding limit, not the MMU |

The DROM window is shared with the app's own `.rodata`, which is why `art_bank_init()` maps only `need_bytes` (`art_bank.h:37-41`). With `.rodata` at 298 KB the window is not the binding limit: the 8 MB `assets` partition is, at two flagship banks. That is exactly the capacity ruling C1 asks for, which is why mechanism A stops being a coin flip.

### 3.2 Agreed layout

```
/sd/apps/<slug>.orbapp              the drop zone (card; hand-copied or uploaded)
assets (re-sliced) 0x420000 + 8 MB  THREE slots, 64 KB aligned, one bank each:
                                      art0  0x000000 .. 0x3D0000   3.81 MB flagship
                                      art1  0x3D0000 .. 0x7A0000   3.81 MB flagship
                                      art2  0x7A0000 .. 0x800000   0.375 MB small
                                    no A/B for apps (D9): one slot per installed
                                    app, verify-then-header, reboot to commit
apps, data, 0xD80000, 0x280000      2.5 MB: catalog + manifests + geometry,
                                    sector-granular, written off the visible path
PSRAM arena (cap from M2)            overflow only: banks past the third slot
NVS "orb.apps"                       installed / enabled / active / order /
                                    grants / settings / fault counters
```

The new `apps` partition fits exactly in the unused tail: `0xD80000 + 0x280000 = 0x1000000`. It moves no existing row, so the `nvs` retention invariant at the top of `partitions.csv` holds and the WiFi credentials survive the change.

**Capacity arithmetic against the cap of 8 (B5).** Two flagship-sized apps take slots `art0`/`art1`. The third slot holds one small app at 384 KB or several that fit together, so a useful mix is *two flagship + several small*, and anything beyond the partition's 539,788 B of slack spills to the PSRAM arena or is refused at install with its byte count in the reason. The remaining installs up to 8 are art-light apps — colour picker, timer, weather text, a pedometer — whose banks are kilobytes, not megabytes. `apps` at 2.5 MB is generous for catalog-plus-geometry and is the right home for those small banks; it is not proposed as a flagship store, because a flagship package does not fit in it.

Do not shrink the two app slots to buy room: 1,476,128 B used of 2 MB, and `partitions.csv` already warns that a slot smaller than the image turns every upgrade into `ESP_ERR_OTA_VALIDATE_FAILED`.

`storage` at 24 KB is the other real constraint, and C9's `string` settings make it tighter: an 8-app grant-and-config table with free-text values needs a per-app byte quota and a size check at install, before any app gets `config`.

### 3.3 Residency: A is the default, B is the overflow valve

| | A: map many (**default**) | B: copy to PSRAM (overflow) |
|---|---|---|
| Art lives | mmap'd flash, one slot per installed app | packed PSRAM arena, filled from the card at boot |
| Cost per switch | one full-page commit, no flash I/O | copy on the worker, then one full-page commit |
| Hard limit | 256 DROM pages, shared with `.rodata`: two flagships cost 125, so A is not what caps us | PSRAM free bytes after `s_canvas` (434 KB), `s_fb` (434 KB), the tile ring and the tourbillon chord buffers |
| Risk | none at two flagships; a third would be measured before it shipped | OPI bandwidth is already being paid for by the flush path and the canvas restore; sprite reads there can steal the very ms the fps model measures |
| Precedent in this tree | exactly what `art_bank.c` does today, one bank at a time (`art_bank.h:37-41`) | the tourbillon chord caches (`clock_view.c:864`) and the GIF canvas already do this |

A becomes the default because the page maths is no longer in doubt at two flagships: 125 of 256. What ships unchanged is the *single-bank* mapping of today; M2 now proves the two-bank case on the board rather than discovering it, and M3 checks A against B for the overflow path only. The ABI hides the choice behind `orb_art_reserve()` either way, so an app never learns which mechanism served its pixels — which is what keeps B viable if a third flagship is ever wanted.

## 4. Apps, capabilities, and gated configuration

### 4.1 The package

One file per app, mirroring the `.orbtheme` discipline in `tools/orbtheme.py`: all little-endian u32 words, every field widened so no padding rule differs between the bake and the reader, header CRC32 and payload CRC32, verified whole before a byte is committed.

    header    magic 'GRA', FORMAT, GEOM_ABI, hdr_size, serial, baked_ms,
              abi_min, abi_max,                       <- C4: refuse out of range,
              sig,                                    <- C3: reserved, 0 today,
                                                        signing later moves nothing
              slug[24], title[32], hdr_crc            <- whole words, disjoint ranges
    manifest  app_id, behaviour_kind,
              required art bytes, layer/comp/aux/bank counts,
              requested period_ms, capability bitmask,
              n_settings, setting_off,
              geom_off/geom_size/geom_crc, art_off/art_size/art_crc
    settings  per descriptor: id[16], type, min, max, default, def_str[32]
              type in {INT, ENUM, TOGGLE, STRING}     <- C9, frozen now
    geometry  the app's own records, opaque to the OS
    payload   the art bank

`min_os_build` is gone (C4): a package carries `abi_min`/`abi_max` and the OS refuses one outside its `GEOM_ABI` with the numbers in the reason, the way `theme_schema.py` refuses an unknown kind. There is no icon or tile field anywhere in the container (C5) — the carousel is text, so a title is the app's whole visual identity and the bake tool must reject a package that ships art no manifest claims.

The container must keep the three-way mirror rule from `theme_schema.py`: the Python writer, the C reader, and the sentinel test move together, or the test fails rather than shipping a dial with the hands' pivots in the overlay's slots.

Two rules are load-bearing, both copied from the theme path:

- the header's `app_id` must equal the filename stem, checked through one helper reached by upload, scan, boot and stage (`filed_as_its_own_slug()`'s shape), or a mis-filed app reads as a broken installer;
- nothing is committed before the whole payload verifies, and a half-uploaded file stays invisible under a `.part` name that the scan does not glob;
- `tools/orbapp.py` is the only sanctioned writer of the container (C11), standing beside `tools/orbtheme.py`, and browser upload lands at `/sd/apps/<slug>.orbapp.part` then renames through the same `orb_sd` validators. The `/apps` PWA is **deferred** (B3): grants, uninstall and the catalog all have to work from the dial alone first.

### 4.2 Capabilities

The manifest declares what it needs; the owner grants it on the dial; nothing is implicit (B3). A grant prompt is a two-row confirm in the OS's own page, persisted per app id in NVS, shown the first time an app asks and re-askable from the app's OS options page.

| Capability | Grants | Gate |
|---|---|---|
| `time` | `orb_time_now()`, tz label | free, always on: no app is useful without the clock |
| `config` | its own NVS keys under `orb.apps`, within the manifest's declared settings | always on, bounded to its own namespace, its declared types and the per-app byte quota |
| `power` | a request to change the dim timeout or hold the dial awake | owner-granted on the dial, clamped by `idle_dim`, auto-revoked when the app is not visible |
| `net` | a bounded HTTP client through the OS, allowlisted hosts, rate-limited | owner-granted on the dial per app, default off |
| `sd_read` | read inside its own `/sd/apps/<id>/` only | owner-granted on the dial, path built by `orb_sd` validators |
| `ui` | the compositor primitives and an OS-rendered options page | free |
| `art` | its own bank, plus reserve from the arena | free for its own bank |

There is no `sd_write` (C10): an app that needs to persist uses `config` inside its quota, and a card it cannot write to is the same boundary the theme path already enforces from the other side.

`power` and `net` are the two that answer the owner's config-override requirement. The mechanism is a **request, not a write**: the app calls `orb_power_request(timeout_s, hold)`, and `idle_dim` stays the policy owner, applies the Kconfig and NVS ceiling, and lifts the hold when the app is suspended or the owner turns the dim back on. That keeps `idle_dim.h:64-70`'s ownership rule intact: the Kconfig values are the boot defaults, the owner's choice is the truth, and an app is a third input that can only ask.

Enforcement is a single check per entry in `orb_svc_t`, taken from a grant table keyed by app id, so a VM tier later cannot bypass it:

    #define SVC(a, cap)  do { if (!orb_grant_ok((a)->id, CAP_##cap)) \n                                  return ESP_ERR_NOT_ALLOWED; } while (0)

### 4.3 The OS service table apps see

    typedef struct {
      /* time */
      void      (*now)(orb_app_t *, time_t *sec, double *frac, bool *synced);
      const char *(*tz_label)(orb_app_t *);
      /* power: a request only, idle_dim owns the decision */
      esp_err_t (*power_request)(orb_app_t *, uint32_t timeout_s, bool hold);
      /* config: bounded to this app's declared settings, int32 and string (C9) */
      esp_err_t (*cfg_get)(orb_app_t *, const char *id, int32_t *out);
      esp_err_t (*cfg_set)(orb_app_t *, const char *id, int32_t v);
      esp_err_t (*cfg_get_str)(orb_app_t *, const char *id, char *buf, size_t cap);
      esp_err_t (*cfg_set_str)(orb_app_t *, const char *id, const char *v);
      /* net: OS-owned, allowlisted, rate-limited, off the LVGL task */
      esp_err_t (*http_get)(orb_app_t *, const char *url, orb_buf_t *out);
      /* art and compositor */
      const uint8_t *(*bank)(orb_app_t *);
      int  (*layer_add)(orb_app_t *, const orb_layer_desc_t *);
      void (*layer_set)(orb_app_t *, int h, const orb_layer_desc_t *);
      void (*layer_touch)(orb_app_t *, int h, const lv_area_t *);
      void (*layer_del)(orb_app_t *, int h);
      /* worker: never block the LVGL task, same rule as the menu's orb_menu */
      esp_err_t (*job_post)(orb_app_t *, orb_job_fn, void *arg, size_t arglen);
      /* ui: the shell's list primitives for the app's settings page */
      void (*status)(orb_app_t *, const char *fmt, ...);
    } orb_svc_t;

`orb_time_now()` is an extraction, not new logic: `compose_now()` (`clock_view.c:2447-2505`) already resolves the two time sources, the `CLOCK_EPOCH` baseline, the `orb_net_time_synced()` trust rule and the drift re-anchor. That belongs to the OS, every app needs it, and the clock should read it like any other app.

The OS must also hand out the tick instead of letting each app own one. `tick_timer_cb`, `tick_base_ms`, `bg_rate` and the deadline ladder become a scheduler an app registers with (`period_ms`, `cb`, `budget_us`); the OS arbitrates, keeps the fastest shared beat, and refuses an app whose callback overruns its budget, logging the overrun. Without that single owner, two apps asking for 25 fps each reproduces the exact cadence collapse `docs/fps-budget-findings-2026-10-10.md` measured.

---

## 5. What "not running natively" costs

The rule falls out of the measured model:

    flush_ms = 55 fixed + 218 us/row x full-width rows + (185..232 ns/px) x blended px
    (docs/fps-budget-findings-2026-10-10.md, single-variable contrasts; the shipped
     kit3 dial sits at 133..138 ms mean flush, or 123.3 ms on the carriage recipe)

**Interpreted decisions are free. Interpreted pixels are fatal.**

| Path | Frequency | Cost of interpretation | Verdict |
|---|---|---|---|
| Compositor pixel blending (`blend_px`, `paint_row_ctx`, `gif_apply_frame`) | per pixel, ~200k/tick | 20-100x the 185 ns/px native price, so a 450-row carriage tick goes from 123 ms to minutes | must stay compiled C, forever |
| Layer job list assembly (`clock_layer_commit`, `comp_add_jobs`, `aux_add_jobs`) | once per commit, `CLK_JOB_MAX` entries | fetch a record and branch: tens of cycles against a 123 ms commit | free. This is where the declarative interpreter belongs |
| Value derivation (`comp_value`, `aux_value_1000`, `hand_angles`) | once per moving element per tick, integer maths | a table lookup plus a switch instead of a switch: single-digit us on ~30 elements | free |
| App logic beyond the vocabulary (fetch, decide, animate a curve) | once per tick | Lua-class VM: a few hundred ns to a few us per op, so 1-10 ms for real work | acceptable **off** the LVGL task, on its own task with a budget and a deadline; fatal if it can delay a commit |
| App logic per frame on the LVGL task | every tick at 15 fps (67 ms) | 1-10 ms is 1.5-15 % of the beat | measure per app; the OS must refuse an app that cannot make its budget |
| App-authored pixel code | — | no sandbox, no bounds, the same 123 ms pipeline, one wild pointer away from a Guru meditation on a device in a sealed case | only via wasm3 (memory-bounded) or a registered native painter in the OS; never raw uploaded machine code |

Runtime footprints against the 1,476,128 B image in a 2 MB slot:

| Runtime | Code size | Fits? |
|---|---|---|
| Lua 5.4 | about 200-260 KB | yes, leaves about 300 KB of headroom |
| QuickJS | about 400-900 KB | yes, but eats most of the headroom |
| wasm3 | about 60-100 KB | yes, and apps stay C, so the existing toolchain survives |
| MicroPython | about 1.5-2 MB + heap | no, not without repartitioning the slots |

Recommendation: build the declarative interpreter and the broker now (no VM cost, no ABI risk, and it covers the clock plus every watch-style app expressible in the vocabulary). Add wasm3 in P7 only when a real app demands logic the vocabulary cannot carry. Do not add native code loading: ESP-IDF 5.4 has no user-space loader, so it means hand-rolling PIC plus a manifest verifier plus a crash-recovery story, and the owner's low-risk tolerance does not change what a bad app costs the dial's frame budget.

## 6. Measurement protocol (the answers this plan refuses to guess)

All three run on the board with the shipped kit3 art, before any app code exists, because a change that regresses the shipped dial invalidates everything above it.

### M1 — switch latency and carousel liveness

Instrument `orb_app_activate()` with `esp_timer_get_time()` at entry and after the first commit, plus `display_bench_snapshot()` deltas and `display_bench_phases_us()`.

    app: switch "steam" -> "orbital": build 4 ms, first commit 142 ms
         (mirror 41, hook 88, te 9, dma 4 ms) rows 452 blends 214k

Pass: full activate under 250 ms; a second switch under 60 ms (objects survive, only the model re-pushes); no new watchdog; the boot dial still reads 6-7 fps.

### M2 — residency budget: prove two banks map at once

Mechanism A is the default on arithmetic (section 3.1: 125 of 256 pages), and arithmetic is not a boot. Map, read-back-CRC and keep resident **two** flagship banks, then a third, and record the first count that fails plus the free PSRAM at that instant (`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)`).

    art_bank: 2 banks resident = 125 pages of 256, ok; crc verified through the mapping
    art_bank: 3rd bank refused: need 60 more, have 131-free-but-reserved, psram 6.1 MB
    psram: free at that instant, printed next to it, as the M2 arena cap input

Two results, both wanted. The pass line is that both flagships stay resident with their CRCs verified through the mapping and the dial still hits 6-7 fps, which is what makes a no-reboot carousel real. The second is the PSRAM number, which is the arena cap for mechanism B and for C12's eviction headroom. Both become compile-time caps the installer refuses against with a named reason, the way `ORB_THEME_SLOT_CAP` already works.

### M3 — the overflow path: A versus B for a bank with no slot

With A the default inside the flash slots, M3's question narrows to the bank that has no slot — the third flagship, or an app whose art spilled to the arena. Same art, two builds: A re-maps on switch (one full-page commit and a fresh mmap); B copies into the PSRAM arena on the worker at boot. Compare M1's activate cost and the steady-state fps of the two most demanding apps while rotating. B steals PSRAM bandwidth from the flush path and A costs an mmap call per switch; only the board says which bill is cheaper, and the answer is allowed to differ per bank size. `art_bank_bytes()` plus `display_bench_phases_us()` print the difference.

### M4 — the existing bench limits, from `.clinerules`

- The bench has no WiFi radio (`.clinerules:153`), so an install over the browser is proven only by `tools/host_sd_test.py`-style lifts of the shipped functions (`.clinerules:333-345`), and the card path itself only by the measured table at `.clinerules:444-458`. Any claim about a live install of an app must name which of the two it rests on.
- Long work runs detached (`setsid /tmp/foo.sh </dev/null >/dev/null 2>&1 & disown`, poll the log): `idf.py build` after a Kconfig change, serial flash, and any OTA.
- `/tmp/serflash.sh` pointed at `B=.../Projects/gorb-os/build`, the sibling clone, until this session repointed it at `.../gorb-os-2/build` (filed in `docs/theme-naming-todo.md:123`, still unchecked because the build has not been flashed from here yet). Verify that line before the first flash of an app build: a stale `B=` re-flashes the old image and every measurement becomes a lie.
- Read the IP from the serial log, never from memory (`.clinerules:461`).

## 7. Menu system work

The shell's discipline is right and stays: the armed/unarmed two-level gate, the clamping non-wrapping cursor, the one long-press escape, the worker task for anything that can block, and "show the damaged row with its reason".

| # | Change | Why |
|---|---|---|
| 1 | One registered `menu_page_t` table (title, hint, rows, label, select, back target) replaces the four coupled switches in `menu_view.c`: `page_rows`, `row_text`, `activate`, `go_back` | they must agree today and nothing enforces it; `go_back` already hardcodes each page by hand (289-303) |
| 2 | Top level becomes dynamic: AP, TZ, DISPLAY, APPS, ABOUT, then one entry per active app that declares options through the `opt_*` hooks | `top_item_t`/`IT_TOP_N` cannot grow without editing the shell; this is B1/B2's rendering side, and the clock app's theme picker becomes one of these entries rather than a hardcoded PG_THEME |
| 3 | `job_t` carries a function pointer plus payload instead of `job_kind_t` | an app posts its own blocking work while the shell still owns the task, the queue, `s_busy`, the mutex and the stack high-water log (`menu_view.c:245-249`) |
| 4 | APPS page: catalog of installed apps, states (`on the dial`, `enabled`, `on card`, `failed: <why>`), actions activate / enable / **uninstall** | B4 puts uninstall in this OS page, not in the app; B5's cap of 8 and the slot arithmetic are enforced here, refusal reason on the row; reuses `orb_theme_scan`'s shown-with-reason rule verbatim |
| 5 | Grant prompt: a two-row confirm when an app asks for a capability, persisted per app id, re-answerable from the app's options page | **on the dial** (B3), same shape as the staged-theme reboot ask (`PG_ASK`); the PWA approval screen is deferred, so nothing may depend on the browser for a first boot |
| 6 | Position readout on long lists (`3/62`), not only the current-row `*` | TZ is 62 rows and the picker cannot answer "how far am I from the end" |
| 7 | Fix the debt this touches: `h_sd()`'s 2,300 B JSON buffer whose overflow check runs **after** the loop (`docs/theme-naming-todo.md` 88-102), the 23/24/31 name-cap disagreement app ids will hit identically, and gate `/api/v1/screen` behind `CONFIG_ORB_DEBUG_HTTP` | an `off >= sizeof json` break inside the loop closes a real buffer overrun |

## 8. Phases

| Phase | Deliverable | Passes when |
|---|---|---|
| P0 | this file agreed and **agreed 2026-10-11 (§0.1)**; freeze the ABI on paper (`orb_app.h`, `orb_svc.h`, `.orbapp` manifest words, `theme_schema.py`'s vocabulary split OS/app) and name the first registered behaviour kinds (section 9.2) | the owner signs the vocabulary subset; no code before this line closes |
| P1 | extract `main/gfx/` compositor, delta bank and scheduler out of `clock_view.c`, zero behaviour change; add `orb_art_reserve()`; run M1 and M2 | boot dial byte-identical behaviour, 6-7 fps, `host_comp_math_test.py` and `host_menu_test.py` green, mutations still red — **compositor + commit pass + `orb_art_reserve()` landed and proven identical (§10); delta bank, scheduler, and the fps/M1/M2 measurements are still open** |
| P2 | `orb_app` registry and a dynamic text carousel (C5); clock as app #0, `color_picker` as app #1; `?to=` becomes a slug; the `opt_*` hooks render through the shell (B1/B2); run M3 | knob walks both apps; long-press still homes to the clock (C8); the carousel survives a disable/enable cycle |
| P3 | broker and grants: `time`, `config` (int/enum/toggle/string), `power` first; on-dial grant prompts (B3); `compose_now`'s anchoring becomes `orb_time_now()`; the dim-timeout request path; the C2 fault counter and boot-into-clock rule | `/api/v1/status` shows per-app grants, call counts and fault count; a denied app degrades visibly; a faulting app cannot wedge the boot |
| P4 | the `apps` partition, `/sd/apps` scan, verify-then-header install with reboot (D9), uninstall from the OS APPS page (B4), boot resolve; the three `assets` art slots; the 8-app cap and slot arithmetic (B5); `net` with an allowlisted client | the `.clinerules:444-458` install ladder re-run with real apps; refusal reasons reach the dial, not only serial; a power cut mid-install leaves the dial bootable and the app marked failed |
| P5 | table-driven menu pages, generic worker jobs, the position readout, the 3 debt fixes | `host_menu_test.py` rewritten for the table, all mutations red |
| P6 | split `apps/clock/` for real: bake emits `.orbapp`, the compiled-in pair stays as the fallback, and the theme install path reconciles with the app install path rather than running both | the shipped kit3 bank still rebuilds byte-identically (`.clinerules:172-200` proof) and a card-less board still boots the dial |
| P7 | optional wasm3 tier over the same `orb_svc_t` | only after an app provably needs logic the vocabulary cannot carry |

## 9. Resolved, and what is still genuinely open

Rulings B1–B5 and C1–C12 are in section 0.1 and are not open questions. What remains is only what the board can answer or what a second app will force:

1. **M2's two numbers.** Whether two flagship banks really stay resident at 6-7 fps, and the free-PSRAM figure that sets the arena cap. Everything in section 3.3 is arithmetic until M2 runs; the plan does not need the answer to start P1, but it forbids shipping three flagship banks without it.
2. **The behaviour vocabulary's first cut.** Which `CLK_KIND_*`/`CLK_SRC_*` become the OS's registered kinds, and which stay clock-app-private. `theme_schema.py`'s SOURCES/KINDS/FLAGS is the inventory; naming the subset is a P0 paper task, not a code one, and it is the last thing that must be agreed before the ABI is frozen.
3. **Whether the compiled-in fallback is one app or several.** Today `assets/clock_assets.h` guarantees a card-less board tells the time (section 1.1). If a second shipped app also wants a baked-in pair, the image grows against the 1,476,128 B figure in section 3.1, and that is a decision the first non-clock flagship will force rather than a guess worth making now.
4. **NVS quota per app** against the 24 KB `storage` partition with `string` settings enabled (C9) and 8 apps allowed (B5): the byte budget is a number to be chosen at P3 with the real key shapes in hand, then enforced at install.

---

## 10. P1 result — what landed 2026-10-11

**Landed.** `main/gfx/orb_gfx.h` (165 lines) and `main/gfx/orb_gfx.c` (568) are the OS
compositor, carved out of `clock_view.c`, which fell from 2,974 to 2,441 lines. gfx owns the
rotation and blend maths (`gfx_trigo_sin`, `gfx_rot_sin_cos`, the private `blend_px`/`mix_16_16`,
`gfx_paint_ctx_t`, `gfx_paint_ctx_init`, `gfx_paint_row_ctx`), the swept-box geometry
(`gfx_swept_box`, `gfx_swept_rect_box`, `gfx_area_union`, and `SWEPT_PAD` — which the header now
owns, because the tourbillon's orbit chords must pad by the same value the compositor's box
carries), the sprite-record helpers, the row mark set (`gfx_mark_rows` plus a `gfx_rows_t` read
view), the staging band, and the whole commit pass: `gfx_commit()` restores each marked row from
the canvas, paints the job list, byte-compares against the mirror, and DMAs only what came out
different. `clock_view.c` keeps the policy — which layers exist, at what angles, in what order,
plus the complication and sub-dial job lists — and ends `clock_layer_commit()` with one call into
gfx. That call is the whole app-side surface today.

`blend_px`/`mix_16_16` stayed file-static inside gfx on purpose: they are the measured
185-232 ns/px loop, and keeping `gfx_commit()` in the same TU preserves the inlining they had
when the hook lived in the clock. `gfx_blend_px` exists only so the DISPLAY_BENCH synthetic ink
can still reach the paint window, now through `gfx_set_band_hook()` instead of a call from the
hook that moved.

`art_bank` gained the residency accounting P1 promised: `orb_art_reserve()` (accounts, never
maps — mapping secondary banks is P2's job), `art_bank_pages_for()`, `art_bank_mmu_pages()`,
`orb_art_resident_count()` and `orb_art_reserved_bytes()`. A request that is not a multiple of
the 64 KB MMU page is refused, since a ragged span wastes a page at each end of the mapping.
`/api/v1/status` answers `art_pages` and `art_banks` beside the existing `art_ready`/`art_bytes`,
and the boot line now prints the page cost — measured on the bench:
`bank "assets" @0x830000, 1978973 B mapped of 8388608 B (theme blob 1978973 B), 31 MMU pages`.
Section 3.3's page arithmetic is now a number the board printed, not one the plan asserted. Free
PSRAM was already reported as `heap_psram`, so it was not duplicated under a second name.

**Not landed, still P1's remaining work.** The delta bank (`s_canvas`, `gif_apply_frame`,
`build_bg_dscs`, `show_plate_frame`, `bg_frame_now`, `bg_rate`, `gif_tick_ms`, `gif_mode`,
`bg_live`) and the frame scheduler (`tick_timer_cb`, `tick_base_ms`, `hand_beat_ticks`, the
deadline ladder, `report_fps_if_due`) are still in `clock_view.c`. They are the next commit.
`gfx_commit()` already takes the canvas as a parameter, so the bank moves without touching the
paint path again.

**Zero-behaviour-change proof, mechanical.** A checker (`/tmp/p1/parity2.py`) lifts each moved
function out of `HEAD:main/clock/clock_view.c` and out of `orb_gfx.c`, canonicalises the renames
and the row-state consolidation, and compares token streams. 12 of 14 are identical, including
the entire commit band loop. The two that differ are `gfx_fill_sprite_dsc` and
`gfx_paint_ctx_init`, and both differ only by the injected `const uint8_t *base` parameter: the
bank base is threaded in per call so gfx never learns about art_bank. That is the intended delta
and the only one.

**Gates.** `idf.py build` clean, zero warnings under `-Wall -Wextra -Werror=all`.
`host_comp_math_test.py` green at 5,933 vectors with all 14 mutants caught; `host_menu_test.py`
green with its 3 mutations caught. Both suites lift complication and sub-dial maths — app code
that did not move — so the extraction left the benches' lift anchors alone, which is what the
context note about `host_comp_math_test.py` breaking at P6 will have to revisit.

**On the bench.** Flashed app-only to ota_0 through `/tmp/serflash.sh` (EXIT=0), boot log captured
over an RTS reset with `/tmp/p1/cap.py`. Clean: no `E (`, no Guru, no abort. The dial runs the L4
path — `hook 435 (rest 23 paint 390 cmp 17) ms/s` and `blend/s by layer: hr 16054/31 min 17468/220
sec 4105/8 ... glass 198085/246` — so the moved code is doing the real compositing and the
per-role counters cross the TU boundary correctly.

**fps: the bar was not evaluable here, so it is not claimed.** The bench's `assets` partition holds
the `skeleton` theme: `bg gif delta bank: 8 frames @ 250 ms (4.0 fps), tick 250 ms`. The dial holds
exactly its baked tempo — 4 fps steady after a 6 fps plate-only warm-up, flush avg 74-84 ms against
a 250 ms tick, so about a third of the tick budget spent. The 6-7 fps figure belongs to a flagship
kit3 bank, which is not what is flashed. P1 therefore stays open on fps: it needs a flagship theme
on this board, which is the same setup M2 needs.

**M1: probe landed, measurement blocked.** `set_screen()` prints
`switch <slug> <ms> frames <n> flush <us> worst <us>`; the boot-time switch reads `switch clock
0 ms frames 0 flush 0 us`. A real clock-to-menu switch needs either the knob (three GPIOs and a
finger) or `GET /api/v1/screen?to=menu`, and this bench has no radio, so the route is unreachable
and no number was taken. Nothing is invented for it.

**M2: not run.** One bank resident at 31 pages, per the boot line above. The two-bank residency
question is P2's, once the loader can map a second bank.

