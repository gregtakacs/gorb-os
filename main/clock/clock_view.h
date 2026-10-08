#pragma once
/**
 * Digital clock view — renders time text centered on the AMOLED display.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the clock UI (call after display_init + lv_init). */
esp_err_t clock_view_init(void);

#ifdef __cplusplus
}
#endif
