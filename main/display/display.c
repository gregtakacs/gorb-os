/**
 * Display bring-up: SPI2 QSPI bus -> panel_io -> CO5300 panel -> LVGL 9.
 */

#include "display.h"
#include "co5300.h"
#include "board_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "lvgl.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;
static lv_display_t             *s_lvgl_disp;

#define LVGL_BUF_LINES 40

/* ---- esp_lcd color-transfer-done callback (DMA complete) ---- */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

/* ---- LVGL 9 flush callback ---- */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    int x1 = area->x1, x2 = area->x2 + 1;
    int y1 = area->y1, y2 = area->y2 + 1;
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, px_map);
    /* flush_ready is called from on_color_trans_done when DMA completes */
}

/* ---- LVGL tick source ---- */
static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- Public API ---- */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing SPI2 QSPI bus");

    /* 1. SPI bus (quad mode on GPSPI2) */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_LCD_D0,
        .miso_io_num     = PIN_LCD_D1,
        .sclk_io_num     = PIN_LCD_SCLK,
        .quadwp_io_num   = PIN_LCD_D2,
        .quadhd_io_num   = PIN_LCD_D3,
        .max_transfer_sz = SCREEN_WIDTH * LVGL_BUF_LINES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "spi_bus_initialize");

    /* 2. Panel IO (QSPI). The DMA-done callback is registered below via
     *    esp_lcd_panel_io_register_event_callbacks so we can pass the
     *    LVGL display handle as user_ctx. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = -1,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = LCD_SPI_CLOCK_HZ,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .flags             = { .quad_mode = true },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                 &io_cfg, &s_panel_io),
        TAG, "panel_io_spi");

    /* 3. CO5300 panel */
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_co5300(s_panel_io, PIN_LCD_RST, &s_panel),
        TAG, "new_panel_co5300");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel_init");

    /* 4. LVGL 9 init */
    ESP_LOGI(TAG, "Starting LVGL 9");
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    /* Allocate draw buffers in internal DMA-capable RAM */
    const uint32_t buf_bytes = SCREEN_WIDTH * LVGL_BUF_LINES * 2;  /* RGB565 = 2 bytes/px */
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        free(buf1);
        free(buf2);
        return ESP_ERR_NO_MEM;
    }

    /* Create LVGL display */
    s_lvgl_disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_buffers(s_lvgl_disp, buf1, buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);

    /* Register DMA-done callback: LVGL flush_ready is called when the
     * QSPI transfer finishes. */
    esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_lvgl_disp),
        TAG, "register_io_callbacks");

    ESP_LOGI(TAG, "Display ready (%dx%d)", SCREEN_WIDTH, SCREEN_HEIGHT);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}
