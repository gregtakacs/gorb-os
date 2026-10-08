#pragma once
/**
 * knob.h — Rotary encoder (KY-040) input, ESP-IDF port of orb-os/src/knob.cpp.
 *
 * The physical knob on the 8-pin header (H2), wired exactly as in the Arduino
 * reference (see board_config.h):
 *   Encoder CLK (A)   -> GPIO18   quadrature phase A
 *   Encoder DT  (B)   -> GPIO17   quadrature phase B
 *   Encoder SW (push) -> GPIO16   active-low momentary switch
 *   Encoder +         -> 3V3
 *   Encoder GND       -> GND
 *
 * Like the reference, ALL edge detection lives in GPIO ISRs, not in a poll
 * loop, for the two reasons orb-os discovered on hardware:
 *   - A press-and-release that both happen inside one slow loop iteration must
 *     not be missed (a hardware interrupt cannot miss it).
 *   - The detent stream that decides a gesture and the stream that moves the UI
 *     must be the SAME counter, so they can never disagree about what happened.
 *
 * The consuming API below mirrors orb-os (takeDelta / takePress / takeLongPress):
 * each returns pending input since the last call and then clears it. They read
 * volatile ISR state and are safe to call from the LVGL task via input_router.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the three GPIOs + install the edge ISRs. Call once from app_main. */
esp_err_t knob_init(void);

/**
 * Periodic housekeeping, call from a single task (the LVGL task) every ~10 ms.
 * Catches a sustained hold (long-press), which by definition spans many calls
 * and so does not need the ISR's edge precision. Rotation and the initial press
 * edge are handled entirely in the ISRs and need no poll.
 */
void knob_poll(void);

/** Net detents since last call: >0 turned right (CW), <0 turned left (CCW). */
int32_t knob_take_delta(void);

/** True exactly once if the knob was tapped (released under the hold time). */
bool knob_take_press(void);

/** True exactly once if the knob was held at least the long-press time. */
bool knob_take_long_press(void);

/** Milliseconds the knob has been continuously held right now (0 if released). */
uint32_t knob_held_ms(void);

/** The hold duration that promotes a hold to a long press (for a countdown UI). */
uint32_t knob_long_press_ms(void);

#ifdef __cplusplus
}
#endif
