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

/** Cumulative µs spent inside flush_cb (swap + mirror + TE wait + draw_bitmap
 *  submit — the whole callback, not just the submit phase). */
uint64_t display_bench_submit_us(void);

/**
 * Mark a closed range of panel rows as needing to go out with the next flush,
 * without writing any pixels. The clock's commit-time layer painter repaints
 * rows that LVGL was never asked to re-render (the wedge a rotating hand
 * vacates), so nothing else would put them in the band set — the mirror would
 * be right and the glass wrong. Range is clipped to the panel; LVGL-task only.
 */
void display_mark_rows(int32_t y0, int32_t y1);

/**
 * Hook invoked at flush-commit AFTER every chunk (LVGL-rendered and pushed)
 * has been mirrored into the panel framebuffer but BEFORE the TE wait and the
 * band DMAs read it out. The clock uses this single safe window to composite
 * its overlay layers straight into the mirror (bypassing LVGL's ~500 cyc/px
 * rotated-blit re-composite). Runs on the LVGL task; pass NULL to detach.
 */
typedef void (*display_commit_hook_t)(void);
void display_set_commit_hook(display_commit_hook_t hook);

/** The panel mirror (byteswapped RGB565, stride == SCREEN_WIDTH) for direct
 *  compositing at commit time; NULL when the coalescing mirror is off.
 *  LVGL-task only, and only meaningful from inside the commit hook. */
uint16_t *display_fb_mirror(void);

void display_bench_reset(void);

#ifdef __cplusplus
}
#endif
