idf_component_register(
    SRCS
        "main.c"
        "app/orb_app.c"


        "app/orb_app_install.c"
        "app/orb_app_manifest.c"
        "app/orb_app_scan.c"
        # P7.2: the wasm3 behaviour module. Own TU because it is the only file in the
        # tree that reaches the interpreter; the registry sees it as one iface behind
        # one behaviour_kind, exactly like the picker.
        "app/orb_app_wasm.c"
        # The widget and canvas service a wasm app draws through (§43): per-slot
        # handle tables, so an app names its own objects and never an OS pointer.
        "app/orb_ui.c"
        "app/orb_carousel.c"
        "app/orb_map_credit.c"
        "app/orb_jobs.c"
        # §43: the HTTPS service a wasm app fetches through (the flight feed's gate
        # and TLS discipline, with no idea what a flight is) and the per-app
        # credential store behind /api/v1/apps/cfg's secrets.
        "app/orb_app_net.c"
        # D51: the streaming JSON tokenizer apps parse through, natively. Pure C,
        # host-tested as shipped by tools/host_json_test.py.
        "app/orb_json.c"
        "app/orb_setui.c"
        # §59: the carousel's owner order and its rotation; pure decisions, host-tested.
        "app/orb_rotation.c"
        "app/orb_svc.c"
        "display/display.c"
        # C2 (theme-delta plan §6.2): the boot/install overlay — the one painter
        # that runs with LVGL unarmed, on esp_lcd_panel_draw_bitmap alone.
        "display/orb_boot_overlay.c"
        # The shell-transition bench (/api/v1/debug/fx): effects painted outside LVGL.
        "display/orb_fx.c"
        "display/orb_strip.c"
        "gfx/orb_gfx.c"
        "gfx/orb_glass.c"
        "gfx/orb_sched.c"
        # D43 §5.4: LVGL's RGB565 image blends with mix_16_16's rounding (the
        # custom-ASM hooks main/lvhook/orb_lv_blend.h declares).
        "gfx/orb_lv_blend.c"
        # P5.5: the app layer table. Its header stays LVGL-free and the suite
        # shims the lvgl.h its .c reaches for, so tools/host_orblayers_test.py
        # compiles the shipped file as-is — the suite is the proof the
        # gate-to-table path is honest.
        "gfx/orb_layers.c"
        "stage/orb_stage.c"
        "stage/orb_live_work.c"
        "stage/orb_label.c"
        "knob/knob.c"
        # radar-plan §2.4: the map's vector renderer. Pure C, host-tested as shipped
        # by tools/host_map_raster_test.py.
        "map/orb_map_raster.c"
        # radar-plan §2.4: the OS maps (views on their own task, the card read under §2.12).
        "map/orb_map.c"
        # radar-plan §2.3: the indexed-PNG reader over the ROM's tinfl, and kept bodies.
        "gfx/orb_png8.c"
        "app/orb_app_blob.c"
        "menu/menu_view.c"
        "menu/knob_edit.c"
        # §43: the Themes page, an OS menu row (it was the clock app's options page).
        "menu/themes_page.c"
        "menu/theme_strip.c"
        "input/input_router.c"
        "imu/imu.c"
        # D39: the sound engine (I2S + ES8311 + NS4150B and the mixer) and its codec.
        # docs/history/sound-plan.md §4; the clock schedules, this plays.
        "audio/orb_es8311.c"
        "audio/orb_sound.c"
        "audio/orb_sound_app.c"      # the svc entries' half: an app's theme sounds by slot
        "audio/orb_sound_menu.c"     # the OS's Sound page: level and quiet hours, every app
        "power/idle_dim.c"
        "art/art_bank.c"
        "art/orb_theme.c"
        "art/orb_bundle.c"
        # theme-pool-plan §2: the pool's table and its compaction, every flash access
        # through an io the caller supplies. tools/host_pool_test.py drives this exact
        # file over a simulated flash that loses power at every step.
        "art/orb_pool.c"
        # P6.5 §25.5(2): the FORMAT 3 clock section's projection. Separate from
        # orb_bundle.c on purpose — that file is the format mirror of
        # tools/orbbundle.py and carries no dialect knowledge; this one is the clock's
        # own reading of it. tools/host_clock3_test.py compiles the shipped file with
        # host gcc, so it stays free of anything but clk_types.h and orb_bundle.h.
        "art/orb_clock3.c"
        # D31's seam, D32's consumer contract: (class, slot, frame) to pixels, for
        # every app that draws from a theme rather than from a baked table. Host
        # coverage is tools/host_orbelems_test.py; the file itself carries no
        # dialect knowledge and no allocator, exactly like the two above it.
        "art/orb_elems.c"
        # §60: a deflated preview, inflated by the ROM's tinfl.
        "art/orb_inflate.c"
        # P5b: the asset registry. Its own directory because it is the one file in
        # the tree that knows what a slot IS, and it compiles without LVGL on
        # purpose — tools/host_orbapp_reader_test.py links it straight into the
        # manifest reader's harness, which no other main/ TU makes possible.
        "assets/orb_assets.c"
        "assets/orb_ui_icons.c"
        "net/orb_net.c"
        "net/orb_https.c"
        "net/orb_home.c"
        # radar-plan §2.1: the three places beside home, one OS setting.
        "net/orb_places.c"
        "net/orb_tzrule.c"
        # weather-plan §2.1: the units every app reads (temperature, speed, distance)
        "net/orb_units.c"
        "net/orb_geo.c"
        "net/orb_cities.c"
        "net/orb_tls_mem.c"   # mbedTLS allocator: PSRAM for the task that asks
        "net/orb_mdns.c"      # _orb._tcp advertiser (docs/history/orb-manager-plan.md §5 item 4)
        "sd/orb_sd.c"
        "sd/orb_files.c"
        # D38: the MTP file-share mode, in its own file because the SD layer it
        # borrows the card from is not its owner. MSC is out of the tree, not out
        # of the menu — see docs/history/usb-sd-msc-plan.md §13.1.
        "usb/orb_usb_mtp.c"
        "usb/orb_usb_msc.c"
        # The RAM log ring: /api/v1/log's source, and the only witness that survives
        # the file-share window taking the console (docs/history/usb-sd-msc-plan.md §13.3).
        "util/orb_log.c"
        # The last panic, recorded in RTC memory by a wrap of esp_panic_handler (below)
        # and reported by /api/v1/status after the reset (2026-10-31).
        "util/orb_panic.c"
        "util/orb_layout_carry.c"
        "util/orb_io_window.c"
    INCLUDE_DIRS
        "."
        "app"
        "display"
        "gfx"
        "stage"
        "knob"
        "map"
        "menu"
        "input"
        "imu"
        "audio"
        "power"
        "art"
        # P5b: main/assets is on the include path because orb_app_manifest.h includes
        # orb_assets.h for the class enum, and the reader must not have to know that
        # the vocabulary lives one directory over from the art it describes.
        "assets"
        "net"
        "sd"
        "usb"
        "util"
    REQUIRES
        wasm3                    # P7.2: the vendored interpreter behind the wasm tier
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c         # imu: the QMI8658 on the shared I2C bus (GPIO 15/14)
        esp_driver_i2s         # audio: the ES8311's I2S link (D39)
        esp_driver_usb_serial_jtag   # prov_reader_task: install the USJ interrupt driver so select() on stdin fires
        esp_lcd
        esp_timer
        esp_hw_support
        esp_psram
        esp_partition          # art bank: find + mmap the `assets` partition
        spi_flash              # "
        esp_wifi               # config page: STA + first-boot provisioning SoftAP
        esp_netif              # "
        esp_event              # "
        esp_http_server        # plain-HTTP config API (TLS terminated upstream)
        esp_http_client        # orb_https: the outbound client the flight feed polls through
        lwip                   # "
        esp_app_format         # esp_app_get_description() for the status endpoint
        app_update             # OTA: esp_ota_begin/write/end + set_boot_partition + rollback confirm
        bootloader_support     # esp_ota_img_states_t / esp_flash_partitions.h (status "img" field)
        nvs_flash
        mbedtls                  # theme_id(): mbedtls_sha256 over the header name -> the card filename
        fatfs                    # microSD: esp_vfs_fat_sdspi_mount + FATFS volume
        sdmmc                    # "            (sdmmc_card_t, card info)
        esp_driver_sdspi         # "            (SDSPI host on SPI3_HOST)
        lvgl__lvgl
        espressif__esp_lcd_co5300
        espressif__esp_tinyusb  # D38: the MTP gadget behind `orb usb files` (MSC is gone from the tree)
)

# No art is compiled in or flashed: every face is a FORMAT 3 theme the store door
# installs into the `assets` partition, which main/art/art_bank.c maps once at boot.

# orb_panic.c records every panic before ESP-IDF's handler prints and resets: the
# wrap is the only hook that sees faults, abort() and both watchdogs alike.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# D43 §5.4: LVGL calls these three blend hooks (main/lvhook/orb_lv_blend.h), but the
# linker scans this component's archive before LVGL's, so nothing would pull
# orb_lv_blend.o in and LVGL's references would go unresolved. -u makes them
# undefined from the start, so the object is extracted on the first pass.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u orb_lv_rgb565_with_opa"
                      "-u orb_lv_rgb565_with_mask" "-u orb_lv_rgb565_mix_mask_opa")
