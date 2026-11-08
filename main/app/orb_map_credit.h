/* The OS maps' on-glass credit (radar-plan §0 ruling 9 as revised 2026-11-07, OSMF's
 * attribution guidelines): the first time an app draws an OS map onto the glass after a
 * boot, "Map data: OpenStreetMap" stands at the foot of the glass for five seconds, then
 * goes, and the full credits and the licence's address stay on ABOUT. LVGL task only. */
#pragma once

#include <stdbool.h>
#include "orb_app.h"

/** An app just drew a map (orb_map_draw). Shows the credit once a boot, if that app is
 *  the one on the glass. */
void orb_map_credit_drawn(const orb_app_t *app);
/** The glass is changing apps: the credit, if standing, goes with the app it was for. */
void orb_map_credit_hide(void);
/** Whether it has been shown since the boot (for the bench). */
bool orb_map_credit_shown(void);
