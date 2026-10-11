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
 * MVP routing (three screens, two levels):
 *
 *   Roaming — nothing is armed:
 *     rotate    : swap between MENU -> HUE -> CLOCK -> MENU
 *     push      : arm the current screen (enter its list/editor)
 *     long-press: go home to the clock
 *
 *   Armed — the screen owns the knob:
 *     CLOCK     : inert (it has no list to enter)
 *     HUE       : rotate adjusts the selected HSB channel, push swaps channel
 *     MENU      : rotate scrolls AP / TZ / THEME / exit, push chooses
 *     long-press: always back to the clock
 *
 * Long-press deliberately no longer cycles screens: rotate owns that gesture now,
 * and one button cannot mean "next screen" two different ways depending on how
 * long it is held. Long-press is the single escape hatch out of every armed
 * state, which is what makes the two-level model recoverable.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_SCREEN_MENU = 0,
    INPUT_SCREEN_COLOR,
    INPUT_SCREEN_CLOCK,
    INPUT_SCREEN_COUNT
} input_screen_t;

/**
 * Register the screens and start the input poll timer. Call after knob_init(),
 * clock_view_init(), color_picker_init() and menu_view_init(). Loads the initial
 * (clock) screen.
 */
esp_err_t input_router_init(void);

/** Switch to a screen (also used by the long-press gesture). Safe from LVGL task. */
void input_router_show(input_screen_t s);

/** Back to the clock, disarming anything that was armed. The escape hatch. */
void input_router_home(void);

/**
 * Feed the router one synthetic knob event. It enters at the same point a real
 * detent or tap would, so the whole two-level model — screen swap, arming,
 * scrolling, selecting — is reachable without a hand on the encoder.
 *
 * This exists for the bench: the knob is three GPIOs, so with nobody turning them
 * the menu and the picker had never been entered on real hardware, only reasoned
 * about. Reached from GET /api/v1/screen. Called only by the single httpd worker,
 * which is why the counters need no lock beyond their own atomicity.
 *
 * lpress asks for the long-press gesture specifically. It is injectable because
 * that gesture is the only way out of an armed screen: were the path broken, an
 * armed menu would soft-lock the dial, since rotate belongs to the list and push
 * chooses a row. It has to be testable rather than merely plausible. The real
 * gesture is still decided by the ISR from a measured hold time; this reaches
 * only the router's consumption of it.
 */
void input_router_inject(int32_t delta, bool pressed, bool lpress);

/** Which screen currently owns the knob. */
input_screen_t input_router_current(void);

/** True while some screen is armed, i.e. rotate belongs to it and not to the swap. */
bool input_router_armed(void);

/**
 * Ask the router to switch screens from another task. Safe to call from anywhere:
 * it only records the request, and the router's own poll on the LVGL task applies
 * it on the next tick, because lv_screen_load() may only be called by that task.
 * Used by GET /api/v1/screen so the screens can be verified without a hand on the
 * knob (the encoder is three GPIOs and nothing else can drive them from the bench).
 */
void input_router_request(input_screen_t s);

#ifdef __cplusplus
}
#endif
