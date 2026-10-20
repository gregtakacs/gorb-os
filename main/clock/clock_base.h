/* clock_base.h — what the dial shows when there is no theme.
 *
 * §26, ruled 2026-10-19: a freshly-flashed orb carries no theme at all, and the app
 * falls back to its base form drawn from OS primitives. The owner's face for the clock
 * is the digital one — large time, date beneath, one status line — chosen as the
 * cheapest thing that is unambiguously legible at arm's length, and as the base form
 * that asks least of a theme it has not been given.
 *
 * It is deliberately NOT a small version of the analog dial. An analog base would
 * imply hand geometry the OS has no right to invent (§25.3's direction: refuse, and
 * say so, rather than draw a wrong layer), and it would make the base render look like
 * a failed theme. A digital readout claims nothing about art. It says the time, which
 * is the one promise M6's third pass clause keeps when no card has ever been in the
 * socket.
 *
 * Three widgets, two fonts and zero pixels from a bank: the LVGL default font for the
 * label, the built-in large font for the time, and the same orb_time_now() every app
 * reads. No clk_theme_t, no blob, no orb_bg — the point of the module is that it works
 * when all three are absent.
 */
#ifndef CLOCK_BASE_H
#define CLOCK_BASE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"          /* lv_obj_t, lv_label_*, the fonts this face uses */

/** Build the base face on `parent` (the clock's screen). Idempotent: a second call
 *  reuses the widgets, so a theme removed at runtime re-shows the same objects rather
 *  than stacking a second time readout under the first. */
void clock_base_show(lv_obj_t *parent);

/** Release the widgets, so a real theme's layers are the only things on the screen. */
void clock_base_hide(void);

/** Drive the readout on its own one-second timer. Deliberately NOT the dial's
 *  scheduler: orb_sched's policy is a sprite view's (pose hands, step a frame bank,
 *  mark layer boxes) and a text face has none of those jobs, so borrowing it would
 *  tick at 12 fps to redraw a string that changes once a minute. Started by
 *  clock_view_init() on the base path and stopped by clock_base_hide(). */
void clock_base_run(void);

/** True between show and hide. clock_view's tick calls through this rather than
 *  testing a theme pointer, so "the base face is up" is stated in one place. */
bool clock_base_visible(void);

/** Refresh the readout from the OS clock. Called on the dial's own tick, so the base
 *  face keeps time by the same rule and the same zone as a themed one — the zone lives
 *  in orb_net and reaches this through localtime_r(), which is the only reason a base
 *  render and a theme can be said to show the same time. */
void clock_base_tick(void);

#endif /* CLOCK_BASE_H */
