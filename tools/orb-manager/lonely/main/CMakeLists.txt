idf_component_register(
    SRCS "lonely_main.c" "font5x7.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_lcd espressif__esp_lcd_co5300)