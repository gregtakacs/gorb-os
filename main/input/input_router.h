#pragma once
/**
 * input_router — the single source of truth for "what the knob does", mirroring
 * orb-os/src/input_router.cpp. One poll's worth of knob input (a signed detent
 * delta + press/long-press flags) is turned into screen actions here, so the
 * meaning of the control lives in exactly one place.
 *
 * Runs its poll as an LVGL timer on the LVGL task — the only context allowed to
 * touch widgets — pulling the consuming knob_* accessors and dispatching to
 * whichever entry the app registry says is loaded.
 *
 * The router no longer knows what a screen is. It knows the carousel steps one
 * walkable registry entry at a time (orb_app_neighbour), that the armed entry
 * owns the turn (orb_app_current_armed), and that the long-press goes to
 * orb_app_home() — the C8 ruling, expressed as a table flag rather than as a
 * hardcoded INPUT_SCREEN_CLOCK. What each screen does with a turn is the app's
 * own iface, reached through orb_app_input().
 *
 *   Roaming — nothing is armed:
 *     rotate    : step the carousel to the next walkable entry
 *     push      : arm the current entry (enter its list/editor)
 *     long-press: go home to the dial
 *
 *   Armed — the entry owns the knob:
 *     the dial  : inert (it has no list to enter)
 *     the picker: rotate adjusts the selected HSB channel, push swaps channel
 *     the menu  : rotate scrolls AP / TZ / THEME / exit, push chooses
 *     long-press: always back to the dial
 *
 * Long-press deliberately no longer cycles screens: rotate owns that gesture
 * now, and one button cannot mean "next screen" two different ways depending on
 * how long it is held. Long-press is the single escape hatch out of every armed
 * state, which is what makes the two-level model recoverable.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "orb_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the input poll timer. Call after orb_app_init_all() and knob_init():
 * the registry must have built every screen before this one loads the first,
 * and the poll must not start before the apps exist. Loads the dial.
 */
esp_err_t input_router_init(void);

/** Put a registered entry on the glass (also used by the long-press gesture).
 *  Call on the LVGL task. Returns the registry's reason if it refused. */
esp_err_t input_router_show(const orb_app_t *app);

/** Back to the home entry, disarming anything that was armed. The escape hatch. */
void input_router_home(void);

/**
 * Feed the router one synthetic knob event. It enters at the same point a real
 * detent or tap would, so the whole two-level model — screen step, arming,
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

/** Which registry entry currently owns the knob; NULL before the first switch. */
const orb_app_t *input_router_current(void);

/** True while some entry is armed, i.e. rotate belongs to it and not to the step. */
bool input_router_armed(void);

/**
 * Ask the router to switch entries from another task. Safe to call from
 * anywhere: it only records the pointer, and the router's own poll on the LVGL
 * task applies it on the next tick, because lv_screen_load() may only be called
 * by that task. Used by GET /api/v1/screen so the screens can be verified
 * without a hand on the knob (the encoder is three GPIOs and nothing else can
 * drive them from the bench).
 *
 * The pointer comes from orb_app_find_slug(), never from an integer the caller
 * typed: an unknown slug is refused at the route, so nothing here can be
 * talked into loading a screen that does not exist.
 */
void input_router_request(const orb_app_t *app);

/**
 * Write the resume record for a slug without touching the glass — for the task
 * that cannot load a screen but knows what the next boot should show. The theme
 * store's worker uses it: the store parks the dial on the menu before the copy
 * (the erase needs the map down), which would otherwise leave `menu` as the
 * remembered page, and every post-store reboot would answer the owner's clock
 * upload with the menu. Naming "clock" here says what the store really did —
 * it replaced the face — and the reboot resume honours it. Nothing loads: the
 * boot's own resume_last resolves the slug exactly as it does for a remembered
 * page, and falls back home if the row refuses or vanished.
 */
void input_router_remember_slug(const char *slug);

#ifdef __cplusplus
}
#endif
