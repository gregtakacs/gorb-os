/**
 * clock_view.c — digital text clock, centered on screen, refreshed every second.
 * Uses LVGL 9 API. Refresh driven by an lv_timer so it runs in the LVGL task.
 */

#include "clock_view.h"
#include "board_config.h"

#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "clock";

static lv_obj_t   *s_clock_label;
static lv_timer_t *s_tick_timer;

/* Free-running time base: starts at a fixed epoch, ticks every second.
 * Replace with SNTP sync when WiFi is added. */
#define CLOCK_EPOCH 1700000000  /* Nov 2023 baseline */
static uint32_t s_elapsed_s = 0;

/* Runs in the LVGL task context — safe to touch widgets directly. */
static void tick_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_elapsed_s++;

    time_t t = (time_t)CLOCK_EPOCH + (time_t)s_elapsed_s;
    struct tm tm_out;
    localtime_r(&t, &tm_out);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
             tm_out.tm_hour, tm_out.tm_min, tm_out.tm_sec);

    lv_label_set_text(s_clock_label, buf);
}

esp_err_t clock_view_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* True-black background for AMOLED */
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* Remove default screen padding so the label centers cleanly */
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* Create the centered time label */
    s_clock_label = lv_label_create(scr);
    lv_obj_set_style_text_color(s_clock_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_pad_all(s_clock_label, 0, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_clock_label, "--:--:--");

    /* 1-second LVGL timer (dispatched from lv_timer_handler) */
    s_tick_timer = lv_timer_create(tick_timer_cb, 1000, NULL);

    /* Render the first value immediately */
    tick_timer_cb(s_tick_timer);

    ESP_LOGI(TAG, "Clock view initialized");
    return ESP_OK;
}
