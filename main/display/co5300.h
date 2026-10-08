#pragma once
/**
 * CO5300 AMOLED panel driver for ESP-IDF (QSPI via esp_lcd).
 *
 * Registers a custom esp_lcd_panel_t that speaks to the CO5300 controller
 * over a quad-SPI bus on the ESP32-S3.
 */

#include "esp_lcd_types.h"
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the CO5300 panel on the given SPI bus and return an
 *        esp_lcd_panel handle that LVGL / drawing code can use.
 *
 * @param io      Pre-configured esp_lcd_panel_io handle (QSPI mode).
 * @param reset_gpio  GPIO number wired to panel RES.
 * @param out_panel   Receives the panel handle on success.
 * @return ESP_OK on success.
 */
esp_err_t esp_lcd_new_panel_co5300(esp_lcd_panel_io_handle_t io,
                                   int reset_gpio,
                                   esp_lcd_panel_handle_t *out_panel);

/**
 * @brief Set panel brightness (0–255) via the CO5300 0x51 command.
 */
esp_err_t co5300_set_brightness(esp_lcd_panel_handle_t panel, uint8_t brightness);

#ifdef __cplusplus
}
#endif
