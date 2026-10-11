#pragma once
/**
 * menu_view — the dial's options screen, driven by the same rotary knob as the
 * HSB editor and the clock.
 *
 * Two-stage by design, mirroring color_picker: the screen first shows an inert
 * landing (a title and a hint), and only a knob press "arms" it. While unarmed,
 * rotating the knob is the router's screen switch; while armed, rotating scrolls
 * this screen's own list and a press chooses. A long-press always returns to the
 * clock, so there is exactly one way out of every state and it never depends on
 * where the user got stuck.
 *
 * Armed list: AP, TZ, THEME, exit.
 *   AP     prints why it is not on the dial yet (it needs a keyboard).
 *   TZ     browses the zone table in orb_net.c and applies one.
 *   THEME  lists the card, stages one, and reboots to install it.
 *   exit   back to the clock.
 *
 * Threading: every widget call in this file runs on the LVGL task, reached only
 * through menu_view_apply_input()/menu_view_service() from the router's poll.
 * The two things that can take seconds — the card walk behind orb_theme_scan()
 * and the verify behind orb_theme_stage_load() — run on a worker task of this
 * module, never on the LVGL task. input_router's poll IS the dial's animation
 * and the only input reader, so blocking it for the 38-48 s an SD upload can
 * hold the card mutex would freeze the screen and swallow the knob.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the menu's widgets on its own screen. Call after display_init. */
esp_err_t menu_view_init(void);

/** The menu's screen object, for input_router to lv_screen_load(). */
lv_obj_t *menu_view_screen(void);

/** Re-sync and repaint after this screen became the visible one. LVGL task. */
void menu_view_show(void);

/** True while the list (not the landing) owns the knob. */
bool menu_view_armed(void);

/**
 * Arm/disarm. Disarming drops back to the inert landing, so a screen reached by
 * rotation always starts from the title rather than mid-list where it was left.
 */
void menu_view_set_armed(bool armed);

/**
 * Apply one poll's worth of knob input while armed. delta = net detents
 * (>0 = CW), pressed = knob tapped. Call from the LVGL task only.
 */
void menu_view_apply_input(int32_t delta, bool pressed);

/**
 * Reap any finished worker job and repaint. Call from the LVGL task on every
 * poll, armed or not: a card walk that finishes while the user has rotated away
 * must still land, or the list would show a stale answer until they return.
 */
void menu_view_service(void);

#ifdef __cplusplus
}
#endif
