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
static uint16_t                 *s_scratch;   /* aligned re-pack buffer, internal DMA RAM (fallback path only) */

/* Panel mirror: last-flushed content of every pixel, wire-order RGB565, in
 * PSRAM (434 KB). The CO5300 GRAM is write-only over QSPI, so when the flush
 * callback must widen an odd-aligned dirty rect by a pad column it would
 * otherwise destroy pixels it never received data for — black padding is fine
 * on a black UI but paints hairlines across a picture-filled dial (the clock
 * plate). The mirror lets the pad column be RESTORED to its previous pixel
 * instead. Every flush path writes back into it; if allocation fails we fall
 * back to black padding and just log once. */
static uint16_t *s_fb;
#define FB(x, y) (s_fb[(size_t)(y) * SCREEN_WIDTH + (x)])


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

/* ---- Full-page refresh telemetry (benchmark) ----
 * One LVGL frame in PARTIAL mode is a burst of row-chunks: s_flush_in_flight goes
 * false->true on the first chunk of the frame, and the chunk flagged
 * lv_display_flush_is_last() closes it. We stamp t0 when the first chunk is
 * submitted, and when the DMA for the LAST chunk completes here we book the
 * elapsed time as that frame's refresh cost. Chunk DMAs on a single SPI bus
 * complete in submission order, so the trans-done following the last-chunk submit
 * is the last chunk's -> exactly one frame booked per frame. This spans the whole
 * commit path (TE wait + swap + repack + DMA); LVGL's own pixel fill happens in
 * lv_timer_handler before flush_cb and shows up between frames (in the measured
 * fps). Declared here (before the callback) because the callback books them. */
static volatile uint32_t s_bench_frames;
static volatile uint64_t s_bench_frame_us_total;
static volatile uint32_t s_bench_frame_us_max;
static volatile uint64_t s_bench_pix;      /* px handed to DMA (all chunks) */
static volatile uint32_t s_bench_chunks;   /* flush_cb calls (row-chunks)   */
static volatile uint64_t s_bench_submit_us;/* time spent inside flush_cb    */
static volatile int64_t  s_frame_t0_us;
static volatile bool     s_last_pending;   /* last chunk submitted, awaiting DMA-done */

/* ---- Frame coalescing state (see lvgl_flush_cb) --------------------------
 * A single LVGL PARTIAL refresh hands us one row-chunk per flush_cb call (an
 * object taller than the 40-line buffer, or several disjoint invalid areas --
 * a hand's swept wedge, every GIF delta rect, the color swatch). Each chunk
 * used to be its own esp_lcd_panel_draw_bitmap: on the QSPI bus that per-call
 * serialization (~5.5 ms setup each) dominated the frame at ~17 calls -> 8 fps,
 * AND the rotated wedges/rects have odd x-bounds, so each flush widened to an
 * even column and restored the pad column from the panel mirror -- which the
 * not-yet-flushed neighbouring chunk had not updated yet, leaving a 1 px stale
 * hairline that walked as the box swept.
 *
 * Fix: coalesce. Mirror every chunk into the full-frame s_fb mirror as it
 * arrives (marking its rows dirty), release that LVGL buffer immediately, and
 * on the LAST chunk DMA the dirty rows merged into <= DB_MAX_BANDS full-width
 * horizontal bands taken straight from s_fb. Full-width (x=0..465) is always
 * even, so no pad column is ever synthesized and the stale-hairline race is
 * structurally impossible; and the whole frame commits behind a single TE wait
 * in one blanking window. s_fb is a complete mirror at that point, so every
 * byte we DMA is exactly what belongs on the glass. */
#define DB_MAX_BANDS 5
static uint8_t  s_dirty_row[SCREEN_HEIGHT];   /* row marked this frame (task ctx) */
static volatile int s_band_inflight;          /* bands DMA'd, awaiting trans-done  */
static bool     s_coalesce_mode = false;       /* true when the PSRAM mirror exists */


