#pragma once
/**
 * Digital clock view — renders HH:MM:SS on the AMOLED display.
 *
 * Off by default the clock is fixed and centred. clock_view_set_bounce(true)
 * makes the label bounce around the round screen as a cheap motion stress-test
 * of the flush pipeline; passing false re-centres it and stops the motion timer.
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the clock UI (call after display_init + lv_init). */
esp_err_t clock_view_init(void);

/** Enable/disable the bouncing motion. Safe to call any time after init. */
void clock_view_set_bounce(bool on);

/** Query current bounce state. */
bool clock_view_bounce(void);

#ifdef __cplusplus
}
#endif
