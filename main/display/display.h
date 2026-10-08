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

#ifdef __cplusplus
}
#endif
