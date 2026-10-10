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

/** Commit-phase breakdown of the submit time (any out-param may be NULL):
 *  mirror = chunk byte-swap into s_fb, hook = the clock's L4 composite,
 *  te = wait_for_te, dma = the band submits (bounce copies, not wire time).
 *  Same monotonic totals as display_bench_submit_us(); read them as deltas. */
void display_bench_phases_us(uint64_t *mirror, uint64_t *hook,
                             uint64_t *te, uint64_t *dma);

/**
 * Mark a closed range of panel rows as needing to go out with the next flush,
 * without writing any pixels. The clock's commit-time layer painter repaints
 * rows that LVGL was never asked to re-render (the wedge a rotating hand
 * vacates), so nothing else would put them in the band set — the mirror would
 * be right and the glass wrong. Range is clipped to the panel; LVGL-task only.
 */
void display_mark_rows(int32_t y0, int32_t y1);

/**
 * display_mark_rows for the clock's commit-time painter: same band marking,
 * but the rows are additionally registered as the clock's own, so
 * display_commit_row may later un-mark them if the re-composite reproduced
 * what the mirror already held.
 */
void display_mark_rows_clocked(int32_t y0, int32_t y1);

/**
 * Decide whether one row participates in the next flush. The clock's commit
 * hook owns this decision for every row it re-composites: it has the mirror,
 * so it can compare against what the glass already shows and skip the DMA of
 * rows whose repaint came out byte-identical (a rotating carriage re-blends
 * a whole disc every tick but visibly changes only its wedge). Sets both the
 * in-flight and not-yet-folded row sets so the answer sticks whichever side
 * of the frame-start fold the call lands on. LVGL-task only.
 */
void display_commit_row(int32_t y, bool send);

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
