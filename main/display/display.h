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

/* ---- Cadence probe (temporary instrumentation; set to 0 after capture) ----
 * The 7-vs-8 fps question is flush-cadence variance, and second-averages
 * cannot answer it: one 200 ms frame and five 132 ms frames read identically
 * to six uniform 150 ms frames. With the flag on, every committed frame is
 * booked into a small ring at its DMA-completion (the same site that bumps
 * the bench frame count), and display_cadence_drain() prints them as one
 * serial line each. Call the drain from the clock's per-second report so the
 * records land interleaved with the CAD tk tick lines (same clock task, so
 * log order equals event order within a second). */
#define DISPLAY_CADENCE_PROBE 0

/* ---- FPS-budget bench instrumentation (leave all four at 0 when not sweeping)
 * docs/fps-budget-benchmark-plan.md Phase 2: price one blended pixel against one
 * refreshed row. The commit hook paints DISPLAY_BENCH_PX solid-colour blocks,
 * 60x60 = 3600 px each, so the ladder 18/36/72/112 adds the planned
 * 65k/130k/259k/403k px/tick, blended through the very blend_px() the real
 * layers use. Blocks walk every tick so the flush gate cannot settle them out
 * of the DMA, and the CAD bands/px fields are the proof the ink shipped.
 * Measured result: docs/fps-budget-findings-2026-10-10.md. */
#define DISPLAY_BENCH 0
#define DISPLAY_BENCH_PX 0
/* DISPLAY_BENCH_ALPHA selects the operation: 255 takes blend_px's opaque
 * shortcut (swap + write, no maths, measured 76 ns/px); 128 runs the real
 * mix_16_16 the dial's layers pay (185-232 ns/px). Sweeping at 255 prices the
 * wrong op and makes the px curve look flat. */
#define DISPLAY_BENCH_ALPHA 255
/* 0 = every block stays inside the shipped GIF row band [128..335], pinning
 * the row union while only the blend count moves. 1 = blocks spread over the
 * whole page, so the same ink count moves the ROW union instead: the only way
 * to measure the row term without changing the art. */
#define DISPLAY_BENCH_SPREAD 0

#if DISPLAY_CADENCE_PROBE
/** Print-and-clear every cadence record booked since the last drain. */
void display_cadence_drain(void);
#endif


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

/**
 * Lend the display's tile ring (2 x 18,640 B of internal DMA RAM) to another
 * peripheral for one sequence, and take it back. The SD driver's bring-up and
 * every unaligned sdmmc_write_words ask the same pool for small DMA-capable
 * blocks, and with the radio associated that pool fragments below the ask:
 * measured, 125 bytes refused with 4,315 bytes free. Lending is the only lever
 * that works, because it is the one large freeable block on the board.
 *
 * While the loan is open the flush path DROPS frames rather than submitting
 * them (PSRAM-direct is not an option at this board's trans_queue_depth), so
 * the dial freezes on its last frame — the panel holds its own GRAM, so nothing
 * blanks — and re-sends the whole page from the mirror when the ring returns.
 *
 * display_dma_borrow() returns the bytes handed over, or 0 when there was
 * nothing to lend: a board that booted without the ring, or a nested call
 * inside a loan that already covers it. Refcounted, so pairs may nest. Safe
 * from any task; the free waits for in-flight tiles to retire.
 *
 * display_dma_return() takes the ring back. ESP_ERR_NO_MEM means the memory did
 * not come back THIS call; the loan is still closed, and the flush path re-funds
 * the ring on its next frame, so the dial recovers on its own and a caller may
 * ignore the status. Until it does, the glass holds its last frame.
 */
size_t  display_dma_borrow(void);
esp_err_t display_dma_return(void);

void display_bench_reset(void);

#ifdef __cplusplus
}
#endif
