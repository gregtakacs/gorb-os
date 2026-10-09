idf_component_register(
    SRCS
        "main.c"
        "display/display.c"
        "clock/clock_view.c"
        "knob/knob.c"
        "color/color_picker.c"
        "input/input_router.c"
        "art/art_bank.c"
        "net/orb_net.c"
    INCLUDE_DIRS
        "."
        "display"
        "clock"
        "knob"
        "color"
        "input"
        "art"
        "net"
        "../assets"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
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


