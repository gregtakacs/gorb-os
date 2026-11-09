#pragma once
/**
 * orb_strip — the carousel (plan §54).
 *
 * A hold, or a turn on a face with no dial, zooms the glass down into the centre of
 * a strip of apps: each app is its theme's preview, large in the centre with its name
 * under it, small at the edges either side, and a gear for the system pages at the
 * end. A turn slides the strip one place (it wraps); a press, or 3 s without a turn,
 * zooms the centre up to the full glass and opens it.
 *
 * Every frame is composed straight into the tile ring (display_fx_frame) while the
 * glass is held (display_glass_hold), so LVGL draws nothing of it and the app under
 * the carousel keeps its screen until the carousel opens another. The previews are
 * read off the card once, after boot, into PSRAM (~108 KB an app): a card read costs
 * ~175 ms, far too slow for a turn. A store reboots the orb, so the cache never goes
 * stale inside a boot.
 *
 * LVGL task only, from input_router's poll, except orb_strip_init.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct orb_app orb_app_t;

/** Start the preview cache's one-shot worker. Call once, after the app registry and
 *  the theme pool are up. */
void orb_strip_init(void);

/** Take the glass and open the carousel on `from`, moved `step` places. False (and
 *  nothing taken) when the carousel cannot run: the cache not filled yet, no mirror,
 *  no memory. The router then opens the text list. */
bool orb_strip_open(const orb_app_t *from, int step);

/** True from orb_strip_open until the carousel hands the glass back. */
bool orb_strip_active(void);

/** The router's poll while active: the knob's events and the poll's timestamp. */
void orb_strip_poll(int32_t delta, bool pressed, bool lpress, int64_t now_us);

/** theme-cache-plan ruling 2: `app` is on the glass and its theme is filling from the card:
 *  take the glass and show its preview at 300 px with the fill's progress ring round it,
 *  then zoom into the app when the fill is over. A no-op while the carousel runs (an open
 *  from the carousel stops its own zoom at the face), or when no fill runs for it. */
void orb_strip_fill_face(const orb_app_t *app);

/** Hand the glass back at once, the app under the carousel as it was (a screen the
 *  router was asked for over HTTP outranks the carousel). */
void orb_strip_abort(void);