/* ---- esp_lcd color-transfer-done callback (QSPI DMA finished) ---- */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    if (s_coalesce_mode) {
        /* The only DMA that reaches here is the coalesced last-chunk band push.
         * Release the LVGL buffer (which LVGL re-arms for the NEXT frame) only
         * once the final band's DMA has completed, so the next frame cannot
         * mirror over s_fb while these bands are still being read out of it. */
        if (s_band_inflight > 0) s_band_inflight--;
        if (s_band_inflight == 0 && s_last_pending) {
            s_last_pending = false;
            lv_display_flush_ready((lv_display_t *)user_ctx);
            const uint32_t dur = (uint32_t)(esp_timer_get_time() - s_frame_t0_us);
            s_bench_frames++;
            s_bench_frame_us_total += dur;
            if (dur > s_bench_frame_us_max) s_bench_frame_us_max = dur;
        }
    } else {
        /* Fallback: one DMA per chunk, released as each completes (LVGL's own
         * double-buffer wait keeps the next chunk off the in-flight buffer). */
        lv_display_flush_ready((lv_display_t *)user_ctx);
        if (s_last_pending) {
            s_last_pending = false;
            const uint32_t dur = (uint32_t)(esp_timer_get_time() - s_frame_t0_us);
            s_bench_frames++;
            s_bench_frame_us_total += dur;
            if (dur > s_bench_frame_us_max) s_bench_frame_us_max = dur;
        }
    }
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

/* Fallback (no PSRAM mirror) path only: gates the single per-frame TE wait when
 * chunks are pushed one-by-one instead of coalesced at commit. */
static bool s_flush_in_flight_pad_gate_done = false;



/* Mirror one flush chunk's pixels into the full-frame panel mirror s_fb. LVGL
 * hands px_map as w*h contiguous wire-order-needful RGB565 (stride == w), the
 * byte-swapped-into-wire-order copy the panel wants is written straight into
 * s_fb at its true (x,y). Row stride of s_fb is SCREEN_WIDTH so a full-width
 * band is later DMA-able as one contiguous run without a repack scratch. */
static void mirror_chunk(int32_t x1, int32_t y1, int32_t w, int32_t h,
                         const uint16_t *src)
{
    for (int r = 0; r < h; r++) {
        uint16_t *d = &FB(x1, y1 + r);
        const uint16_t *s = src + (size_t)r * w;
        for (int c = 0; c < w; c++) {
            uint16_t v = s[c];
            d[c] = (uint16_t)((v >> 8) | (v << 8));
        }
    }
}

/* Coalesce the rows marked dirty this frame into <= DB_MAX_BANDS full-width
 * horizontal bands and DMA each straight from s_fb. Returns the band count
 * issued (>=1 always, so a frame with nothing dirty still reports cleanly). */
