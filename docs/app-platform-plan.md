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
| D7 | Where art lives while running | **Mechanism A (map many)** is the default now the page maths works for two flagships; M2/M3 confirm on the board | Both flagships plus the image's own segments cost 142 of 256 DROM pages (`orb_theme.h:48-55`, `build/gorb-os.map`); PSRAM was never the only option, it was the unmapped one |
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
| C13 | Vocabulary freeze | **Ratified 2026-10-11.** The whole vocabulary is OS-frozen, none of it is app-extensible: all 4 `KINDS` (window, strip, ring, flare), all 9 `FLAGS` bits, and the 17 `SOURCES` are OS-owned names resolved through one internal table fed by the `time` and `config` capabilities. An app declares which frozen names it uses; it cannot add one. Only `text` needs a config grant (it reads an app's own NVS keys). `battery` and `power_reserve` stay reserved and unimplemented — no power source is wired, so the bake tool must refuse them. The 12 calendar sources plus `none`/`value` are OS-internal to the clock's own theme schema and need no per-app grant at all |

### 0.2 Owner's ruling, 2026-10-12 — the clock app's bundled default art

| Ref | Decision | Ruling |
|---|---|---|
| D10 | Whose art boots the clock | **The OS ships no art.** The clock app carries its own bundled default artwork, ingested from the SD card. The authoritative source is committed at `assets/sources/clock-app-default/`: the Design ID `WCD5-W1sic2…` (full string in that folder's `DESIGN_ID`), its compiled container `clock_app_default.orbtheme`, and the Watch Composer layer kit `skeleton_theme.zip` that the ID refers to. Until P4/P6 ship the `.orbapp` container and the `apps`-partition ingest, this art is **recorded, not wired**: it does not touch the boot path, and the compiled-in `assets/clock_assets.{bin,h}` pair stays the sole card-less boot fallback (section 1.1). |

This container is validated, not assumed. `tools/orbtheme.py read_container` passes both CRCs; `unpack_geom` yields the geometry the Design ID describes — hands `show=[1,1,0,1,0]`, 8 GIF frames at `tick_rate 4`, 128 delta rects, 0 tourbillon, and 2 rotating-second `aux` records (the c2 enamel subdial's seconds hand). Art is 2,650,159 B = 41 pages, inside the `SLOT_CAP` (0x400000) and the 234-page budget `tools/host_art_residency_test.py` pins. `skeleton_theme.zip` is byte-identical to the owner's source (sha256 `95aa5d3a4b646d9e80c63e75a118778d548cb7a46d0fab7e90b482a6c5614d38`); its `manifest.json` carries the same `WCD5-…` ID. The Design ID holds a *reference* to the art, never the bytes (composer help, `Watch Composer.dc.html`); the layer kit holds the bytes.

Three facts for whoever wires this at P6. The container's slug and title are `skeleton`, which collides with the theme already installed under that slug on orb-A's card — it cannot go on a card as-is under that name. The committed container has never rendered on hardware: routine `serflash.sh` reflashes push only the app, never the `assets` partition, and the slot headers validate before the fallback is reached. And no tool converts a finished container back into the compiled-in `clock_assets.{bin,h}` pair — `bake_clock.py` regenerates that pair only from an Arduino-style `clock_style.json`/`theme.json` folder, and the composer's `orbtheme-baker.js` emits only the container. Building that reverse path, or the `.orbapp` packaging, is P4/P6 work, not this ruling.

### 0.3 Owner's rulings, 2026-10-12 (second round) — closing P2's item 1

The `opt_*` shell rendering is accepted as designed — the shell really does render
what the clock answers (`menu_view.c` `PG_TOP`/`PG_APP`, §7 item 2 done, `PG_THEME`
gone); §12.6's "NULL everywhere" row was stale and is corrected there. Three rulings
close the item; the work order is §13.

| Ref | Decision | Ruling |
|---|---|---|
| D11 | The theme picker's commit path | **A theme pick installs and reboots, unconditionally.** The stage-then-ask flow is rejected: no pending NVS key, no "reboot now" / "keep this dial" rows, no boot-time card install. Press on a theme copies the container into the alternate slot on the worker task, verifies it from flash, marks that slot live, and asks the OS for the reboot — the pick IS the confirmation. Failure stays honest: the row answers with its reason and the device does not reboot. |
| D12 | App option pages may nest | **Sub-menus, one level deep** (owner chose this ABI shape from the four offered). An app's option list can carry header rows that open one nested list; items live inside it, never inside each other. One ABI hook added (§1.4): `opt_kind` — `OPT_ROW` / `OPT_GROUP` / `OPT_ITEM`, contiguous-section convention, NULL hook = today's flat page. |
| D13 | The default theme's slot | **The factory default is never an install target, and stays pickable.** Today the flashed bank at `assets` partition offset 0 sits inside slot A's region and `target_slot()` installs into A whenever slot B holds an install: the second pick erases the default AND the `use_baked_in()` card-less fallback, which reads pixels from that same region. The model the owner ratified: **A = the default, B = the card install** — a swap flips which is live, it never copies over A. |

### 0.4 Owner's ruling, 2026-10-12 (third round) — the M3 overflow-mechanism choice

The owner ratified the §14.1 ruling as proposed. Numbers, method and the
refusal-hunt finding stand there unchanged.

| Ref | Decision | Ruling |
|---|---|---|
| D14 | Which mechanism serves the overflow bank | **A (flash re-map) is the mechanism — for the overflow bank as for the primary.** The measured 3.6 ms of switch and 5% of read cost an arena saves do not pay its 471 ms boot fill and 3.86 MB of PSRAM (of 7.95 free), and steady rotation is identical at the reporting resolution. **B (PSRAM arena) ships dark** (`ORB_ART_MECH_B 0`) as P4's escape hatch — the only way a third flagship can boot when the page budget says no — with its gate measured and its ABI promise host-pinned. Flipping B on is one byte (`art_bank.h:98`); if it ever ships lit, its carried-bank rule applies (§14.1: the release-for-free variant re-fills at 471 ms, so an arena app carries its parked bank — the §6/M-carrier decision is mandatory for B, not optional). |

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
| Dial art | the whole pixel bank: `assets/clock_assets.bin`, every `clk_sprite_t`. The clock app's own bundled default (ruling D10) is the Design-ID-sourced container committed at `assets/sources/clock-app-default/` (§0.2) — recorded now, wired by P6 once `.orbapp`/`apps`-partition ingest exists |
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
       * the OS owns the list, the app owns what the list means. Single-level
       * nesting per D12 (§0.3): a GROUP row opens the run of ITEMs beneath it,
       * the shell hides those ITEMs until the group is open, and a NULL opt_kind
       * is the whole pre-D12 flat page. The shell validates the convention. */
      int       (*opt_count)(orb_app_t *);
      orb_opt_kind_t (*opt_kind)(orb_app_t *, int idx);  /* ROW | GROUP | ITEM (D12) */
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
| Two flagships mapped together | 120 pages + the image's own **22** (`.text` 17 + `.rodata`/`.appdesc` 5, per `idf.py size` 2026-10-11) = **142 of 256** | arithmetic; 114 pages of headroom. An earlier revision of this row counted only `.rodata`'s 5 and said 125 |
| Measured flagship, for calibration | `perpetual-calendar` = 2,503,747 B = **39 pages** on the board (§10); two of *these* plus `.rodata` = 83 of 256 | boot line `39 MMU pages`, `art_pages` in `/api/v1/status` |
| Two flagships in `assets` | 7,848,820 B of 8,388,608, **539,788 B spare** | arithmetic; the partition is the binding limit, not the MMU |
| Two *maximum*-size banks | 128+128+22 = **278 of 256 — overflows** | arithmetic; why `orb_art_reserve()` caps the running sum, not each request |

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
| Hard limit | 256 DROM pages, shared with the image's own 22: two flagships cost 142, so A is not what caps us | PSRAM free bytes after `s_canvas` (434 KB), `s_fb` (434 KB), the tile ring and the tourbillon chord buffers |
| Risk | none at two flagships; a third would be measured before it shipped | OPI bandwidth is already being paid for by the flush path and the canvas restore; sprite reads there can steal the very ms the fps model measures |
| Precedent in this tree | exactly what `art_bank.c` does today, one bank at a time (`art_bank.h:37-41`) | the tourbillon chord caches (`clock_view.c:864`) and the GIF canvas already do this |

A becomes the default because the page maths is no longer in doubt at two flagships: 142 of 256, using the image's measured 22 pages rather than `.rodata` alone. What ships unchanged is the *single-bank* mapping of today; M2 now proves the two-bank case on the board rather than discovering it, and M3 checks A against B for the overflow path only. The ABI hides the choice behind `orb_art_reserve()` either way, so an app never learns which mechanism served its pixels — which is what keeps B viable if a third flagship is ever wanted. **M3 measured that check 2026-10-12 (§14) and the owner ratified A for the overflow bank as D14 (§0.4); B ships dark as P4's escape hatch.**

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

Pass: full activate under 250 ms; a second switch under 60 ms (objects survive, only the model re-pushes); no new watchdog; the boot dial still reads its baked tempo (8 fps on the flagship bank measured in §10, whose art asks for 16 frames @ 125 ms).

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
| P0 | this file agreed and **agreed 2026-10-11 (§0.1)**; freeze the ABI on paper (`orb_app.h`, `orb_svc.h`, `.orbapp` manifest words, `theme_schema.py`'s vocabulary split OS/app) and name the first registered behaviour kinds (section 9.2 — **drafted 2026-10-12, `docs/behaviour-vocabulary.md`, awaiting the owner's Q1-Q3 answers to sign as D15**) | the owner signs the vocabulary subset; no code before this line closes |
| P1 | extract `main/gfx/` compositor, delta bank and scheduler out of `clock_view.c`, zero behaviour change; add `orb_art_reserve()`; run M1 and M2 | boot dial byte-identical behaviour, 6-7 fps, `host_comp_math_test.py` and `host_menu_test.py` green, mutations still red — **DONE. Compositor + commit pass + delta bank + frame scheduler all landed and proven identical (`836a0c2`); `orb_art_reserve()` landed; fps bar met at 8 fps on the flagship bank and 15 fps on the aviator stress bank; M1 measured, M2's two numbers taken (§10). `clock_view.c` is down to 1,957 lines of dial policy.** |
| P2 | `orb_app` registry and a dynamic text carousel (C5); clock as app #0, `color_picker` as app #1; `?to=` becomes a slug; the `opt_*` hooks render through the shell (B1/B2); run M3 — **art loader landed 2026-10-11: `art_bank.c` maps and unmaps resident secondary banks behind the page budget, M2 measured (§11); registry, `color_picker` as app #1 and the slug route landed 2026-10-11 (§12); carousel landed 2026-10-11, hardware-verified 2026-10-12 (§12.8), and its settle window replaced by the hand-measured two-window rule 2026-10-12 (§12.9); M1 re-run on hardware (§12.7) — the `opt_*` shell rendering landed and was accepted 2026-10-12 under rulings D11-D13 (§0.3), executed as §13; M3 measured on hardware 2026-10-12 (§14) — **P2 complete** | knob walks both apps **(§12.8: 63-83 ms per activation)**; long-press still homes to the clock (C8) **(§12.8: lands on `clock`, 315 ms)**; ~~the carousel survives a disable/enable cycle~~ — moved to P4 with the `enabled`/`failed` state it needs (§12.6) |
| P3 | broker and grants: `time`, `config` (int/enum/toggle/string), `power` first; on-dial grant prompts (B3); `compose_now`'s anchoring becomes `orb_time_now()`; the dim-timeout request path; the C2 fault counter and boot-into-clock rule | `/api/v1/status` shows per-app grants, call counts and fault count; a denied app degrades visibly; a faulting app cannot wedge the boot |
| P4 | the `apps` partition, `/sd/apps` scan, verify-then-header install with reboot (D9), uninstall from the OS APPS page (B4), boot resolve; the three `assets` art slots; the 8-app cap and slot arithmetic (B5); `net` with an allowlisted client | the `.clinerules:444-458` install ladder re-run with real apps; refusal reasons reach the dial, not only serial; a power cut mid-install leaves the dial bootable and the app marked failed |
| P5 | table-driven menu pages, generic worker jobs, the position readout, the 3 debt fixes | `host_menu_test.py` rewritten for the table, all mutations red |
| P6 | split `apps/clock/` for real: bake emits `.orbapp`, the compiled-in pair stays as the fallback, and the theme install path reconciles with the app install path rather than running both | the shipped kit3 bank still rebuilds byte-identically (`.clinerules:172-200` proof) and a card-less board still boots the dial |
| P7 | optional wasm3 tier over the same `orb_svc_t` | only after an app provably needs logic the vocabulary cannot carry |

## 9. Resolved, and what is still genuinely open

Rulings B1–B5 and C1–C12 are in section 0.1 and are not open questions. What remains is only what the board can answer or what a second app will force:

1. **M2's two numbers. — answered 2026-10-11, §11.** Five banks (the live dial's,
   both installed payloads, two 59-page stress banks — 206 art pages) stayed resident
   with CRCs verified through the mapping while the dial held its 15 fps baseline, and
   free PSRAM at the refusal was 7.9 MB. The plan's pass line held; what stays open is
   the MMU's true refusal point (the probe never reached it — §11's third finding),
   which M3 measures with a second partition. **— answered 2026-10-12, §14.2: the hunt
   mapped all 212 distinct pages the table can offer (assets, www, both app slots) with
   ~99 pages already resident and refused zero times; below flash capacity the allocator
   does not refuse at all, so budget 234 stands as contract, not ceiling.**
2. **The behaviour vocabulary's first cut.** Which `CLK_KIND_*`/`CLK_SRC_*` become the OS's registered kinds, and which stay clock-app-private. `theme_schema.py`'s SOURCES/KINDS/FLAGS is the inventory; naming the subset is a P0 paper task, not a code one, and it is the last thing that must be agreed before the ABI is frozen. **— draft written 2026-10-12: `docs/behaviour-vocabulary.md` carries the signable table (17 sources over four tiers, the composition enums, seven findings G1-G7) and needs three owner answers (Q1-Q3 there) to be signed as D15.**
3. **Whether the compiled-in fallback is one app or several.** Today `assets/clock_assets.h` guarantees a card-less board tells the time (section 1.1). Ruling D10 (§0.2) settles the clock's own default art: the OS ships none, and the clock app's bundled default is the Design-ID-sourced container at `assets/sources/clock-app-default/`, recorded now and wired by P6. If a second shipped app also wants a baked-in pair, the image grows against the 1,476,128 B figure in section 3.1, and that is a decision the first non-clock flagship will force rather than a guess worth making now.
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

**Landed same day, second commit (`836a0c2`).** `main/gfx/orb_sched.h` (149 lines) and
`main/gfx/orb_sched.c` (589) are the delta bank and the frame scheduler, carved out of
`clock_view.c`, which fell from 2,441 to 1,957 lines. The bank owns the GIF-mode PSRAM canvas
(`s_canvas`, `s_canvas_dsc`), the classic plate-frame bank (`s_bg_dsc`, `build_bg_dscs`,
`bg_frame_now`, `show_plate_frame`), the frame cycling and the push (`gif_apply_frame`), and the
three pacing predicates (`gif_mode`, `gif_tick_ms`, `bg_live`); the scheduler owns the `lv_timer`
with its deadline ladder (`tick_timer_cb`), `tick_base_ms`, `bg_rate`, the commit nudge
(`request_commit`), `l4_active`, and the per-second throughput report (`report_fps_if_due`).
`gfx_commit()` already took the canvas as a parameter, so the bank moved without touching the
paint path again.

The seam is `orb_sched_policy_t`, installed at `orb_sched_start()`: the tick calls the app's
`compose_now()`+`tourbillon_advance()` pair through `.tick`, and the L4 full-box re-mark and the
bench plan through `.mark_all`/`.bench`. This TU includes no clock header, so the dependency
stays app → gfx and never inverts — the same shape as `gfx_set_band_hook()`. Three things stayed
in the app deliberately. `s_bt_ctr/sec/frac` stayed with `hand_angles()`, its only reader: the
plan's original P1 line listed `hand_beat_ticks` under the scheduler, but the code graph shows
the tick never touches it — it is sampled where the angles are made, so moving it would have
meant inventing an accessor for one caller. `mark_all_layers()` stayed because it walks the
app's own layer and shadow arrays. `l4_active()`'s eleven mark-site callers are policy-side; the
predicate itself moved and is exported.

**Zero-behaviour-change proof, mechanical.** `tools/gfx_parity_test.py` lifts each moved
function out of the pre-extraction `clock_view.c` (the paint path pinned at `786efcd`, the last
revision whose clock still held it whole; the bank and the tick pinned at `3116942`, two
revisions later, because `orb_gfx.c`'s own move had already rewritten `request_commit` and the
fps report's per-role loop — comparing those against `786efcd` would read someone else's change
as this one's) and out of `orb_gfx.c` / `orb_sched.c`, canonicalises the renames, the row-state
consolidation and the policy callbacks, and compares token streams. 13 paint routines and 14
bank/tick routines, all identical. It fails on anything but the accepted delta, and it was
checked to fail four ways: shifting one bit in `gfx_rot_sin_cos` (`>> 5` to `>> 4`) returns rc 1
naming the function, and so does `s_gif_due += d;` becoming `s_gif_due = d;`, the deadline
ladder's `- 8` becoming `- 9`, and the `gif_tick_ms()` floor's `d < 20` becoming `d < 21`. The
last three are pure scheduler, invisible to any dial-pixel test.
Of the 28 comparisons, 26 are identical, including the entire commit band loop. The two that
differ are `gfx_fill_sprite_dsc` and `gfx_paint_ctx_init`, and both differ only by the injected
`const uint8_t *base` parameter: the bank base is threaded in per call so gfx never learns about
art_bank. That is the intended delta and the only one.

**Verified on the bench, not just on the host.** The aviator 15 fps bank was the stress case,
since it is the only theme whose tick is over-subscribed. Pre- and post-extraction builds were
flashed back to back over the same serial link, same theme, same 62 s capture: fps median 15
both, p10 12 both, mean 14.41 vs 14.33; flush median 60 ms and p90 77 ms both; px/s median
1,212,532 vs 1,231,172; chunks/s median 167 vs 165. The boot dial prints the identical lines:
`bg gif delta bank: 15 frames @ 67 ms (14.9 fps), tick 67 ms` and `composite: baked static
matte`. All eight host suites are green (`gfx_parity`, `host_tz`, `host_sd`, `host_comp_math`
at 5933 vectors, `host_bake_comp`, `host_idle_dim`, `host_knob`, `host_menu`), and the app builds
clean under `-Werror`.

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

**fps: the bar is met, measured on a flagship bank 2026-10-11.** An earlier version of this
section declined the claim because the bench held only the `skeleton` theme
(`8 frames @ 250 ms (4.0 fps)`, which it held exactly). The owner has since loaded
`perpetual-calendar`, and the same build now reads:

    art_bank: bank "assets" @0x430000, 2503747 B mapped of 8388608 B (theme blob 2503747 B), 39 MMU pages
    orb_theme: theme "perpetual-calendar" from slot-A, 2503747 B of pixels
    clock: 3 hand(s), shadow on(0,0), sweep on(sweep, every tick), bg gif delta bank: 16 frames @ 125 ms (8.0 fps)

Steady state over 13 one-second samples past the warm-up: **8 fps** (min 8, peak 9 — it
reaches the tempo the art asks for, so the plan's 6-7 fps bar is exceeded, not just met),
947,844 px/s, flush avg 78 ms of the 125 ms tick — 62% spent, 38% of the tick unspent.
The breakdown says where the remaining budget goes, and it is not the bus: of the
compositor's 467 ms/s, `paint` is 415 (89%), `cmp` 19, `rest` 25, while DMA is 108 ms/s
(13.5 ms per flush) and TE 61 ms/s (7.6 ms). Row-major paid for itself, and the next
percent of frames is in the blend loop, not in the SPI path. Per layer: `glass` 440,334 px/s
at 146 ns/px and `dial` 424,250 at 114 ns/px are the two costs; the three hands together are
133,000.

**M1: measured — 2026-10-11, three clock-to-menu-to-clock round trips.** Taken over
`GET /api/v1/screen?to=…` on the connected bench (`sta ip 192.168.2.32`, rssi -56). An
earlier note here said the route was unreachable because "the bench has no radio"; that
misread `.clinerules:136-166`, which says the *host* has no radio and so cannot play the
phone joining the AP. The orb's own station joins the LAN and its routes answer curl, and
`.clinerules:161` lists exactly that. A switch between two already-resident views costs:

| theme / direction | synchronous | request to glass | the commit it provoked |
|---|---|---|---|
| skeleton, clock -> menu | 0 ms | 75, 75, 91 ms | 1, avg 68-80 ms |
| skeleton, menu -> clock | 0-1 ms | 256, 262, 266, 273 ms | 1, avg 239-255 ms |
| perpetual-calendar, boot -> clock | 1 ms | 464 ms | 1, avg 446 ms |

No reboot is involved in either direction, so D5's rule ("no reboot to rotate between
resident apps") is satisfied by the only switch the firmware has today. Two things the
numbers say, which the first version of the probe hid by sampling its counters inside the
switch (a 1 ms window at a 250 ms tempo holds no commit, so it printed `frames 0` and
printed display's all-time flush maximum as if the switch had caused it):

1. **The app's half is free.** 0-1 ms for the pointer swap plus the widget rebuild. An
   app host can spend the whole budget on locating a view and none on drawing it.
2. **The asymmetry is the compositor's, and it is a full-damage commit.** Entering the
   clock repaints every marked row (~250 ms), while the menu's first commit is ~70 ms.
   The steady-state commit at this theme's 250 ms tick costs 74-84 ms, so the switch
   costs ~3x a normal tick of the same compositor work: event-only row marking keeps
   steady frames small, and a switch loses that advantage because nothing has a valid
   delta bank yet. The flagship row scales that effect with the art — a cold first commit
   is 446 ms at 8 fps against 256 ms at 4 — so M3's two-app rotation must be judged against
   the ~273 ms figure at 4 fps and the ~450 ms one at 8, and P2's bank swap must not add to
   either. Warming a delta bank instead of discarding it is the lever.

**M2: the two numbers are measured; the residency test itself is P2's.** With the flagship
resident and the dial running its own 8 fps tempo, `/api/v1/status` reads
`heap_psram 7,471,404` free and `heap_internal 26,111` (largest block 9,216). So the
mechanism-B arena cap and C12's eviction headroom have a real budget to be cut against
instead of an estimate. The page arithmetic is now checkable against a measured bank rather
than a guessed one: at 64 KB per page, this 2,503,747 B bank is 39 pages, so two flagships
cost 78 of the **234 pages art actually gets** — 256 less the 22 the image's own `.text`
(17) and `.rodata`/`.appdesc` (5) occupy, per `idf.py size`. §3.1's original "125 of 256"
understated the fixed cost by 17 pages by counting `.rodata` alone; corrected there, and the
conclusion (A is not what caps us) holds with more room than claimed. Where the cap bites is
the slot, not the theme: two *maximum* 8 MB banks cost 128+128+22 = 278 of 256, so
`orb_art_reserve()` refuses on the running total — implemented and its decisions replayed.
Whether two mapped banks *actually* hold 8 fps under animation is still unmeasured: the
loader cannot map a second bank until P2, and no number is claimed for it here. That the
page figures above are size-report arithmetic rather than a reading of the live MMU table is
exactly what M2 has to confirm.

---

## 11. P2's art loader, and the M2 residency result — 2026-10-11

**Landed.** `art_bank.c` is no longer single-bank. The static mapping is joined by a
resident table — `art_bank_map_extra(offset, bytes, &slot)`, `art_bank_extra_ptr/bytes`,
`art_bank_extra_count`, `art_bank_unmap_extra` — capped at `ART_BANK_EXTRA_MAX` = 8
(one bank per installed app, ruling B5; the page budget, not the table, is what
runs out for flagship art). A re-request of a live offset returns or grows that entry
rather than billing a second region — same-app-asking-again, not a second copy. The
reservations are a per-bank table now, and a map of the same page-rounded size
consumes its booking, so reserve-then-map charges the window once;
`art_bank_mmu_pages()` sums each live map and each outstanding booking separately,
because each is its own virtual region. The primary's contract is untouched: still
`art_bank_init()`, still never unmapped, so every cached LVGL descriptor in
`clock_view.c` keeps working and the dial's boot path is byte-identical to P1's.
An app hands its bank to the compositor through the seam P1 already cut: whatever
pointer the app passes to `orb_bg_init()` is just `art_bank_extra_ptr(slot)` instead
of `blob()`, and gfx still never learns `art_bank.h` exists. `/api/v1/status` needed
no change — its `art_pages`/`art_banks`/`art_refusals` read the same accessors, and
they now describe a real multi-bank window.

**M2: measured on the bench, probe compiled out afterwards.**
`ORB_ART_M2_PROBE` (art_bank.h, the `DISPLAY_BENCH` one-line switch) maps the primary
plus the two installed payloads still on flash — slot A's perpetual-calendar at
0x10000 (2,503,747 B, 39 pages) and slot B's at 0x410000 (1,978,973 B, 31 pages) —
books both sizes through `orb_art_reserve()` first to prove the consumption rule,
CRCs every bank through the mapping against chunked `esp_partition_read` ground
truth, then pushes 59-page stress banks until the budget itself refuses. Boot log,
verbatim (`/tmp/p2/boot.txt`, fw `6e91c47-dirty`):

    I (716) art_bank: bank "assets" @0x420000, 1173563 B mapped ... 18 MMU pages
    I (716) art_bank: m2: start — primary 1173563 B (18 pages, resident); budget 234 of 256 pages, image claims 22
    I (717) art_bank: m2: booked A 2555904 B: ESP_OK
    I (717) art_bank: m2: booked B 2031616 B: ESP_OK
    I (1177) art_bank: m2: bank A — 3 banks resident = 110 pages of 256, ok; crc verified through the mapping
    I (1541) art_bank: m2: bank B — 3 banks resident = 110 pages of 256, ok; crc verified through the mapping
    I (2249) art_bank: m2: stress bank — 4 banks resident = 169 pages of 256, ok; crc verified through the mapping
    I (2958) art_bank: m2: stress bank — 5 banks resident = 228 pages of 256, ok; crc verified through the mapping
    E (2958) art_bank: m2: stress bank 3 refused (ESP_ERR_NO_MEM): need 59 more, art holds 206 of 234 budgeted; psram 7.9 MB

Both of the plan's numbers, and both wanted results. (1) Two flagship-sized banks
plus the live dial's own — five banks, 206 art pages — stayed mapped, CRC-verified
through the mapping, and the dial ran at its requested tempo under them: the same
boot's `clock N fps` lines read 15 for nine consecutive seconds and 12-16 after
(60 lines sampled), matching the P1 aviator A/B baseline of median 15, with no
watchdog and no reboot. The no-reboot carousel is real on this board. (2) Free
PSRAM at the refusing instant: 7.9 MB — mechanism B's arena cap and C12's eviction
headroom input, measured before the canvas, `s_fb` and the tile ring claim theirs,
which is the same basis §10's `heap_psram` figure uses and the same caveat applies.
`/api/v1/status` after boot read `art_pages: 206, art_banks: 5, art_refusals: 1` —
the budget's decision, the residency it granted, and the refusal are all visible
from the LAN without the serial cable.

**And one finding the plan did not ask for.** The probe's second loop maps every
distinct 64 KB page of the 8 MB partition, unaccounted, looking for the flash MMU's
own refusal point — and never found one: all 128 requests succeeded, 127 of them to
fresh pointers, one folded (page 0 returned the primary's own base, which is the
mmap layer deduping an identical physical request; it is also why the first version
of this loop, hammering one offset 256 times, "succeeded" without measuring
anything). Together with the 22 pages the image claims and the 206 of art already
resident, the window accepted 356 region-pages' worth of mappings without a single
ESP_ERR_NO_MEM from the allocator itself. Those numbers are region sums and the
spans overlap, so this bounds requests-accepted, not entries-consumed — but it is
already past §3.1's 256-page figure, which is therefore a `soc/mmu.h` region-count
reading, not the capacity this allocator refuses at. It was never the binding limit;
the 8 MB partition was, and still is. `ORB_ART_PAGE_BUDGET` 234 stays as the enforced
contract — it is what the installer's refusal reasons are cut against, and a
conservative budget that refuses early is a bug report, not a liability. The true
refusal point is unmeasured: the partition ran out before the MMU did. M3 should
find it with a second mmap-able region (the www partition is at hand) — that number
decides whether the budget can rise, and it belongs there, not in the loader.

**Verified.** `idf.py build` clean under `-Werror` with the probe at 0 and at 1;
all eight host suites green after the change (the reserve semantics — per-bank
entries, consumption on map — are not host-liftable, so the boot log's page
arithmetic stands as their check: 57 → 88 → 88, no double-billing, across
book+map); shipping-image boot re-captured after the flip-back — no `m2:` lines,
same primary map line, same dial ramp. The aviator swap in `assets/` remains
uncommitted and unrelated, as filed under P1.

**Reclaim latency: measured on the bench, probe compiled out afterwards (2026-10-12, `0623239-dirty`).**
The Step-5 owner question — release an app's `art_bank_map_extra` reservation on
suspend (release) or carry it while parked (carry) — turned on an unmeasured number:
what does the re-claim actually cost? `ORB_ART_LAT_PROBE` (art_bank.h, the same
one-line switch as `ORB_ART_M2_PROBE`) times `art_bank_map_extra()` (the reclaim a
resume runs) and `art_bank_unmap_extra()` (the release a suspend runs) over 31
map/unmap pairs per bank, at page-aligned offsets clear of the primary, fully
unmapped between samples so every timing is a cold re-claim. Boot log, verbatim:

    I (955)  art_bank: lat: start — primary 1855055 B (29 pages, resident); budget 234 of 256 pages; 31 iters/bank
    I (974)  art_bank: lat: kit3    59 pages  3768 KB | RECLAIM map_extra   min    305 median    316 max    403 us  (~5 us/page median)
    I (975)  art_bank: lat: kit3    59 pages  3768 KB | RELEASE unmap_extra min    250 median    265 max    326 us  (mean 266 us)
    I (975)  art_bank: lat: kit3   one switch (reclaim+release pair): ~581 us median
    I (994)  art_bank: lat: d10     41 pages  2588 KB | RECLAIM map_extra   min    302 median    318 max    409 us  (~7 us/page median)
    I (995)  art_bank: lat: d10     41 pages  2588 KB | RELEASE unmap_extra min    249 median    269 max    335 us  (mean 270 us)
    I (995)  art_bank: lat: d10    one switch (reclaim+release pair): ~587 us median
    I (1019) art_bank: lat: avkit   18 pages  1146 KB | RECLAIM map_extra   min    458 median    508 max    555 us  (~28 us/page median)
    I (1019) art_bank: lat: avkit   18 pages  1146 KB | RELEASE unmap_extra min    213 median    241 max    268 us  (mean 237 us)
    I (1020) art_bank: lat: avkit  one switch (reclaim+release pair): ~749 us median
    I (1020) art_bank: lat: done — 1 banks accounted (29 art pages; every lat bank unmapped, primary intact)

The answer to the owner question is unambiguous on the latency axis: **every op is
sub-millisecond** — worst observed single op 555 µs, worst reclaim+release *pair*
(one suspend, one resume) ~749 µs. The cost is dominated by the fixed
cache-invalidate/MMU-walk, not the page count: `esp_partition_mmap` installs the
bank's page-table entries and copies nothing (no CRC in this path), so a 59-page
reclaim (316 µs) is *faster* than an 18-page one (508 µs) — the small bank mapped
last, into a more-churned MMU free-list, and absolute µs tracks allocator state, not
size. Read against §12.7's M1 figures this is noise: an app already activates in
63-87 ms and the dial's own return is 411-476 ms, so the ~0.3 ms the release choice
adds on suspend and ~0.3 ms on resume are ~0.4% of an app-switch and ~0.07% of a
dial-return. **Reclaim latency is not a reason to carry.** The real cost of release
is therefore not µs-on-the-glass; it is the budget-pressure failure mode (§12.4: a
re-claim that returns NULL under pressure paints garbage) and the headroom carry
would consume (§12, the 234-page budget that M2 watched refuse at 206). The
`lat: done` line proves the probe is self-cleaning — every bank it mapped it
unmapped, MMU back to primary-only (29 pages) — and the same boot's `clock 12 fps`
lines after it confirm the dial runs at tempo untouched. `idf.py build` clean under
`-Werror` at the probe both 0 and 1; shipping-image boot re-captured after the
flip-back — no `lat:` lines, same primary map, same dial ramp.

---

## 12. P2's first half — the app registry lands (2026-10-11)

`main/app/orb_app.{h,c}`, ~500 lines of registry plus a 43-check host suite. §1's
three hardcoded couplings are gone: `main.c` builds whatever the table lists, the
router walks the table, and `?to=` resolves through it.

### 12.1 What actually changed

| Was | Now |
|---|---|
| `main.c` called `clock_view_init()`, `color_picker_init()`, `menu_view_init()` in a fixed order | `orb_app_init_all()` walks `s_apps[]` in order; table order IS boot order, so the clock's worker still comes first and the invariant moved into the table instead of being deleted |
| `input_screen_t {MENU, COLOR, CLOCK}` + five switch statements in the router | `orb_app_neighbour()`, `orb_app_current_armed()`, `orb_app_input()`, `orb_app_disarm_all()`, `orb_app_home()` |
| `orb_net.c` tested three literals against an enum | `orb_app_find_slug()`, exact slug before alias |
| long-press hardcoded `INPUT_SCREEN_CLOCK` | `orb_app_home()` reads the `is_home` flag (C8, as §1.4 anticipated) |

An entry whose `init()` fails is dropped from the carousel with its reason
logged; the boot continues. `ESP_ERROR_CHECK` on an app init would turn a broken
app into a bricked device on a board in a sealed case.

The table's three entries: **clock** (`is_home`, primary bank), **color_picker**
(app #1, `alias="hue"`, vector-only), **menu** (`ORB_APP_KIND_SHELL` — OS chrome
that walks in the carousel, not an app). Tagging the shell page rather than
faking it into apphood is what keeps B5's cap and the uninstall page honest:
only `ORB_APP_KIND_APP` rows are uninstallable.

### 12.2 The one deliberate behaviour change

The knob cycle was menu→picker→dial. It is now dial→picker→menu, because the P2
pass line asks for "knob walks both apps (clock → color_picker → clock)".
Everything else — the two-level gate, one step per poll, the direction coming
from `knob_col_step`, long--home — is unchanged, and `tools/host_menu_test.py`
still pins each of those.

`?to=clock|menu|hue` all still work: two are slugs, `hue` is the picker's
registered alias, so `/tmp/p2/cap.sh`, `tools/idle_watch.py` and the bench table
in `.clinerules` did not need editing to keep functioning.

### 12.3 The ABI moved from §1.4's paper list, by three hooks

`activate`/`suspend`/`resume`/`input`/`armed`/`set_armed`/`service`/`opt_*` are
§1.4's, with the same meanings. Three additions, all because these entries are
compiled in rather than shipped on the card:

- **`init`** — §1.4 has `activate` build the widgets, which is right for a VM app
  handed a container. A compiled app's widgets are built by its own `*_init()` on
  the boot ladder, where `main.c` can order the clock's worker task before the
  LVGL task exists. `activate` is therefore the *pre-load* half: claim art,
  restart the tick.
- **`screen`** — §1.4's "activation returns its owning obj", spelled for the tier
  that already has one.
- **`show`** — the post-load model re-push. It cannot fold into `activate`: LVGL
  invalidates on `lv_screen_load()`, and the order that has always run is
  park-outgoing → restart-incoming tick → load → re-push. The picker's chosen
  colour surviving a trip to the dial depends on that order specifically.

Nothing in the scheduler, the arming gate or the residency path reads `init` or
`screen`, so a VM-tier app that never supplies them is still ABI-complete.



### 12.4 The refusal paths, found by writing the test

The registry's art-claim failure paths were where the real bugs were, and the
host suite caught one on first run: a refused activation suspended the outgoing
app and then restored it via `resume`, which the clock did not implement — so
the dial stayed on the glass with its tick parked. A refused switch would have
left a frozen clock, caused by an app that never appeared.

Three refusals now share one `app_restore()`: art claim refused, `activate`
refused, no screen to load. Each re-claims the outgoing bank, restarts what its
`suspend` parked, and returns the code. The re-claim can itself fail (the budget
that refused the incoming bank is the same budget), which is logged as the
emergency it is: an app on the glass with no bank reads `NULL` from
`art_bank_extra_ptr()` and paints the silent garbage the loader's header has
been fencing off since it was written. `activate` returning non-NULL is not
enough; an unimplemented `resume` had to be caught by something other than the
glass.

**The release-vs-carry question (the residual on this step), now closed with a
measured number and a test.** The suspend path releases a banked app's secondary
(`app_art_release`, `orb_app.c`); the alternative was to carry it while parked,
sidestepping the re-claim that can fail above. Measured on the bench (§6,
`ORB_ART_LAT_PROBE`), re-claiming a flagship is ~320 µs and a full suspend+resume
pair ~580-750 µs — sub-millisecond, ~0.4% of an app switch's 63-87 ms, so latency
is not a reason to carry. **Owner ruling 2026-10-12: keep release-on-suspend.** The
parked app holds no MMU pages (so a banked app is never refused because a parked
one is squatting the window), and the cost is the NULL-reclaim branch below, which
is now pinned on the host by `tools/host_app_registry_test.py` — the app left ON the
glass ends with `art_slot == -1` after a switch whose rollback also fails, the
`art_bank_extra_ptr(-1)` garbage paint `art_bank.h` fences off, asserted rather than
only logged. It stays dead code for the shipping path until P4's first banked app
(§12.5), but the registry's handling of it is now covered.

### 12.5 Two findings this step settled

**`art_bank_extra_ptr()` has no shipping consumer yet, and will not get one from
P2.** `color_picker` is vector-only (LVGL draws the whole editor; 327 lines, no
bank), and ruling C5 makes the carousel text-only, so nothing P2 introduces owns
a secondary bank. The dial's own bank is the PRIMARY mapping, taken by
`art_bank_init()` before `display_init()` and never unmapped, because
`clock_view.c`'s descriptors cache raw pointers into it. `orb_app_t.art_bytes`
is therefore zero for all three entries today: the claim/release pairing is
implemented and host-tested, but its first real caller is P4's first banked app,
not the carousel. An earlier reading of this step ("wire the dial's bank
through `art_bank_map_extra`") would have moved a mapping the compositor cannot
survive losing, so it is recorded here as not happening.

**The residency arithmetic has no host suite.** `orb_art_reserve`,
`art_bank_map_extra` and the page-budget checks in `art_bank.c` are exercised on
the bench by the M2 probe's log and nowhere else; no `tools/` file referenced
them before this step. My registry suite drives the *registry's* pairing through
stubs, which is not the same claim. The loader's own arithmetic — page rounding,
the 234-page budget, reservation consumption, refusal counting — is untested on
a host, and `art_bank.c`'s behaviour after a bulk flash is not a rebuildable
fact. Flagged, not fixed here: it belongs with P3/P4, where a second banked app
makes it observable.

### 12.6 What P2 still owes

| Item | Note |
|---|---|
| Text carousel (C5) | **DONE on hardware 2026-10-12 (§12.8), ratified and re-specified by the owner's hand the same day (§12.9)**: a burst inside the settle window costs ONE activation and ONE full-page commit, detents spaced beyond the READ window cost one each, and a press commits a named peek instantly. The single 400 ms window of §12.8 is gone — measured read-pauses (510-1936 ms) made it commit under the reader. Its first real caller of `art_bank_unmap_extra()`/`map_extra()` in the shipping path is still P4 — see 12.5 |
| `opt_*` through the shell (B1/B2) | **Shell rendering DONE and accepted 2026-10-12 (§0.3 D11-D13).** This row's earlier claim — "NULL everywhere; tiles painted by the clock's own view code" — was stale when written: the clock implements all six hooks (`clock_themes.c:184-263`), the shell renders them (`menu_view.c` `PG_TOP`/`PG_APP`, §7 item 2 done, `PG_THEME` gone from the enum, menu list and knob walk share one table order), and `host_menu_test.py` exercises the path. What closed the item were the owner's three follow-up rulings — unconditional install-and-reboot on pick (D11), one-level sub-menus (D12), default-theme slot protection (D13) — which shipped as §13 |
| APPS page (B3/B4) | Needs the manifest and catalog (P4) to have anything to list |
| Disable/enable CRC round-trip | Testable once an app owns a bank, since it is the remap path that must CRC-verify |
| **M1 re-run** | **DONE on hardware 2026-10-11 (§12.7)**: apps activate in 63-87 ms, second switch flat, zero watchdog, and an md5-matched A/B shows the dial's tempo unchanged at 7 fps median either side. The dial's own return costs 411-476 ms, which is its full-page commit, not the switch |
| **Clock app bundled default (P2 clock theme-options E2E)** | **Owner ruling D10 (§0.2), 2026-10-12.** The OS ships no art; the clock app carries its own bundled default, ingested from the SD card. Its source of truth — Design ID `WCD5-W1sic2…`, the compiled `clock_app_default.orbtheme` (both CRCs verified through `tools/orbtheme.py read_container`), and the `skeleton_theme.zip` layer kit the ID refers to — is committed at `assets/sources/clock-app-default/`, byte-identical to the owner's originals. It is **recorded, not wired**: booting it as the clock's art needs the P4/P6 `.orbapp` container, the `apps`-partition ingest, and a container→`clock_assets.{bin,h}` reverse tool that do not exist yet. Its slug `skeleton` also collides with orb-A's installed card theme, so a card install under that name is refused by the duplicate-slug check. The compiled-in `assets/clock_assets.{bin,h}` pair (av-kit) stays the card-less boot fallback and has never itself rendered on hardware — slot headers validate first |

M1 is captured (§12.7) and the carousel is captured (§12.8), so the registry is
host-proven and hardware-proven, and C5's deferral is hardware-proven. The `opt_*`
item is **accepted** (D11-D13, §0.3; work order §13). What keeps
P2 open is M3. The pass line's third clause, "the
carousel survives a disable/enable cycle," is **not testable at P2 and is not a
blocker**: the registry exposes no enable/disable/set-failed entry point at all
(`orb_app.h`'s surface is init/activate/arm/service/opt), `enabled` is a plain
field written from the compiled table, and `orb_app.h:129` defers `enabled` and
`failed` to P4. The clause belongs to the P4 catalog, where the state exists.

### 12.7 M1 re-run against the registry — captured 2026-10-11, `8535154`

Kit3 (`3,858,874 B`, `8 frames @ 125 ms`), ota_0, serial flash, wall-powered,
`sta ip 192.168.2.32`, **rssi -73**. Screens driven over `GET /api/v1/screen`,
which is the same `set_screen()` the knob reaches.

| Switch | sync | on glass | commits |
|---|---|---|---|
| → color_picker (first ever) | 0 ms | +76 ms | 1, avg 65 ms |
| → menu | 0 ms | +87 ms | 1, avg 76 ms |
| → color_picker (2nd) | 0 ms | +70 ms | 1, avg 63 ms |
| → color_picker (3rd, via `hue`) | 0 ms | +63 ms | 1, avg 57 ms |
| → clock (return to the dial) | 1 ms | +411 / +476 ms | 1, avg 394-462 ms |

**Verdict against the four pass lines.**

1. **Full activate under 250 ms — passes for an app** (63-87 ms), and the clock's
   return is the one case that does not: 411-476 ms. That number is not switch
   cost. `clock_view_set_bounce(true)` forces a clean repaint, which is one
   full-page commit, and kit3's full commit is 403 ms in `.clinerules`' own
   measurement; §10 measured the same shape at 256-273 ms on the skeleton bank
   and 464 ms on perpetual-calendar. The switch adds roughly 8-70 ms on top of a
   commit the dial was always going to pay. The bar is met by apps and missed by
   the dial's own reveal, for a reason owned by the dial.
2. **Second switch under 60 ms — passes where it means anything.** Sync is 0 ms
   every time, and the to-glass cost is flat across first/second/third entry into
   the picker (76/70/63 ms), which is the proof that objects survive and only the
   model re-pushes. The glass figure cannot go below ~60 ms at all: it is one
   commit of a 466-wide band, and the average commit measured 57-65 ms.
3. **No new watchdog — passes.** Zero `guru`/`abort`/`watchdog`/`stack overflow`
   in both boot captures; clean `boot: Loaded app from partition at offset
   0x20000`.
4. **Boot dial at baked tempo — passes on the only comparison that is valid.**
   The A/B, same bank (md5-identical `aba381f3…`), same rig, same session:

   | image | stamp | one-second samples | fps median | flush avg median |
   |---|---|---|---|---|
   | pre-registry | `24fc3c2` | 44 | 7 | 133 ms |
   | registry | `8535154` | 45 | 7 | 132 ms |

   Zero delta, so the registry costs the dial nothing. The absolute 7 fps sits
   below kit3's documented 8.32 (`1999911`'s table), and the honest reading is
   rig state, not code: rssi is -73 tonight against -56/-61 in those runs, and
   this rig's established rssi sensitivity says the flush cost roughly doubles by
   -84. Named rather than explained away — the number to reproduce after the next
   antenna-side change is 8.32 with rssi above -60.

Also proven on hardware: `?to=color_picker` and the `hue` alias both switch, and
`?to=bogus` answers `{"error":"unknown app","known":[…]}` with a 400 without
echoing the caller's value. The registry costs **2,256 B** of app image
(1,478,240 against 1,475,984 for the same tree one commit earlier).

**Bench caveats worth repeating.** DHCP did not complete on the first retry boot
(no `sta ip` line at all), so the run needs a reachability poll rather than a
fixed sleep. The dial auto-dims at 60 s on this build, so the usable lit window
is t≈12-58 s; every figure above is from that window, and `frames` in each fps
line is a one-second count, not the cumulative total.

---

### 12.8 C5's carousel on hardware — captured 2026-10-12, `71947dd`

The ruling-C5 promise is that a flick costs one full-page commit instead of one
per detent. `host_carousel_test.py` proved it against a synthetic clock and
stubbed leaves; this proves it on the board, driving the shipping build through
`tools/bench/carousel_bench.py`. Detents are fired as separate
`GET /api/v1/screen?turn=+1` requests, which reach the router through
`input_router_inject()` — the same fold the encoder's own detent uses
(`input_router.c:202`) — so from the fold down the path is the shipping one: real
LVGL task, real registry, real display, and the commit count read from the
router's own M1 probe line (`input: switch X -> glass +N ms: N commit(s)`).

Run: orb-A (`44:bd:8d:60:80:88`, ARP-confirmed by the tool's own gate), `fw
71947dd`, slot ota_0, `art_banks 1`, `art_refusals 0`, rssi -56 to -69 across
three passes. The tool reads the two constants under test **from the device's
boot log** (`chip ready (settle 400 ms)`, `router running (poll 10 ms, 3 walkable
app(s))`), so if `ORB_CAROUSEL_SETTLE_MS` changes the control case moves with it
instead of silently testing a window the board does not use. Exit 0, all cases
held.

| Case | Detents | Device-side gap | Activations | Commits | Glass |
|---|---|---|---|---|---|
| coalesce-2 | 2 | 97-206 ms | **1** | **1** | 73-80 ms |
| coalesce-w+1 | 4 | 148-247 ms | **1** | 1 | 74-80 ms |
| wrap-w | 3 (= walkable) | 105-209 ms | **0** | — | — |
| control-2 | 2, spaced beyond window | 635-720 ms | **2** | 1 / 1 | 67-77 ms |
| control-3x600 | 3, spaced beyond window | 726 ms | **3** | 1 / 1 / 1 | 64 / 70 / 294 ms |
| hold-c8 | long-press from `color_picker` | — | lands on **`clock`** | 1 | 315 ms |

The differential is the whole result. Two detents inside the window: one
activation, one commit. The same two detents spaced past the window: two
activations, two commits. Without the control, an infinite window (never
switches) and a zero window (switches every detent, i.e. the pre-carousel
behaviour) would both pass the coalesce row. Five tight detents also cost one
activation, after one mid-burst wrap-cancel.

**Falsification checked, not assumed.** Forcing the control's spacing inside the
window (300 ms) must break the suite, and it did: `control-2 ... acts=1 want 2 |
FAIL`, exit 1. A second guard proved itself unprompted — one pass returned `VOID
(HTTP jitter pushed detents to 446 ms)` rather than claiming a coalesce it had not
tested. That matters because HTTP round-trip at bench rssi is the noise floor: a
requested 25 ms spacing arrives as 92-446 ms apart, so the x-axis is the device's
own `synthetic turn` timestamps and a drifted burst is voided, never passed.

**Bracketing the window.** Coalesced at gaps up to 326 ms observed; split at 510 ms
and above. Both straddle the coded 400 ms, so the constant behaves as written. The
bracket is ~180 ms wide because of the HTTP jitter above, which is why this
**cannot** ratify the number — see the limit below.

**Two findings the pass produced.**

1. **A full-circle flick does nothing, by design.** With three walkable entries
   (`clock → color_picker → menu`), a burst of exactly three names its way back
   onto the entry already on the glass, and `orb_carousel.c:164` cancels rather
   than commit a phantom switch — `wrap-w` prints 0 activations and one
   `cancelled on … — no switch`, and the hops counter resets. The host suite already
   pinned the no-phantom rule; what is new is the ergonomic consequence the table
   makes visible: on a three-app carousel, a three-detent flick returns the user
   exactly where they started. Correct, but worth knowing before a fourth app is
   not in the table.
2. **Switch cost reproduced, dial return still the outlier.** Picker and menu
   activate in 63-83 ms against §12.7's 63-87 ms, so the carousel added nothing to
   the switch. The clock's own return is the expensive one again: 294-315 ms here,
   411-476 ms in §12.7 — the dial's full-page commit, not the switch.

**What this run cannot say, stated by evidence.** Synthetic turns bypass the
encoder: `/api/v1/status` reads `raw 0, det 0, spd10 0, vhits 0, shits 0` after the
31 injected turns of the first pass, while the walk visibly happens. Injection
enters at `input_router_inject()`, downstream of `knob.c`'s valley/bounce logic, so
**the gap distribution of a real hand is not measurable this way**, and the 400 ms
window is therefore still un-ratified against fingers. It is derived from
`KNOB_SETTLE_MS` (30 ms, `knob.c:82`) and the picker's `VEL_IDLE_US` (220 ms), not
measured — `orb_carousel.c:40-47`'s own note, flagged "not owner-ratified" when the
module landed. Ratifying it needs a hand on the knob while the encoder telemetry is
read, which is a two-minute ask rather than an engineering task.

**That two-minute ask was made the same day, and it killed the single window.**
The hand measurement, its verdict, and the two-window replacement that stands in
the shipped code today are §12.9. Nothing in this section's bench record is
withdrawn — every case it lists still passes — but "still un-ratified" in the
paragraph above is now history: the number was ratified, found wrong at human
speed, and replaced.

Health: zero `Guru`/`abort()`/watchdog, zero `no display commit within` (the
parked beat was never leaked — the commit handover and the cancel-unpark both held
across 46 fired detents), and the dial resumed its 10-15 fps baseline after the
run. The device was re-flashed to `71947dd` first: it had come up on
`8eb4bf0-dirty` (branch `main` plus uncommitted edits), which the `serflash.sh`
audit caught as a stamp mismatch before anything was written.

### 12.9 The settle window, measured on a hand — 2026-10-12, ruling A+B

§12.8 closed with one debt: 400 ms was arithmetic, not a feeling. The owner turned
the knob on orb-A while `tools/orb_watch_ser.py` captured the carousel's own
`peek`/`settle`/`cancelled` lines — the timestamps are the device's, so the
numbers below carry no host-side clock — across two passes, with encoder
telemetry confirming a real hand each time (`det -16, spd10 42, shits 0`;
injection would show zeros, §12.8's proof). Logs: `/tmp/hand_flicks_20261012.log`,
`/tmp/hand_flicks_20261012b.log`.

**The measurement.** Pass 1 was free-style: 12 detents, every inter-detent gap
510-1266 ms, every one committed alone — coalescing never once engaged at human
speed, and the feature was a no-op for exactly the use it exists for. Pass 2
split the two regimes deliberately: 3 fast flicks of 3 detents each, and a slow
walk of single detents with a read-and-decide stop at each.

| Regime | Gaps observed (ms) | Against the old single 400 |
|---|---|---|
| Fast flick, detent-to-detent | 33-157 (n=9) | coalesces, 2.5x headroom |
| Slow read-stop, detent-to-detent | 510-1936, median ~1000 (n=24 both passes) | every one committed early, under the reader |
| peek -> commit latency | 400-408, all 19 commits | the timer itself is faithful |

The medians of the two regimes sit 15x apart, and the closest pair (157 vs 510)
still bracket 3x. **No single number can serve both**: any window above the
longest read-pause taxes every clean flick by ~2 s (past the ~1 s
feels-dropped line, and past the host suite's own `<= 800` ceiling); any window
below it dumps the reader onto an entry mid-decision — which is what the hand
experienced at 400. An earlier 800 ms compromise from this thread is withdrawn:
it coalesced the pass-1 gaps only by treating the reader's pauses as bursts, and
still committed early on the 1266-1936 ms tail.

**Owner ruling A+B.** (A) two windows, chosen per detent by the gap that
precedes it; (B) press-to-commit, so nobody waits on a timer they have already
beaten. B turned out to be already shipped — `input_router.c`'s "that one, now"
branch has committed a named peek on press since P2 — but no bench had ever
exercised it; it is now (press-1 below). The owner's same-day follow-up turned
the press from commit into **enter**: rotate to name, press once, and you are
inside the named app — the commit path calls `arm_current()` on the switched
entry, so the HSB editor or menu list opens with the same press (the state a
press on an already-showing screen produces; the dial stays inert because it
has no armed view). The auto-commit at window expiry deliberately does NOT
arm — an expiry that entered would make every settled switch a room the user
cannot turn the knob out of, so the bench asserts the absence of `armed` on
the coalesce/control cases and its presence, name-matched to the activation,
on press-1. Arming happens only when the switch landed: a refused
`set_screen` leaves the old view unarmed, because arming it would arm a
screen the user never named.

**The shipped rule** (`main/app/orb_carousel.c`):

| Constant | Value | Protects against |
|---|---|---|
| `ORB_CAROUSEL_SETTLE_MS` | 400 | in-burst commit delay; floors: encoder 30 ms recovery, picker 220 ms velocity-idle, 2.5x widest flick |
| `ORB_CAROUSEL_BURST_GAP_MS` | 300 | the classifier, cut between the two regimes (157 < 300 < 510), strictly-under comparison |
| `ORB_CAROUSEL_READ_MS` | 2000 | the hold for a detent that landed after a pause — or alone, a pause the shell never saw end; covers the 1936 ms observed max |

A lone first detent is deliberately a read, not a burst: the walk-away case pays
2 s for a switch nobody is waiting on, and the reader who has finished presses.
`cancel()`/`commit()` reset the classification, so the first detent of the next
gesture is a new gesture — pinned by a host case, because a predicate placed
after the pending assignment silently makes every detent look in-burst.

**Tests.** `host_carousel_test.py` gained the two-regime re-arm (fast window
counted from the in-burst detent itself), the read-window walk (1000 ms pause:
not due at 2999, due at 3000, walk continues from the named entry), the exact
`BURST_GAP` boundary direction, the post-cancel new-gesture rule, and bounds
tying all three constants to the measured regimes (`157 < GAP < 510`,
`READ >= 1936`, `READ <= 2500`). All 11 host suites green.

**Bench re-pin and the pass run.** The board now publishes all three windows in
one boot line (`chip ready (settle 400 ms, read 2000 ms, burst gap 300 ms)`),
and the bench takes its case spacing from the device, never from this file. The
control moved beyond the READ window (detents spaced between the two windows
coalesce legitimately now — a control that coalesces is not a control), the
tight-burst VOID re-pinned to the burst gap, the control gained its own VOID
for jitter that failed to clear READ, and `press-1` landed: one detent, a press
0.6 s later, the activation TIMED to prove the press pre-empted the auto-commit
(a count alone cannot tell them apart). 2026-10-12, `6188cd1-dirty`, orb-A,
rssi -58: coalesce-2 `seen 215, 1 act, hops 2, glass 87 ms`; coalesce-w+1
`seen 165, 1 act`; wrap-w `0 acts, 1 cancel`; control-2 `seen 2494, 2 acts,
71/83 ms`; hold-c8 lands clock; press-1 ok. All assertions held. With
rotate-press-enter the same seven pass on `e2926c2-dirty` (rssi -61): press-1
`1 act, armed color_picker`, control-2 `2 acts, no armed line` (the negative
arm pin), coalesce-w+1 `peeks+canc == 4 steps`. The tight-burst VOID earned
its keep in between: one run delivered only 3 of coalesce-w+1's 4 requests —
two HTTP arrivals clustered into one poll lose the extra step to the
one-step-per-poll rule — and was reported VOID, not passed and not failed.

**Precondition fixed on the way: the association wedge.** Three bench boots in
a row went network-dead for minutes at ~7.4 s, recovered only by pulsing EN.
The capture named the mechanism exactly: the self-inflicted reason-3 leave at
boot arms a 5 s retry; the association then reaches `run`; the uncancelled
timer fires `esp_wifi_connect()` into the live-but-unleased station, gets
`ESP_OK`, and destroys the association with no event left to re-arm the ladder
— both old guards (`s_sta_up`, `s_provisioning`) are false in that window by
definition. The fix (`main/net/orb_net.c`): a new `WIFI_EVENT_STA_CONNECTED`
handler cancels a pending retry at the association (not at the lease — the
unleased window is where the wedge lived) and arms a 15 s lease watchdog;
`NET_CMD_CONNECT` grew `s_associated` to its guard via the pure
`connect_attempt_is_moot()`; a stalled association is dropped the honest way
(`lease_watch_should_drop()` + `esp_wifi_disconnect()`) so the existing,
host-tested ladder retries from a real DISCONNECTED. Both predicates are
truth-tabled in `host_tz_test.py` alongside the ladder they complete. The pass
run above completed its own port-open boot with no wedge and no EN pulse.

**Caveats before this is called final.** n=1 person, one device, three walkable
entries — the 510 ms floor of the read regime especially needs a second hand
before sign-off (a fast reader could classify their own pauses as bursts and
get the 400; the press path caps the damage either way). The beat parks for up
to READ_MS after the last detent, so a deliberate slow walk keeps the clock
frozen ~2 s after the final stop; the idle-dim and every knob event bound it as
before. §12.8's ergonomic finding stands under the new windows: a three-detent
flick still wraps and cancels on a three-app table — correct, and now visibly
named three times before it does.

## 13. Work order — closing P2's item 1 under D11/D12/D13 (2026-10-12)

Owner-ratified scope; the rulings are §0.3, the ABI shape is §1.4. Three
deliverables, one or more commits, all before P2's `opt_*` item is called done.
Nothing here opens a question the owner has not already answered; the only
implementation latitude named below is flagged as such.

### 13.1 The defect being fixed (found by the owner's hand, confirmed in code)

`orb_theme.c` keeps two art slots, A at partition offset 0 and B at 0x400000,
each 4 MB (`SLOT_CAP`). `idf.py flash` writes the factory default bank at
offset 0 — inside slot A — and `use_baked_in()` draws the card-less fallback's
pixels from that same region. `target_slot()` protects the default only in the
never-installed state (both-invalid picks B): once slot B holds the first card
install, slot A reads invalid (a raw bank has no slot header), and the
`a->valid ? B : A` line aims the next install at A. The second pick therefore
erases the default and the fallback together; the "both live: replace the
older" branch that would honor the function's own comment never runs. The
pending NVS key makes it worse than one loss: a staged install survives into
whichever boot applies it, and the two ask-rows put the recovery decision on
the glass.

### 13.2 D13 — slots become what the owner said they were

**A is the default's permanent home; B is the only install target; the NVS
key records a choice, not a pending operation.** Concretely:

- `target_slot()` is deleted. `copy_into()` always receives `SLOT_B_HDR`.
  The erase-before-header discipline (the old header stays valid until the
  new one is ready) and the read-back verification stay exactly as they are —
  they are the reason a failed copy costs a row message and not a brick.
- Replace `THEME_NVS_PENDING` ("install this at next boot") with
  `THEME_NVS_LIVE` = `"default" | "card"`: which slot the dial draws. No
  install is ever deferred to boot — if the NVS says `card` but slot B fails
  verification at boot, boot falls back to the default with its damage row
  shown, because B's bytes were verified when written and anything else is
  corruption, not a pending state. The `THEME_MAX_FAILS` ladder and the
  boot-time card-open/copy path in `orb_theme_boot()` are deleted with it:
  the boot path resolves, it never installs from the card again.
- Picking the default is `live=default` + reboot: no card access, no copy.
  The picker must list a **Factory default** row (first row, title from the
  compiled pair, value `on the dial` when live) even when the card holds
  nothing — until D10's `.orbapp` wiring ships, its source is the flashed
  bank at offset 0, not the SD container.
- Pin in `host_tz_test.py` (next to the ladder's existing tables): the
  boot-resolve predicate, live-key x B-valid/B-corrupt x fallback, and
  `install_target() == SLOT_B_HDR` unconditionally.
- The debug HTTP surface must stop offering the old path: `h_theme_stage`
  (`POST /api/v1/sd/load?name=`) calls the same worker entry the picker
  posts, and `/api/v1/sd/unstage` disappears with the pending concept; the
  PWA's "staged … installs at the next reboot / Cancel" banner shows the
  live choice, not a pending one. Keep one code path; the endpoint and the
  knob must not be able to disagree about what a theme change is.

### 13.3 D11 — a pick is an install, an install ends in a reboot

`clock_themes_pick()` stops posting `J_STAGE`: it posts the install job (the
worker task — multi-second SD-to-flash copy, never the LVGL task). The job
runs the existing verify-first ladder (`card_check` → `verify_container` →
identity → `copy_into` → partition read-back → header commit); on success it
sets `live=card` and calls `orb_jobs_reboot_soon("theme <slug>")`. On any
failure it returns the reason to the row's value (the `not loadable`/`why`
rule is unchanged) and the device stays up. There is no second press to
have faith in: the rows "reboot now" and "keep this dial", the `staged`
field, and the `+2` in `clock_opt_count` are deleted — the count is the row
list again, plus the Factory default row from 13.2. While the job runs the
shell's existing busy treatment holds the page (no ghost press; the C8
lesson: a press the firmware swallows is a press the user must be told
about). Reboot timing stays the OS's — `orb_jobs_reboot_soon` is already
the only door, and the delay that lets the last frame reach the panel is
its whole point.

### 13.4 D12 — one level of app-owned nesting, validated by the shell

ABI: add `orb_opt_kind_t (*opt_kind)(orb_app_t *, int idx)` to the iface
(`orb_app.h`; §1.4's paper list is its frozen text: `OPT_ROW`, `OPT_GROUP`,
`OPT_ITEM`). A NULL hook means every row is `OPT_ROW` — the entire pre-D12
ABI, and every existing answer; the registry wrapper defaults it like
`orb_app_opt_label` guards. Contiguous-section convention: an `ITEM` belongs
to the nearest preceding `GROUP`; `GROUP`s are top-level only. The shell
(menu `PG_APP`) hides `ITEM`s while their group is closed, and validates the
shape at entry — a leading `ITEM`, or an `ITEM` with no group, renders as
`ROW` and logs once, because a silently invisible row is the "knob appears
not to work" failure in list form. Inside an open group the cursor clamps
over that group's items only; a press on a `GROUP` opens or closes it (its
value column carries the state: `open`, or `1 of 4` naming the chosen item);
long-press inside an open group closes it and stays on the page — homing to
the dial remains C8's, at the router. `color_picker` passes NULL (flat page
unchanged — the pin that the NULL default is real); the clock's rows become
`ROW "Factory default"`, then one `GROUP "From the card"` whose `ITEM`s are
the scanned themes: the dogfood that proves the feature.
`host_menu_test.py` carries the shape table: group open/close, hidden-row
renumbering across the whole page, cursor clamps on both sides, the two
malformed-shape rows, NULL-hook regression — and the suite's mutation rule
applies: break the validator, at least one test goes red.

### 13.5 What "done" means

Build clean; every host suite green; `carousel_bench.py` green (unrelated to
themes, but the iface literal changes every registered app's table); then
on orb-A, the owner's hand: MENU → the clock's options, enter the theme
sub-menu, pick a card theme — the board installs, reboots itself, and
returns on the new dial with no row ever asking; pick Factory default and it
returns without touching the card. After both trips, the flashed bytes at
slot A's region still hash to `assets/clock_assets.bin` (the D13 invariant
proven on hardware, not asserted). Record the run here as §13.6 and the
§0.3 rows are signed off. If the pick-to-reboot sequence should also ask the
PWA for a confirm, that is a new owner question, not an implementation
choice.

### 13.6 The sign-off run, 2026-10-12, orb-A

Image: the tree these three commits record, stamped `c0a224b-dirty`, audited by
`tools/serflash.sh` (`AUDIT OK: the image's own stamp matches the working tree`)
before the write at `0x20000`. Card: `SD16G SDHC 29.4GB @20000kHz`, four containers:
`skeleton`, `diver`, `perpetual-calendar`, `neon`.

**The owner's walk, MENU → Clock → the card sub-menu → a theme.**
`menu: cursor MENU row 3/4: Clock`, press, then
`menu: cursor Clock row 1/2: From the card  (1 of 4: diver)`. The items are absent from
that page while the group is shut, and the header answers with what is on the wall. Press:
`menu: group row 1 open: From the card`, and the page becomes rows `2/6` to `5/6`, each
label indented, the live one carrying `(on the dial)`. Press again:
`menu: group row 1 closed: From the card`. Both toggles are the shell's, and the app never
heard about either.

**The cursor clamps inside the group.** Five downward detents from the header stop at
`5/6:   neon`, never at the back row. Eight upward detents stop at `2/6:   Skeleton`, never
onto the header. A hold gives `menu: hold closes group row 1: From the card`, and the hold
after it gives `menu: menu idle` plus `input: switch clock`: C8's escape hatch intact for
every screen with nothing to collapse.

**D11, the pick is the confirmation.** A press on `  Skeleton` and nothing else:
`orb_theme: installed "skeleton" into slot-B in 8850 ms — the dial reboots to draw it`,
`jobs: rebooting in 750 ms: theme install`. The board returned on the new dial:
`orb_theme: theme "skeleton" from slot-B (chosen), 1978973 B of pixels`,
`art_bank: bank "assets" @0x830000, 1978973 B mapped`,
`clock: art bank mapped: 1978973 B (theme needs 1978973 B) from slot-B`. No row asked
"reboot now" or "keep this dial" at any point, before or after.

**D13, the default is a choice rather than a file.** `POST /api/v1/sd/default` gives
`jobs: rebooting in 750 ms: theme: the factory default`, and the boot after it says
`orb_theme: slot-B holds "skeleton" but the dial is on the factory default` beside
`art_bank: bank "assets" @0x420000, 1173563 B mapped … from baked-in`. `/api/v1/sd` answers
`live:"default"`, `source:"baked-in"`. The card stayed shut for that trip.

**A refused install says why and changes nothing.** `POST /api/v1/sd/install?name=neon`
carries intact checksums and one complication record with no size. That install left the dial
where it was and answered on its own row: `/api/v1/sd` returned `try_slug:"neon"` with
`try_why:"comp is shown but has no size"`, while `live`, `source` and `active` kept their
previous values and no reboot was asked for. An unknown name logs
`orb_theme: install: no such theme on the card`. A missing or traversal `name` is a 400
before the worker hears about it: `name is missing or not a theme name`. The page confirms
first, on both paths: `if(!confirm('Install '+n+' and …` and
`if(!confirm('Go back to the facto…`.

**The interrupted-install invariant, forced by hand.** `erase_region 0x820000 0x1000` with
`live:"card"` (B's header gone): boot answers
`orb_theme: no installed theme — drawing the baked-in bank` and
`art_bank … @0x420000 … from baked-in`, and the watch still tells the time. No damage record
is set, because an absent slot is not a damaged one. Erasing the pixels beneath a live header
(`erase_region 0x830000 0x40000`) does set one: `source:"baked-in"`, `damage_slug:"diver"`,
`damage_why:"the installed copy did not verify — re-install it"`, `live` still `"card"`. One
press of Install repaired it: `source:"slot-B"`, `active:"skeleton"`, both reason fields empty.

**Slot A, measured rather than asserted.** After two installs, one headerless boot, one
corrupt-pixel boot and five reboots, the 1,173,563 B read from `0x420000` hashes to
`f6e21a315489851461d04930752b50151a826d0a0f20e13f82dc80d389f7d0d6`, byte-identical to
`assets/clock_assets.bin`.

**Endpoints.** `POST /api/v1/sd/load` and `POST /api/v1/sd/unstage` → 404 `Nothing matches
the given URI`. `POST /api/v1/sd/install?name=X` → 200 `{"state":"installing",…,"reboot":"automatic"}`.
`POST /api/v1/sd/default` → 200 `{"state":"default","live":"default","reboot":"automatic"}`.

**Host side.** `idf.py build` clean. `host_sd_test`, `host_tz_test`, `host_menu_test` and
`host_app_registry_test` green, each with its `--mutations` battery reporting zero survivors,
and `carousel_bench.py` reporting all assertions held.

**Two notes for the next person at the bench.** `/api/v1/screen?turn=N` moves exactly one row
per request, and its sign comes from `knob_row_step`, so a scripted walk sends one request per
detent and turns negative to go down the list. The first attempt here pressed `IT_AP` three
times believing it had reached the clock page, and the `menu: cursor …` line is what named the
mistake. The three commits land as menu, theme, net: checked out alone, the theme commit does
not link, because it deletes `orb_theme_stage_load()`, `orb_theme_unstage()` and
`orb_theme_pending_slug()` while the previous commit's page and picker still call them. The API
deletion and its consumers are one atomic change. Squash the three if a buildable `git bisect`
matters more than the three rulings read separately.

§0.3: D11, D12 and D13 signed off against this run.

### 13.7 The dial that would not wake, 2026-10-12, orb-A

**Report.** The owner: the dial no longer wakes from the dimmed state. Intermittent — both wake
sources answered when tested at the bench minutes later.

**Ruled out on the bench, with the log as the witness.** The dim lands: `idle_dim: armed: dim to
20% after 60 s (knob + motion)`, then `co5300_spi: set brightness to 20% (hardware value: 51)` at
61 s of quiet. A knob event that reaches the router restores the glass in 32 ms:
`touch (http): 2 stamps since boot` at 300641 ms, `set brightness to 100% (hardware value: 255)`
at 300680 ms. So the poll timer runs in the dark, the decision is right and the write lands when
the panel is answering. One bench trap cost an hour: the orb's DHCP lease moved to
`192.168.2.32`, and every request to the old `192.168.1.177` hung until its timeout and looked
like a wedged device. Resolve the port and IP through `tools/orb_bind`, never from memory.

**The defect.** `idle_task` committed `s_dimmed = want` *before* asking the panel. The brightness
level is one QSPI write to the CO5300's `0x51`, `apply_level()` returned `void`, and a refused
write only logged. One refusal and the flag says lit while the glass says 20%: no later tick
disagrees with the flag, so the transition is never attempted again, and `per` — chosen from that
same flag — drops to the 1 Hz lit tick, taking the 20 Hz dark accelerometer poll, and the second
wake source, with it. `apply_level` is the file's only brightness writer and `s_dimmed` its only
assignment, which is why this one latch is the whole story inside the module.

**Fix.** `apply_level()` reports whether the panel took the write. The commit goes through
`idle_dim_level_apply_decide(want, dimmed, wrote)` — `want` if it landed, `dimmed` if not — so the
next tick asks again at the rate of the state the panel is actually in: a lost wake retries every
50 ms, a lost dim every second. No new timer, no new task. The dim-only side effects (the `0x51`
stamp the wake log measures from, the motion re-seed and run reset) now happen on a dim that
landed. The failure line is rate-capped once per second the way the touch line is, because a dead
panel is refused at 20 Hz.

**Made measurable.** `/api/v1/status` gains `idle.writes_failed`, the refused writes since boot,
and `tools/idle_watch.py` prints it as `wfail` and names the diagnosis when `dimmed` is stuck true
under a climbing `stamps`. Before this, a dial that woke on the third try and a dial that never
woke produced the same evidence.

**Pins.** `tools/host_idle_dim_test.py` gains 7 commit vectors (both refused directions, both
accepted, both no-ops, and the refused-then-accepted retry — the pair no single call can
distinguish), 4 predicate mutants and 3 mutants of the call site, checked by a lint on
`idle_task`'s own text: the only assignment to `s_dimmed` may be the one that asks the predicate.
Without the lint, `s_dimmed = want;` beside a correct predicate passes every vector and returns the
dark dial. 29 mutants, 0 survivors. All 11 host suites green, `idf.py build` clean, and the
dim-compiled-out variant (`build_vdim`) links.

**On hardware.** Flashed to orb-A (`AUDIT OK`), dim at 61 s, wake to `lit_pct=100%`, a second dim
after it, `wfail=0` throughout — the panel is answering now, so the retry did not have to.

**Limit, stated plainly.** The owner's episode was not reproduced. The CO5300's `0x51` has no
readback, so a write the driver *reports* as taken and the panel never applies stays invisible to
this fix; it covers the refused write, which is the failure the module could turn permanent. The
next report reads `idle.writes_failed` before it reads the glass.

### 13.8 Theme identity FORMAT 2 — outcome, hardware-verified 2026-10-12 (closes `docs/theme-naming-todo.md`)

FORMAT 2 replaces the slug+title pair with one `name` field; the card filename is
`sha256(name)[:12] + ".orbtheme"` and carries no identity of its own. The full design and the
green/blocked inventory live in `docs/theme-identity-plan.md`; that file's §7.4 promised this
outcome record before `docs/theme-naming-todo.md` was deleted, and the deletion is in this commit.

**Verified on orb-A at `73a5acf`, over the wire and on the glass.** FORMAT 1 containers refuse at
the format gate with an honest reason string, on the card list and at boot, and the dial degrades
to the baked-in bank instead of drawing garbage. Upload lands the card copy under the header's own
id — the request carries no name. Identical re-send is a no-op; same name with different bytes
answers 409 plus `overwrite:false`, and one deliberate `?overwrite=1` replaces. Install by id
mirrors into slot B, reboots, and boots logging `theme "skeleton" from slot-B (chosen)`; the row's
`changed` hint lights when the card file drifts from the installed mirror. Delete resolves by id
only: `id=skeleton` and uppercase hex are 400s. Worst-case `GET /api/v1/sd` measures 1395 B against
the 6144 B buffer with the list at its eight-entry cap and three 55-byte UTF-8 names.

**One defect fell out of the verification.** bake_clock.py truncated an over-55-byte `--theme-name`
silently — a 55-CHARACTER slice before the id hash and a 55-BYTE slice in orbtheme.py's header
write, disagreeing for multibyte names: the printed card id hashed a different string than the one
the header stored. Both layers now refuse with the byte count, matching the studio page's nameOk
rule, before any file is written. The device reader stays tolerant of a ≤56-byte stored name by
design: its id is the hash of the stored bytes, so any such file is self-consistent.

**Still open.** The node-gated `_selftest` suites (headless-orbtheme, export-test) have never been
executed — no node or Chrome on this machine. `diver`, `perpetual-calendar` and `neon` need their
owner's layer kits to re-bake. The archived `clock_app_default.orbtheme` is not regenerated: the
kit bake renders `show=[1,1,0,0,0]` against the archived `[1,1,0,1,0]`, so the original bake flags
are unknown and the archive stays byte-faithful to `skeleton_theme.zip` until they are.



---

## 14. M3 — the overflow bank: re-map (A) vs PSRAM arena (B) — captured 2026-10-12, orb-A

The last open item of P2 (§8's M3, §6's M3 text, §11's handed-over refusal
question). Same art, two builds, one variable: `ORB_ART_MECH_B` at 0 and at 1.
Everything else was held identical — same tree (`d7de71f` + this session's
bench code, stamp `d7de71f-dirty`, audited by `tools/serflash.sh` on every
push), same board (orb-A, `44:bd:8d:60:80:88`, wall-powered, ota_0, rssi −55),
same bench bank, same 31 iterations, same capture (`tools/capf.py`).

**The bank is real kit3, not a size.** The M2/LAT probes bill address ranges;
M3 reads pixels, so it measures on flashed bytes: `/tmp/m3-kit3.bin`,
3,858,874 B, md5 `aba381f34903574babf25320fd83ae22`, crc32_le `0x59801b57`,
baked from the committed `assets/sources/watch-orbital-kit3.zip` (prep_watch_kit
→ bake_clock, kit_check gate green) and written behind the clone's MAC gate.
The first campaign placed it at `assets+0x100000` (flash `0x520000`) on the
strength of an old comment claiming the baked bank ended at `0x11E000`; it
actually ends at `0x11E83B`, so the bench write ate the live bank's last
~122 KB and orb-A ran a corrupt dial (§14.4). The bench home is now slot B's
art, `M3_BANK_OFF = assets+0x410000` (flash `0x830000`) — under an erased,
never-installed slot header and clear of both slots' live bytes. The probe
refuses erased flash, verifies map-vs-read consistency, and now also compares
the whole plane against the flashed file's crc32_le before printing a number;
`/tmp/m3_bankwrite.sh` keeps the write reproducible.

**The probe** (`ORB_ART_M3_PROBE`, ships 0, same family as M2/LAT) prices one
app switch per arm: release → reclaim → first full-page commit (60 blocks of
60×60 through the shipping `gfx_blend_px` at alpha 128, sources striped one
per page band so every one of the bank's 59 pages is touched each pass). The
bench ladder (`DISPLAY_BENCH 1`, `PX 72`, `ALPHA 128` — 255 prices the wrong
op) then runs the dial at tempo reading the same bank through the arm's own
plane, and the per-second `clock N fps | ink px/s` lines are the rotation
steady state. `ORB_ART_MECH_B` is compile-time on purpose: a runtime switch
would be a second residency table the host mirror cannot pin.

### 14.1 The numbers (medians of 31, µs; the two arms' distributions do not overlap)

| | A (re-map) | B (PSRAM arena) |
|---|---|---|
| release (suspend) | 280 | 0 (carrying host) |
| reclaim (resume) | 318 | 1 (covered-entry lookup) |
| first full-page commit | 63,019 | 59,986 |
| **switch total (matched samples)** | **63,067** | **59,473** |
| boot cost (once) | — | **471,117** (arena fill, ≈ 8.2 MB/s) |
| holds | 59 MMU pages | 3,858,874 B PSRAM (free 7,947,908 → 4,081,280) |
| steady rotation | 6 fps med (83 lines), ink 1,555,200 px/s, hook 924 ms/s | 6 fps med (79 lines), ink 1,555,200 px/s, hook 921 ms/s |
| status LAN corroboration | `art_pages 77, art_banks 2` | `art_pages 18, art_banks 2` — the arena bills the window NOTHING |

A's switch reproduced within 0.15% across four boots (63,018/63,021/63,067/63,085).
Read against §12.7: an app switch through a cold re-map costs what an app
switch already cost (M1's 63–87 ms) — the µs-scale MMU op is noise under the
first composite, and the 5% the arena saves on reads (3.0 ms) is inside that
figure's spread. The release-for-free variant of B — an arena app whose
parked bank is NOT carried — costs a 251 µs free plus a **471 ms re-fill**:
for the arena, the carry decision of §6/M-carrier is not optional, it is the
mechanism's survival condition.

**Ruling — ratified by the owner 2026-10-12 as D14 (§0.4).** A stays the
mechanism for the overflow bank too: 3.6 ms of switch and 5% of read cost do
not pay 471 ms of boot and half the PSRAM (3.86 of 7.95 MB free), and steady
rotation is identical at the reporting resolution. B ships dark
(`ORB_ART_MECH_B 0`) as P4's escape hatch — it is the only way a third
flagship can ever boot, its gate (free minus bank minus 2 MB margin, refusal
counted with the byte figure in the log) is measured, and the ABI promise of
§3.3 is now code, host-pinned (`host_art_residency_test.py`: 16 checks, 11
mutations). If B's 3.6 ms is ever wanted badly enough to pay the PSRAM, the
define is one byte (`art_bank.h:98`).

### 14.2 The refusal hunt — §11's third finding, answered

The hunt maps every distinct 64 KB-aligned page of every partition the table
exposes — assets (128), www (20), ota_0 (32), ota_1 (32) — counts distinct
virtual bases against fold-backs (M2's dedup lesson: an overlapping-stride
version of this loop "ran" 1,165 folded requests and measured nothing),
and unmaps everything before the dial starts. **Result: 212 attempts, 211
fresh, 1 folded, zero refusals, 170 KB internal free at the end** — with the
image's 22 pages, the primary's 18 and the bank's 59 already resident, the
allocator accepted ≥ 310 region-pages. The §3.1 "256-page window" is
confirmed a `soc/mmu.h` region-count reading, not the capacity anyone refuses
at; below the size of the flash itself the allocator does not refuse at all.
`ORB_ART_PAGE_BUDGET` 234 therefore stays exactly what §11 called it — an
enforced contract, deliberately conservative, refusing early with a named
reason — and there is no measured ceiling to raise it toward. Two flagships
plus the image cost 142 of it; that is the real constraint's whole shape.

### 14.3 One incident worth a standing rule

The first M3 builds died their network boot: `httpd_start:
ESP_ERR_HTTPD_TASK` at the same millisecond every time — the probe's two 4 KB
flash-staging statics and its hunt arrays had eaten the last contiguous
internal block the httpd task's 8 KB stack needed (this heap's largest block
measures 7,168 B with a healthy boot). Fixed by moving every probe buffer to
PSRAM (`m2_stage_buf`, heap-allocated handle arrays). Standing rule for this
probe family: **boot-probe scratch earns no internal `.bss`** — a bench
instrument must not change what the board does after the instrument finishes,
which is the same reason the probes ship 0 in the first place.

### 14.4 The corrupt-dial incident — a bench fixture is flash content too

The bench bank's first home (`assets+0x100000`) was chosen from the LAT
header's claim that the baked bank ends "near 0x11E000". It ends at `0x11E83B`
— the bake output answers that in one line, and the probe's gate now quotes
it. The bench write therefore replaced the live bank's last 124,987 bytes with
kit3's head, orb-A rendered a corrupt dial, and `art_ready` stayed `true` all
the way through: the ship path verifies header geometry at load and runs no
whole-bank CRC (§11.2's "no CRC runs in the shipping path"), and the probe's
erase/CRC gates compare the mapping against the read path over the same bytes
— both said "consistent" about content nothing had cross-checked against the
baked file. The A/B numbers are unaffected: esptool verified the kit3 hash
after the write, and the probe reads only the bench range, which held genuine
kit3 — every measured byte was a byte of the bank it claims to price.
Recovery: re-flash the committed `assets/clock_assets.bin` at `assets+0`
behind the MAC gate, then read back and compare head and full tail —
byte-exact, `art_refusals 0`, dial restored. Two rules come out of it:
**a bench fixture earns no flash offset without checking the live bank's
exact end in the bake output**, and a probe that measures content gates on the
content's own hash, not just on self-consistency — hence the `crc32_le`
ground-truth gate, the relocated `M3_BANK_OFF`, and the retired-offset lint
pin in `host_art_residency_test.py`.


