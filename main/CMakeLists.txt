idf_component_register(
    SRCS
        "main.c"
        "app/orb_app.c"
        "app/orb_app_install.c"
        "app/orb_app_manifest.c"
        "app/orb_app_scan.c"
        "app/orb_carousel.c"
        "app/orb_jobs.c"
        "app/orb_svc.c"
        "display/display.c"
        "gfx/orb_gfx.c"
        "gfx/orb_sched.c"
        # P5.5: the app layer table. Its header stays LVGL-free and the suite
        # shims the lvgl.h its .c reaches for, so tools/host_orblayers_test.py
        # compiles the shipped file as-is — the suite is the proof the
        # gate-to-table path is honest.
        "gfx/orb_layers.c"
        "clock/clock_view.c"
        "clock/clock_themes.c"
        "knob/knob.c"
        "color/color_picker.c"
        "menu/menu_view.c"
        "input/input_router.c"
        "imu/imu.c"
        "power/idle_dim.c"
        "art/art_bank.c"
        "art/orb_theme.c"
        # P5b: the asset registry. Its own directory because it is the one file in
        # the tree that knows what a slot IS, and it compiles without LVGL on
        # purpose — tools/host_orbapp_reader_test.py links it straight into the
        # manifest reader's harness, which no other main/ TU makes possible.
        "assets/orb_assets.c"
        "net/orb_net.c"
        "net/orb_mdns.c"      # _orb._tcp advertiser (docs/orb-manager-plan.md §5 item 4)
        "sd/orb_sd.c"
        "sd/orb_files.c"
    INCLUDE_DIRS
        "."
        "app"
        "display"
        "gfx"
        "clock"
        "knob"
        "color"
        "menu"
        "input"
        "imu"
        "power"
        "art"
        # P5b: main/assets is on the include path because orb_app_manifest.h includes
        # orb_assets.h for the class enum, and the reader must not have to know that
        # the vocabulary lives one directory over from the art it describes.
        "assets"
        "net"
        "sd"
        "../assets"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_driver_i2c         # imu: the QMI8658 on the shared I2C bus (GPIO 15/14)
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
)

# The baked analog-clock art (assets/clock_assets.bin, produced by
# tools/bake_clock.py from an orb-os theme folder) is NOT embedded in the app
# image any more. It used to arrive here via target_add_binary_data(), which put
# ~1.85 MB of pixels into .rodata and made every OTA payload carry art that
# never changed between themes.
#
# It now lives in the `assets` data partition (partitions.csv), is flashed by
# the esptool_py_flash_target_image() rule in the top-level CMakeLists.txt, and
# is mapped once at boot by main/art/art_bank.c; clock_view.c's blob() returns
# the mapped base, so sprite reads are unchanged. Geometry parsed offline from
# the theme's clock_style.json still lives in assets/clock_assets.h.


