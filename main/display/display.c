/**
 * Display bring-up for the CO5300 1.75" round AMOLED over QSPI.
 *
 * Uses the official espressif/esp_lcd_co5300 driver (component "espressif/esp_lcd_co5300").
 *
 * Key points discovered on hardware:
 *  - QSPI CO5300 needs 32-bit opcode-prefixed command framing; the vendor
 *    QSPI config macros handle that (lcd_cmd_bits = 32, flags.quad_mode).
 *  - The ESP32-S3 SPI GDMA can only DMA straight from internal RAM; a PSRAM
 *    source buffer is silently bounce-copied into an internal MALLOC_CAP_DMA
 *    temp first (spi_master.c: !esp_ptr_dma_capable -> alloc+memcpy). So LVGL
 *    draw buffers live in internal DMA RAM for a zero-copy fast path, and LVGL
 *    runs in PARTIAL render mode. A full 466x466 PSRAM frame is only viable as
 *    a staged one-transaction-per-frame push that pays the bounce copy.
 *  - Tear-free: the panel's GRAM is scanned out independently of our QSPI
 *    writes, so a write landing mid-scan shows new top + old bottom at once.
 *    The official driver's default init already sends TEARON (0x35,{0x00}), so
 *    the panel drives LCD_TE (GPIO13) at vertical blanking; we poll it and only
 *    queue a flush during blanking. See wait_for_te().
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
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lvgl.h"

static const char *TAG = "display";

/* Number of screen rows per LVGL partial buffer.
 * 466 px * 40 rows * 2 B = 37,280 B per buffer (x2 = ~75 KB internal RAM). */
#define LVGL_BUF_LINES  40

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t    s_panel;
static lv_display_t             *s_lvgl_disp;
static uint16_t                 *s_scratch;   /* aligned re-pack buffer, internal DMA RAM */

/* ---- Tear-effect (TE) sync state ----
 * The panel already drives LCD_TE (we don't send TEARON — the official driver's
 * default init table includes {0x35, 0x00}). s_te_live is decided once at init by
 * watching the pin actually toggle, so a depopulated/absent signal doesn't make us
 * burn the full timeout on every frame. s_te_enable lets it be A/B tested at run
 * time via a console command. Mirrors orb-os's s_teOn / display_set_te(). */
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
static volatile bool     s_te_live   = false;   /* pin proven to toggle at init */
static volatile bool     s_te_enable = true;    /* runtime gate (A/B toggle) */
static volatile uint32_t s_te_wait_us = 0;      /* cumulative us spent waiting, for /health */
#endif

/* Block until the panel finishes a TE pulse (i.e. we are inside/just after
 * vertical blanking), so the queued QSPI write lands between scans. Ported from
 * orb-os's wait_for_te(): wait out the current HIGH, then wait out the LOW, so we
 * return on the rising edge that opens the blanking window. Bounded at 25 ms so a
 * missing pulse degrades to "no worse than tearing" instead of hanging the flush. */
static void wait_for_te(void)
{
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    if (!s_te_live || !s_te_enable) {
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    const int64_t deadline = t0 + 25000;                    /* 25 ms ceiling */
    while (gpio_get_level(PIN_LCD_TE) == 1 && esp_timer_get_time() < deadline) { }
    while (gpio_get_level(PIN_LCD_TE) == 0 && esp_timer_get_time() < deadline) { }
    s_te_wait_us += (uint32_t)(esp_timer_get_time() - t0);
#endif
}

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
/* RGB565 byte order.
 *
 * The CO5300 wants each 16-bit pixel sent big-endian over the wire (RRRRRGGG
 * byte first, then GGGBBBBB). The esp_lcd driver's draw_bitmap is a verbatim
 * tx_color — it does no reordering — and LVGL on the little-endian S3 stores
 * RGB565 low-byte-first. So every pixel we hand the panel is byte-swapped.
 *
 * This is invisible for grayscale (white/black text, the 0x1818/0x8080/0x9090
 * chips): those values are byte palindromes, so swapping them changes nothing.
 * It only betrays itself on saturated hues — which is why the clock looked
 * fine while the color picker's swatch was "way off". lv_color_hsv_to_rgb is
 * correct; the wire order was the culprit.
 *
 * We swap in place on px_map. Safe because LVGL re-renders the entire flush
 * rect from its own (unswapped) color state on every flush, so a given pixel
 * is only ever swapped once immediately before its DMA — never twice. */
static inline void rgb565_swap(uint16_t *px, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        px[i] = (uint16_t)((px[i] >> 8) | (px[i] << 8));
    }
}

