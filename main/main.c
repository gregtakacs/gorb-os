/**
 * orb-os-esp-idf — main application entry.
 *
 * MVP: Initialize CO5300 AMOLED + LVGL 9, display a centered digital clock.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "clock_view.h"

static const char *TAG = "main";

/* LVGL handler task — calls lv_timer_handler() periodically */
static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t sleep_ms = lv_timer_handler();
        if (sleep_ms > 50) sleep_ms = 50;
        if (sleep_ms < 5) sleep_ms = 5;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== orb-os-esp-idf booting ===");

    /* Initialize NVS (needed for future WiFi / settings) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Bring up the display (SPI + panel + LVGL) */
    ESP_ERROR_CHECK(display_init());

    /* Build the clock UI */
    ESP_ERROR_CHECK(clock_view_init());

    /* Launch LVGL handler task */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "orb-os-esp-idf running — clock displayed");
}
