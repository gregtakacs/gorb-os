/**
 * clock_themes.h — the clock app's theme options, answered through the ABI.
 *
 * B1/B2's actual content. The theme picker used to be a page inside the shell's
 * menu: menu_view.c carried the card walk, the staging, the reboot question and
 * the row text, which meant the OS knew what a theme was. Under B2 the clock app
 * owns the theme picker and the OS only renders rows, so everything in this
 * header is a question the shell asks without knowing what the answer means.
 *
 * The split that makes this possible is the one the file service established:
 * the OS enumerates /sd/themes and hands back names (orb_files), the theme layer
 * says whether a file is loadable and what it takes to install it (orb_theme),
 * and this file is the only place that knows any of that is worth showing. The
 * shell still owns the cursor, the clamp, the five visible rows and the knob's
 * feel — the same primitives the menu's own pages use, which is the point: the
 * dial's options and the browser's copy of them come from one set of rows.
 *
 * Threading. The two operations that can take seconds — walking the card and
 * streaming a container through CRC (measured at 2952 ms for 3.87 MB) — never
 * run here. They go to orb_jobs_post(), which runs them on the shell's one
 * worker task, and their results are reaped by clock_themes_service() on the
 * LVGL task. opt_label() and opt_value() therefore only ever read a copy that
 * already arrived; a page that asked the card for a name would hold the task
 * that paints the dial, and the knob would stop answering mid-turn.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Ask the worker for the card's theme list. Ignored while one is outstanding:
 *  a double-tap must not queue two multi-second walks, and the first result
 *  repaints the list anyway. Call from the shell's poll, never the worker. */
void clock_themes_request_scan(void);

/** Reap a finished walk into the rows the option hooks answer from. Call from
 *  the LVGL task on every poll, whether or not the page is showing: a scan that
 *  finishes while the owner has rotated to the dial must still land, or the
 *  list would show a stale answer until they came back. */
void clock_themes_service(void);

/** True while a walk, an install or the default switch is outstanding, for the
 *  hint line. */
bool clock_themes_busy(void);

/** The status line the last job left — why the card did not answer, or what
 *  the last pick did. Empty when there is nothing to report. */
const char *clock_themes_status(void);

/** Press the row the shell's cursor is on. The shell owns the cursor, the clamp
 *  and the window; this module keeps no second idea of which row is highlighted,
 *  because two cursors for one row is how a page ends up staging the theme
 *  nobody is looking at. */
void clock_themes_pick(int idx);

/** Leave the theme page: the cursor goes back to the top so re-entering answers
 *  "what is on the card" rather than dropping the owner mid-list where they
 *  left it, which on a round screen with no scroll bar reads as a lost knob. */
void clock_themes_reset(void);

#ifdef __cplusplus
}
#endif

/* ---- the clock app's option answers ----------------------------------------
 *
 * Declared for the registry table only (main/app/orb_app.c). Nothing else may
 * call them: the shell's list primitives go through orb_app_opt_*(), which is
 * where NULL hooks and out-of-range rows are handled, and a caller that reached
 * these directly would be reading an app's state through the wrong window. */

#include "orb_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the theme page's state. Call before orb_app_init_all() reaches the
 *  clock's init, which is where the boot ladder puts it. */
esp_err_t clock_themes_init(void);

int          clock_opt_count(orb_app_t *app);
const char  *clock_opt_label(orb_app_t *app, int idx);
const char  *clock_opt_value(orb_app_t *app, int idx);
orb_opt_kind_t clock_opt_kind(orb_app_t *app, int idx);
void         clock_opt_inc(orb_app_t *app, int idx, int32_t steps);
void         clock_opt_sel(orb_app_t *app, int idx);
void         clock_opt_arm(orb_app_t *app, bool armed);

#ifdef __cplusplus
}
#endif
