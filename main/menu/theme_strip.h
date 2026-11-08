#pragma once
/**
 * theme_strip — an app's themes as a vertical carousel (app-platform-plan §61).
 *
 * The Themes row of an app's page opens it in place of the text list: the theme under
 * the cursor large in the centre with its name across it, the ones above and below
 * peeking in, and a Back card at the top. A turn moves one theme a detent and clamps at
 * the ends; a press installs the centre theme, or opens the app when the centre is the
 * theme it already wears, or goes back from the Back card; a hold goes back to the app's page.
 *
 * The pictures load lazily: a disc is a plain card with the name until its preview has
 * been read, nearest the cursor first, and only while the strip rests — the card holds
 * the display's DMA ring while it reads, so nothing is read while it moves. The centre
 * sharpens to a 300 px plane once the cursor stops. Everything is freed when it closes;
 * a pick reboots the orb anyway.
 *
 * Frames are composed straight into the tile ring (display_fx_frame) under
 * display_glass_hold, as the app carousel's are. LVGL task only.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct orb_app orb_app_t;

/** Take the glass and show `app`'s themes, the cursor on the one it wears. False (and
 *  nothing taken) when it cannot run: no themes listed, no memory, the glass busy. */
bool theme_strip_open(const orb_app_t *app);

/** True from theme_strip_open until it hands the glass back. */
bool theme_strip_active(void);

/** The router's poll while active: the knob's events and the poll's timestamp. */
void theme_strip_poll(int32_t delta, bool pressed, bool lpress, int64_t now_us);

/** Every poll, open or not: reaps a load that finished after the strip closed. */
void theme_strip_service(void);

/** Hand the glass back at once (a screen asked for over HTTP outranks it). */
void theme_strip_abort(void);
