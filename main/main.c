/**
 * gorb-os — main application entry.
 *
 * MVP: Initialize CO5300 AMOLED + LVGL 9, display a centered digital clock.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "board_config.h"
#include "display.h"
#include "orb_app.h"
#include "orb_carousel.h"
#include "knob.h"
#include "input_router.h"
#include "imu.h"
#include "idle_dim.h"
#include "orb_net.h"
#include "orb_sd.h"
#include "orb_theme.h"
#include "art_bank.h"

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
    ESP_LOGI(TAG, "=== gorb-os booting ===");

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

    /* Resolve the theme — and carry out any install the config page staged — BEFORE
     * the display comes up. Two reasons, both load-bearing:
     *   - An install is an erase-and-rewrite of the multi-megabyte region the dial
     *     reads its pixels from. Doing it while the LVGL task is mid-paint is how a
     *     device shows half of one theme and half of another.
     *   - art_bank_init() must map the slot the CHOSEN theme lives in, and only this
     *     call knows which of the two that is.
     * The cost is that a boot with a staged load shows a dark screen for the seconds
     * the copy takes. That is the correct trade: it happens once, only when somebody
     * asked for a new theme, and the alternative is a dial that can corrupt.
     * Deliberately not ESP_ERROR_CHECK — a failed load falls back to the theme that
     * was already installed (or to the baked-in one), and the clock still has to tell
     * the time. */
    const esp_err_t theme_err = orb_theme_boot();
    if (theme_err != ESP_OK)
        ESP_LOGE(TAG, "theme bring-up degraded (%s) — clock runs on the fallback bank",
                 esp_err_to_name(theme_err));

#if ORB_ART_M2_PROBE
    /* M2 residency protocol (docs/app-platform-plan.md §6): runs after the
     * theme's art_bank_init() so the primary counts, and before display_init()
     * so the dial's own fps lines, later in the same boot log, are measured
     * with every probed bank still mapped. */
    art_bank_m2_probe();
#endif

    /* Bring up the display (SPI + panel + LVGL) */
    ESP_ERROR_CHECK(display_init());

    /* Build the apps and their screens. Each registers its own LVGL screen
     * object; the input router decides which one is loaded. The GIF background
     * animates from inside the clock view (delta-rect bank), so there is no
     * separate orb/bench screen any more. */
    /* Build the apps, in registry table order. main.c no longer names any of
     * them: the table in main/app/orb_app.c decides what boots, in what order,
     * and an app whose init fails is dropped from the carousel with its reason
     * logged instead of taking the dial down with it.
     *
     * The order the table encodes is the order these three were called in
     * before, and it is not arbitrary: the clock creates the worker task the
     * theme path needs, and the menu creates the worker task the blocking card
     * calls run on. Creating that second worker before the LVGL task exists is
     * deliberate and safe — it only ever calls the orb_theme and orb_net APIs
     * and copies results into its own buffer, never a widget, which is the one
     * thing that needs the LVGL task to exist first. */
    ESP_ERROR_CHECK(orb_app_init_all());

    /* The carousel's name chip, on the top layer, built once here so no flick
     * ever allocates. Deliberately NOT an ESP_ERROR_CHECK: the chip is chrome,
     * and a dial that cannot paint a destination falls back to switching
     * immediately on each turn (orb_carousel_ready()) rather than refusing to
     * boot into a sealed case. */
    const esp_err_t carousel_err = orb_carousel_init();
    if (carousel_err != ESP_OK)
        ESP_LOGE("carousel", "chip refused (%s) — the knob walks without a peek",
                 esp_err_to_name(carousel_err));

    /* Rotary knob: GPIO edge ISRs feed the detent + press streams. */
    ESP_ERROR_CHECK(knob_init());

    /* Route knob events to the active screen (color picker first) and start the
     * ~10 ms input poll, which runs on the LVGL task created below. */
    ESP_ERROR_CHECK(input_router_init());

    /* Idle dim, last of the bring-up: it needs the panel (to write brightness)
     * and the IMU (to poll it in the dark), and neither is worth a dependency
     * elsewhere. The IMU is brought up only when motion wake is enabled, so
     * ORB_IDLE_MOTION_ENABLE=n drops the driver out of the image entirely. */
#if CONFIG_ORB_IDLE_MOTION_ENABLE
    ESP_ERROR_CHECK(imu_init());
#endif
    ESP_ERROR_CHECK(idle_dim_init());

    /* Launch LVGL handler task */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "gorb-os running — knob: turn swaps menu/hue/clock, push opens, hold returns to clock");

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    /* Rollback handshake. With the safety net compiled in, the bootloader boots a
     * freshly OTA'd image in PENDING_VERIFY and REVERTS it at the next reboot
     * unless the app confirms itself workable. "Workable" here means the dial
     * actually drew, so wait for a couple of real flushes from the LVGL task
     * instead of confirming the instant we reach this line: an image that comes
     * up, wedges the display, and leaves a frozen clock is exactly the case the
     * net exists for, and a blind delay would wave it through. */
    for (int waited_ms = 0; waited_ms < 6000; waited_ms += 100) {
        uint32_t frames = 0;
        uint64_t us_total = 0;
        uint32_t us_max = 0;
        display_bench_snapshot(&frames, &us_total, &us_max);
        if (frames >= 2) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    const esp_err_t rb = esp_ota_mark_app_valid_cancel_rollback();
    if (rb != ESP_OK) {
        /* Not fatal: the clock is running. But this image stays unconfirmed, so a
         * reboot sends it back to the previous slot — say so loudly. */
        ESP_LOGE(TAG, "rollback not cancelled (%s) — next reboot may revert to previous image",
                 esp_err_to_name(rb));
    }
#endif

#if CONFIG_ORB_NET_ENABLE
    /* Start the network AFTER the LVGL task exists, so the dial is already
     * ticking while the radio comes up and a bring-up failure can never show up
     * as a blank screen. Deliberately not ESP_ERROR_CHECK: the clock is the
     * product, and this is a device in a sealed case. */
    if (orb_net_start() != ESP_OK)
        ESP_LOGE(TAG, "networking down — clock running without config page");
#endif

#if CONFIG_ORB_SD_ENABLE
    /* Same rule as the network, and for the same reason: the TF socket has no
     * card-detect line, so finding out whether a card is in it means driving
     * the SPI bus through an init ladder that can take a second per failed
     * rung. That belongs after the dial is drawing, never before it. */
    if (orb_sd_init() != ESP_OK)
        ESP_LOGW(TAG, "no SD card — themes can still be installed from the "
                      "card once one is inserted (POST /api/v1/sd/rescan)");
#endif
}

