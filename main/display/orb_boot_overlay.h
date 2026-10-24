#pragma once
/**
 * Boot / install overlay — the one thing that can paint while the theme may be
 * the thing that failed (docs/theme-delta-mmap-plan.md §6.2).
 *
 * Deliberately built on esp_lcd_panel_draw_bitmap, not LVGL: it must work with
 * LVGL down (boot, before display_init's buffers exist) and with LVGL parked
 * (install, §6.3). No theme font, no filesystem, no PSRAM: one 466x24 row-band
 * buffer in internal DMA RAM, one compiled-in 5x7 bitmap font, RGB565 fills.
 *
 * The caller owns the glass when it calls this: at boot LVGL has drawn nothing
 * yet; at install the clock parks first (§6.3). The first real LVGL commit
 * retires the overlay by simply painting over it.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Draw the overlay frame naming `stage` (lowercase words drawn as-is; unknown
 * bytes render as '?'). `total` == 0 draws the stage line only; total > 0 adds
 * the ring arc and the `done/total` KB line. `secs` is the elapsed figure the
 * caller wants named; 0 lets the overlay compute it from its own first draw and
 * apply §6.4's 4 s rule: once the boot has run 4 s, every frame carries the
 * elapsed seconds next to the stage, so a boot that hangs names where it hung.
 *
 * Redraws are throttled to 10 Hz for a changed `done` (§6.3's copy must not be
 * rate-limited by the panel); a changed `stage` always redraws at once. Safe to
 * call before display_panel_early()? No — it refuses, loudly, until the panel
 * exists; the boot ladder calls display_panel_early() first.
 */
void orb_boot_overlay(const char *stage, uint32_t done, uint32_t total, uint32_t secs);

/**
 * Paint the whole glass black and clear the throttle so the next orb_boot_overlay
 * frame cannot be skipped as unchanged (§32.8). The store door calls this right
 * before its first "storing" frame: the cold-boot black fill inside
 * orb_boot_overlay runs only once per boot, so an install that lands mid-run
 * needs its own backdrop, or the LVGL menu the dial parked behind shows above
 * and below the store card. Costs one ~22 KB black fill over QSPI, once.
 */
void orb_boot_overlay_backdrop(void);

#ifdef __cplusplus
}
#endif