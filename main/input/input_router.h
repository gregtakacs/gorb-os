#pragma once
/**
 * input_router — the single source of truth for "what the knob does", mirroring
 * orb-os/src/input_router.cpp. One poll's worth of knob input (a signed detent
 * delta + press/long-press flags) is turned into screen actions here, so the
 * meaning of the control lives in exactly one place.
 *
 * Runs its poll as an LVGL timer on the LVGL task — the only context allowed to
 * touch widgets — pulling the consuming knob_* accessors and dispatching to
 * whichever screen is currently loaded.
 *
 * MVP routing:
 *   color screen: rotate adjusts the selected HSB channel, push swaps the channel
 *   clock screen: (turn/press currently unclaimed)
 *   long-press  : switch between the color and clock screens
 */

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_SCREEN_COLOR = 0,
    INPUT_SCREEN_CLOCK,
    INPUT_SCREEN_COUNT
} input_screen_t;

/**
 * Register the screens and start the input poll timer. Call after knob_init(),
 * clock_view_init() and color_picker_init(). Loads the initial (color) screen.
 */
esp_err_t input_router_init(void);

/** Switch to a screen (also used by the long-press gesture). Safe from LVGL task. */
void input_router_show(input_screen_t s);

/** Which screen currently owns the knob. */
input_screen_t input_router_current(void);

#ifdef __cplusplus
}
#endif
