#pragma once
/**
 * Display subsystem — brings up QSPI + LVGL.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the CO5300 panel and LVGL. Call once at boot. */
esp_err_t display_init(void);

/** Get the esp_lcd panel handle (for brightness, etc.). */
esp_lcd_panel_handle_t display_get_panel(void);

/**
 * Enable/disable the tear-effect (vertical-blanking) wait around each flush.
 * Only has an effect if the TE pin was proven live at init.
 */
void     display_set_te(bool on);

/** True when TE sync is currently active (pin live AND enabled). */
bool     display_te(void);

/** Cumulative microseconds spent waiting for TE (frame-budget telemetry). */
uint32_t display_te_wait_us(void);

/**
 * Full-page refresh benchmark counters, accumulated in the flush path: a frame is
 * booked from the submit of its first row-chunk to the DMA-completion of its last.
 * Snapshot the running totals (any out-param may be NULL); reset zeroes them.
 */
void display_bench_snapshot(uint32_t *frames, uint64_t *us_total, uint32_t *us_max);

/** Pixels + flush row-chunks handed to the panel since reset/before now. */
void display_bench_stats(uint64_t *pix, uint32_t *chunks);

/** Cumulative µs spent inside flush_cb (swap + mirror + draw_bitmap submit). */
uint64_t display_bench_submit_us(void);

void display_bench_reset(void);

#ifdef __cplusplus
}
#endif
