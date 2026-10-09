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
#include "color_picker.h"
#include "knob.h"
#include "input_router.h"
#include "orb_net.h"

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

    /* Initialize NVS (WiFi driver's own store: namespace "nvs.net80211").
     *
     * The recovery erase below is safe for user data because application
     * settings and WiFi credentials do NOT live in this partition — they are in
     * the separate `storage` partition (partitions.csv) owned by main/net. At
     * worst a wipe here costs the driver's cached profile and PHY calibration,
     * both of which are re-derived; orb_net_start() re-pushes the stored SSID
     * and passphrase on every boot, so credentials survive it. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "corrupt/unformatted NVS — erasing (WiFi creds are safe in `storage`)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Bring up the display (SPI + panel + LVGL) */
    ESP_ERROR_CHECK(display_init());

    /* Build the two screens. Both register their own LVGL screen object; the
     * input router decides which one is loaded. The GIF background animates
     * from inside the clock view (delta-rect bank), so there is no separate
     * orb/bench screen any more. */
    ESP_ERROR_CHECK(clock_view_init());
    ESP_ERROR_CHECK(color_picker_init());

    /* Rotary knob: GPIO edge ISRs feed the detent + press streams. */
    ESP_ERROR_CHECK(knob_init());

    /* Route knob events to the active screen (color picker first) and start the
     * ~10 ms input poll, which runs on the LVGL task created below. */
    ESP_ERROR_CHECK(input_router_init());

    /* Launch LVGL handler task */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "orb-os-esp-idf running — knob HSB editor live (long-press: switch screen)");

#if CONFIG_ORB_NET_ENABLE
    /* Start the network AFTER the LVGL task exists, so the dial is already
     * ticking while the radio comes up and a bring-up failure can never show up
     * as a blank screen. Deliberately not ESP_ERROR_CHECK: the clock is the
     * product, and this is a device in a sealed case. */
    if (orb_net_start() != ESP_OK)
        ESP_LOGE(TAG, "networking down — clock running without config page");
#endif
}

