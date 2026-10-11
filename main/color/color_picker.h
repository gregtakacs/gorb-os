#pragma once
/**
 * color_picker — HSB (hue / saturation / brightness) editor driven by the knob.
 *
 * Validation surface for the ported input path:
 *   rotate knob  -> adjust the currently selected channel (wraps hue, clamps s/v)
 *   push knob    -> swap which channel is selected (HUE -> SAT -> VAL -> HUE)
 *
 * The picker lives on its own LVGL screen so it coexists with the clock screen;
 * input_router decides which screen is loaded and forwards knob events here. All
 * LVGL work happens on the LVGL task (color_picker_apply_input is called from the
 * router's poll timer), so no cross-thread widget access.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the picker's widgets on its own screen. Call after display_init. */
esp_err_t color_picker_init(void);

/** The picker's screen object, for input_router to lv_screen_load(). */
lv_obj_t *color_picker_screen(void);

/**
 * Re-sync the widgets to the (persistent) model and repaint. Call from the LVGL
 * task right after this screen is loaded, so the swatch/chips reflect the last
 * chosen values rather than stale frame content.
 */
void color_picker_show(void);

/**
 * Apply one poll's worth of knob input. delta = net detents (>0 = CW),
 * pressed = knob was tapped. Call from the LVGL task only.
 */
void color_picker_apply_input(int32_t delta, bool pressed);

/**
 * True while the editor (not the "HSB" title card) owns the knob. input_router
 * keeps the picker unarmed until a press asks for it, so arriving here by
 * rotation shows a title rather than a swatch mid-edit, and a turn still means
 * "swap screen".
 */
bool color_picker_armed(void);

/**
 * Arm/disarm. Disarming collapses the editor and shows the "HSB" title card;
 * the chosen H/S/B values are kept, so re-arming returns to the same color.
 */
void color_picker_set_armed(bool armed);

/** Read back the current channel values (0..359, 0..100, 0..100). */
void color_picker_get(uint16_t *h, uint8_t *s, uint8_t *v);

#ifdef __cplusplus
}
#endif
