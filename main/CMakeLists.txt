idf_component_register(
    SRCS
        "main.c"
        "display/display.c"
        "clock/clock_view.c"
        "knob/knob.c"
        "color/color_picker.c"
        "input/input_router.c"
    INCLUDE_DIRS
        "."
        "display"
        "clock"
        "knob"
        "color"
        "input"
        "../assets"
    REQUIRES
        esp_driver_spi
        esp_driver_gpio
        esp_lcd
        esp_timer
        esp_hw_support
        esp_psram
        nvs_flash
        lvgl__lvgl
        espressif__esp_lcd_co5300
)

# Embed the baked analog-clock art (assets/clock_assets.bin, produced by
# tools/bake_clock.py from an orb-os theme folder): the GIF keyframe + delta
# rects behind the hands, then cropped planar-RGB565A8 hand/shadow/overlay
# sprites. LVGL samples it as flash-resident rodata. Symbols + geometry
# (parsed offline from the theme's clock_style.json and the GIF's frame bank)
# live in assets/clock_assets.h. The blob lives in .rodata, which is why the
# factory app partition in partitions.csv is sized to hold ~4 MB (app + bank).
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/../assets/clock_assets.bin" BINARY)