/* A single LVGL refresh can hand us several row-chunks (an object taller than
 * the partial buffer, e.g. the 170px swatch with a 40-row buffer, arrives as
 * ~5 calls). If we gate EVERY chunk on its own blanking, the chunks land in
 * ~5 different vertical-blanking periods — the DMA itself is quick (~3ms) but
 * each wait_for_te() can stall up to a whole frame. The panel then scans out
 * the top of the object in its new colour for ~30ms while the lower chunks are
 * still the previous colour: a horizontal split line that lingers while you
 * turn the knob fast.
 *
 * So we gate on TE only for the FIRST chunk of a frame and let the remaining
 * chunks of that same frame stream back-to-back (they're already serialized by
 * flush_ready). One frame = one blanking window, so the whole object commits
 * together. lv_display_flush_is_last() tells us when the frame's last chunk has
 * been handed over, which re-arms the gate for the next frame. Single-chunk
 * flushes (the clock label) are always "first and last", so their behaviour is
 * unchanged — this only affects tall multi-chunk objects. */
static bool s_flush_in_flight = false;

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

    /* Blank out at most once per LVGL frame. */
    if (!s_flush_in_flight) {
        wait_for_te();
        s_flush_in_flight = true;
    }
    if (lv_display_flush_is_last(disp)) {
        s_flush_in_flight = false;         /* next flush is a new frame */
    }

    if (ax1 == x1 && aw == w) {
        /* already even-aligned: byte-swap in place, then DMA directly */
        rgb565_swap((uint16_t *)px_map, (size_t)w * h);
        esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px_map);
    } else {
        /* re-pack rows into the even-width scratch buffer, swapping as we go */
        const uint16_t *src = (const uint16_t *)px_map;
        const int left = x1 - ax1;
        for (int r = 0; r < h; r++) {
            uint16_t *d = s_scratch + (size_t)r * aw;
            const uint16_t *s = src + (size_t)r * w;
            for (int c = 0;  c < left;      c++) d[c] = 0x0000;
            for (int c = 0;  c < w;          c++) {
                uint16_t v = s[c];
                d[left + c] = (uint16_t)((v >> 8) | (v << 8));
            }
            for (int c = left + w; c < aw;  c++) d[c] = 0x0000;
        }
        wait_for_te();
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

    /* 5. Tear-effect sync. The panel is scanning by now and (its default init
     *    having sent TEARON) should be pulsing LCD_TE at vertical blanking.
     *    Configure the pin as a plain input, then PROVE the signal is actually
     *    moving before we ever spend time waiting on it — a pin that never
     *    toggles would burn the whole 25 ms timeout every frame. Better to find
     *    that out once, here, than to pay for it silently forever (mirrors the
     *    orb-os stash's toggle-probe). */
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    {
        const gpio_config_t te_cfg = {
            .pin_bit_mask = 1ULL << PIN_LCD_TE,
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&te_cfg), TAG, "te_gpio_config");

        bool moved = false;
        const int first = gpio_get_level(PIN_LCD_TE);
        const int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 50000) {          /* 50 ms window */
            if (gpio_get_level(PIN_LCD_TE) != first) { moved = true; break; }
        }
        s_te_live = moved;
        ESP_LOGI(TAG, "LCD_TE on GPIO%d: %s -> TE sync %s", PIN_LCD_TE,
                 moved ? "toggling" : "STUCK (no pulse in 50 ms)",
                 moved ? "ON" : "OFF");
    }
#endif

    ESP_LOGI(TAG, "Display ready (%dx%d), LVGL buffers: %u B x2 internal DMA RAM",
             SCREEN_WIDTH, SCREEN_HEIGHT, (unsigned)buf_bytes);
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return s_panel;
}

/* ---- Runtime TE A/B controls + frame telemetry (mirrors orb-os display_set_te)
 *      These are safe to call from the console; they only flip a bool. ---- */
void display_set_te(bool on)
{
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    s_te_enable = on;
    ESP_LOGI(TAG, "TE wait %s", on ? "on" : "off");
#else
    (void)on;
#endif
}

bool display_te(void)
{
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    return s_te_live && s_te_enable;
#else
    return false;
#endif
}

uint32_t display_te_wait_us(void)
{
#if defined(PIN_LCD_TE) && (PIN_LCD_TE >= 0)
    return s_te_wait_us;
#else
    return 0;
#endif
}
