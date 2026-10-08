/**
 * Display bring-up for the CO5300 1.75" round AMOLED over QSPI.
 *
 * Uses the official espressif/esp_lcd_co5300 driver (component "espressif/esp_lcd_co5300").
 *
 * Key points discovered on hardware:
 *  - QSPI CO5300 needs 32-bit opcode-prefixed command framing; the vendor
 *    QSPI config macros handle that (lcd_cmd_bits = 32, flags.quad_mode).
 *  - The ESP32-S3 SPI GDMA driver cannot read source buffers out of PSRAM
 *    (spi_common.c sets access_ext_mem = false), so LVGL draw buffers MUST
 *    live in internal DMA-capable RAM. Hence LVGL runs in PARTIAL render
 *    mode with modest double buffers instead of a full 466x466 PSRAM frame.
 */

#include "display.h"
#include "board_config.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "lvgl.h"

static const char *TAG = "display";

/* Number of screen rows per LVGL partial buffer.
 * 466 px * 40 rows * 2 B = 37,280 B per buffer (x2 = ~75 KB internal RAM). */
#define LVGL_BUF_LINES  40

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;
static lv_display_t             *s_lvgl_disp;
static uint16_t                 *s_scratch;   /* aligned re-pack buffer, internal DMA RAM */

/* ---- esp_lcd color-transfer-done callback (QSPI DMA finished) ---- */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;   /* no yield needed */
}

/* ---- LVGL 9 flush callback ----
 *
 * The CO5300 over QSPI requires each flush window to start on an EVEN column
 * and span an EVEN number of columns: the SPI GDMA engine in quad mode needs a
 * transfer length that is a multiple of 4 bytes (2 px * 2 B). When LVGL renders
 * a partial region with an odd x-start or odd width (e.g. the centered clock
 * label), flushing it verbatim shears/slants every row. The Arduino reference
 * lib solves this with a rounder_cb; LVGL 9 dropped rounder_cb, so we align the
 * rectangle here and re-pack the pixel rows into an even-width scratch buffer.
 *
 * Padding columns are filled black — invisible on the true-black AMOLED clock.
 */
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    const int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    const int w = x2 - x1 + 1;
    const int h = y2 - y1 + 1;

    int32_t ax1 = x1 & ~1;                 /* round x-start down to even */
    int     aw  = (w + 1) & ~1;            /* round width up to even     */
    if (ax1 + aw > SCREEN_WIDTH) {         /* keep window inside the panel */
        aw = (SCREEN_WIDTH - ax1) & ~1;
    }

    if (ax1 == x1 && aw == w) {
        /* already even-aligned: DMA the LVGL buffer directly */
        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px_map);
    } else {
        /* re-pack rows into the even-width scratch buffer */
        const uint16_t *src = (const uint16_t *)px_map;
        const int left = x1 - ax1;
        for (int r = 0; r < h; r++) {
            uint16_t *d = s_scratch + (size_t)r * aw;
            for (int c = 0;  c < left;      c++) d[c] = 0x0000;
            memcpy(d + left, src + (size_t)r * w, (size_t)w * sizeof(uint16_t));
            for (int c = left + w; c < aw;  c++) d[c] = 0x0000;
        }
        esp_lcd_panel_draw_bitmap(s_panel, ax1, y1, ax1 + aw, y2 + 1, s_scratch);
    }
    /* flush_ready fires from on_color_trans_done when the DMA completes. */
}

/* ---- LVGL tick source ---- */
static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ---- Public API ---- */
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing CO5300 QSPI display (%dx%d)",
             SCREEN_WIDTH, SCREEN_HEIGHT);

    /* 1. LVGL core first, so the display handle exists before panel IO
     *    (the IO's trans-done callback carries it as user_ctx). */
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    const size_t buf_bytes = (size_t)SCREEN_WIDTH * LVGL_BUF_LINES * 2; /* RGB565 */
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_scratch  = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2 || !s_scratch) {
        free(buf1);
        free(buf2);
        free(s_scratch);
        s_scratch = NULL;
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers in internal DMA RAM");
        return ESP_ERR_NO_MEM;
    }

    s_lvgl_disp = lv_display_create(SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_display_set_buffers(s_lvgl_disp, buf1, buf2, (uint32_t)buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lvgl_disp, lvgl_flush_cb);

    /* 2. QSPI bus on SPI2 (GPSPI2) — dedicated data0..3 pins */
    const spi_bus_config_t bus_cfg =
        CO5300_PANEL_BUS_QSPI_CONFIG(PIN_LCD_SCLK,
                                     PIN_LCD_D0, PIN_LCD_D1,
                                     PIN_LCD_D2, PIN_LCD_D3,
                                     buf_bytes);
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
        TAG, "spi_bus_initialize");

    /* 3. Panel IO — 32-bit commands + quad mode (QSPI CO5300 protocol) */
    esp_lcd_panel_io_spi_config_t io_cfg =
        CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_color_trans_done, s_lvgl_disp);
    io_cfg.pclk_hz = LCD_SPI_CLOCK_HZ;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                 &io_cfg, &s_panel_io),
        TAG, "panel_io_spi");

    /* 4. CO5300 panel via the official vendor driver */
    co5300_vendor_config_t vendor_cfg = { 0 };
    vendor_cfg.flags.use_qspi_interface = 1;

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_co5300(s_panel_io, &panel_cfg, &s_panel),
        TAG, "new_panel_co5300");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel),             TAG, "panel_reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),              TAG, "panel_init");

    /* Apply the round-panel GRAM offset: the visible 466 px map to GRAM
     * columns 6..471 (vendor init sets CASET 0x06..0x1DD). Without this the
     * rightmost columns show stale GRAM (green splotch) and the image shifts. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_COL_OFFSET, LCD_ROW_OFFSET),
                        TAG, "panel_set_gap");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    ESP_LOGI(TAG, "Display ready (%dx%d), LVGL buffers: %u B x2 internal DMA RAM",
             SCREEN_WIDTH, SCREEN_HEIGHT, (unsigned)buf_bytes);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}