static int dma_dirty_bands(lv_display_t *disp)
{
    /* Contiguous dirty row-ranges, then merge toward DB_MAX_BANDS by bridging
     * the cheapest gaps. Bounded work: <= SCREEN_HEIGHT rows, a few passes. */
    int32_t r0[SCREEN_HEIGHT / 2 + 1], r1[SCREEN_HEIGHT / 2 + 1];
    int n = 0;
    int y = 0;
    while (y < SCREEN_HEIGHT) {
        if (!s_dirty_row[y]) { y++; continue; }
        r0[n] = y;
        while (y < SCREEN_HEIGHT && s_dirty_row[y]) y++;
        r1[n++] = y - 1;
    }
    /* Merge the closest-adjacent ranges until within the band budget. Bridging
     * a gap repaints those rows from s_fb with their current content -- cheap
     * and invisible -- versus paying another per-call QSPI serialization. */
    while (n > DB_MAX_BANDS) {
        int best = 0, best_gap = 0x7fffffff;
        for (int i = 0; i + 1 < n; i++) {
            int gap = r0[i + 1] - r1[i] - 1;
            if (gap < best_gap) { best_gap = gap; best = i; }
        }
        r1[best] = r1[best + 1];
        for (int i = best + 1; i + 1 < n; i++) { r0[i] = r0[i + 1]; r1[i] = r1[i + 1]; }
        n--;
    }
    if (n == 0) return 0;

    /* DMA each band straight from the PSRAM mirror s_fb. A full-width band is
     * one contiguous run in s_fb (its row stride == SCREEN_WIDTH == the band
     * width), so no repack scratch is needed. The band is full-width (x=0..465,
     * always even) so no odd pad column is ever synthesized -> the stale-
     * hairline race is structurally impossible; and because s_fb is a COMPLETE
     * mirror by commit time (every chunk was mirrored before any DMA -- the
     * commit is deferred to the last chunk) every byte DMA'd is exactly what
     * belongs on the glass.
     *
     * The SPI master cannot DMA from PSRAM (ESP32-S3 GPSPI has no EDMA), so it
     * bounce-copies each bus-sized chunk into a MALLOC_CAP_DMA temp; that is
     * why esp_lcd_new_panel_io_spi is created with trans_queue_depth==1 (see
     * display_init) -- a deeper queue piles up N simultaneous bounces and OOMs.
     * s_band_inflight MUST equal the number of draw_bitmap calls we ACTUALLY
     * enqueue; a failed submit gets no post-trans callback, so it is accounted
     * for inline or the LVGL task spins forever in wait_for_flushing. */
    s_band_inflight = n;
    s_last_pending  = true;
    for (int i = 0; i < n; i++) {
        const int bh  = r1[i] - r0[i] + 1;
        const uint16_t *src = &FB(0, r0[i]);
        s_bench_pix  += (uint64_t)SCREEN_WIDTH * (uint32_t)bh;
        s_bench_chunks++;
        esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, r0[i], SCREEN_WIDTH,
                                                 r1[i] + 1, src);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "coalesce band %d [r=%d..%d] draw_bitmap failed: %s (src=%p)",
                     i, (int)r0[i], (int)r1[i], esp_err_to_name(e), (const void *)src);
            if (s_band_inflight > 0) s_band_inflight--;
            if (s_band_inflight == 0 && s_last_pending) {
                s_last_pending = false;
                lv_display_flush_ready(disp);
            }
        }
    }
    return n;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area,
                          uint8_t *px_map)
{
    const int32_t x1 = area->x1, y1 = area->y1, x2 = area->x2, y2 = area->y2;
    const int w = x2 - x1 + 1;
    const int h = y2 - y1 + 1;

    const int64_t t_entry = esp_timer_get_time();

    /* First chunk of an LVGL frame: stamp the latency clock (the single TE wait
     * that gates the whole frame happens at commit time, in dma_dirty_bands). */
    if (!s_flush_in_flight) {
        memset(s_dirty_row, 0, sizeof(s_dirty_row));
        s_frame_t0_us   = esp_timer_get_time();
        s_flush_in_flight = true;
    }

    const bool is_last = lv_display_flush_is_last(disp);

    if (s_fb) {
        /* Zero-copy-ish accumulate: byte-swap this chunk into the mirror, mark
         * its rows, and hand the LVGL buffer straight back -- the actual QSPI
         * push is deferred to the last chunk as a handful of band DMAs. */
        mirror_chunk(x1, y1, w, h, (const uint16_t *)px_map);
        for (int r = y1; r <= y2; r++) s_dirty_row[r] = 1;

        if (is_last) {
            s_flush_in_flight = false;
            wait_for_te();
            int bands = dma_dirty_bands(disp);
            if (bands == 0) {                 /* nothing dirty: book the frame */
                s_last_pending = false;
                lv_display_flush_ready(disp);
            }
            /* else: flush_ready is signalled by on_color_trans_done once the
             * last band's DMA completes. */
        } else {
            lv_display_flush_ready(disp);      /* buffer copied to s_fb: release */
        }
    } else {
        /* No PSRAM mirror: fall back to the per-chunk push. These are rare and
         * each keeps its own byte-swap + even-alignment in a scratch repack. */
        int32_t ax1 = x1 & ~1;
        int     aw  = (w + 1) & ~1;
        if (ax1 + aw > SCREEN_WIDTH) aw = (SCREEN_WIDTH - ax1) & ~1;

        if (!s_flush_in_flight_pad_gate_done) {   /* one blanking per frame */
            wait_for_te();
            s_flush_in_flight_pad_gate_done = true;
        }
        if (ax1 == x1 && aw == w) {
            rgb565_swap((uint16_t *)px_map, (size_t)w * h);
            esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, px_map);
        } else {
            const uint16_t *src = (const uint16_t *)px_map;
            const int left = x1 - ax1;
            for (int r = 0; r < h; r++) {
                uint16_t *d = s_scratch + (size_t)r * aw;
                const uint16_t *s = src + (size_t)r * w;
                for (int c = 0;  c < left;    c++) d[c] = 0x0000;
                for (int c = 0;  c < w;       c++) {
                    uint16_t v = s[c];
                    d[left + c] = (uint16_t)((v >> 8) | (v << 8));
                }
                for (int c = left + w; c < aw; c++) d[c] = 0x0000;
            }
            esp_lcd_panel_draw_bitmap(s_panel, ax1, y1, ax1 + aw, y2 + 1, s_scratch);
        }
        if (is_last) {
            s_flush_in_flight = false;
            s_flush_in_flight_pad_gate_done = false;
            s_last_pending = true;
            s_band_inflight = 1;
        }
    }
    s_bench_submit_us += (uint64_t)(esp_timer_get_time() - t_entry);
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

    /* Panel mirror (see s_fb comment): CPU-only access, so PSRAM is fine and
     * costs us no internal DMA RAM. Black-init: matches the panel's post-reset
     * state and the flush path keeps every subsequent write in step. */
    s_fb = heap_caps_malloc((size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2,
                            MALLOC_CAP_SPIRAM);
    if (s_fb) {
        memset(s_fb, 0, (size_t)SCREEN_WIDTH * SCREEN_HEIGHT * 2);
        s_coalesce_mode = true;   /* whole-frame mirror present: coalesce flushes */
    } else {
        ESP_LOGW(TAG, "no PSRAM for panel mirror; odd-aligned flushes pad black");
    }

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
    /* The coalesced bands are DMA'd straight from the PSRAM mirror s_fb. The
     * ESP32-S3 GPSPI has no EDMA (it can't source a descriptor from PSRAM), so
     * the SPI master bounce-copies every bus-sized chunk of a PSRAM transfer
     * into its own internal MALLOC_CAP_DMA temp (spi_master.c: !esp_ptr_dma_capable
     * -> aligned_alloc+memcpy). Those bounce buffers live until the chunk's DMA
     * completes, so a trans_queue_depth of N lets N of them pile up at once: a
     * ~300 KB full-width band = ~8 chunks x ~37 KB = ~296 KB of internal DMA
     * demanded simultaneously -> heap_caps_aligned_alloc fails -> draw_bitmap
     * returns ESP_ERR_NO_MEM and the chunk is silently dropped (no trans-done,
     * so the flush would never complete). The default depth is 10; pin it to 1
     * so the driver frees each chunk's bounce (via the recycle in
     * panel_io_spi_tx_color) before allocating the next -> peak ~1 bounce. The
     * fps win is the reduction in per-BAND command serialisations (<=5), not
     * chunk queue depth, so this costs no throughput. */
    io_cfg.trans_queue_depth = 1;
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

/* ---- Full-page refresh benchmark counters (consumed by clock_view) ---- */
void display_bench_snapshot(uint32_t *frames, uint64_t *us_total, uint32_t *us_max)
{
    /* Single writer is the SPI ISR (on_color_trans_done), single reader is the
     * LVGL task; these counters only ever grow, so a torn read is at worst off by
     * one in-flight frame and is immediately corrected on the next report. No
     * critical section needed for a monotonic stats pull. */
    if (frames)    *frames    = s_bench_frames;
    if (us_total)  *us_total  = s_bench_frame_us_total;
    if (us_max)    *us_max    = s_bench_frame_us_max;
}

/* Pixels + row-chunks actually handed to the panel — tells a partial flush
 * (few thousand px) apart from a whole-screen invalidate (~217k px) without
 * reaching into LVGL's private display struct. */
void display_bench_stats(uint64_t *pix, uint32_t *chunks)
{
    if (pix)    *pix    = s_bench_pix;
    if (chunks) *chunks = s_bench_chunks;
}

uint64_t display_bench_submit_us(void)
{
    return s_bench_submit_us;
}

void display_bench_reset(void)
{
    s_bench_frames         = 0;
    s_bench_frame_us_total = 0;
    s_bench_frame_us_max   = 0;
    s_bench_pix            = 0;
    s_bench_chunks         = 0;
    s_bench_submit_us      = 0;
}
