#pragma once
/* The orb's three places beside home (radar-plan.md §2.1, owner ruling 2026-11-07):
 * one OS setting every app that wants places reads, so Weather, the radar and any later
 * app agree on them without knowing of each other.
 *
 * One door writes them, POST /api/v1/places, which the hub's Places card on the Orb tab
 * drives; the dial has no location UI (owner ruling 2026-11-01). Apps read them through
 * the broker's `place` entry behind CAP_LOCATION, as they read home. A slot keeps its
 * number when another is cleared, so "place 2" stays place 2. */
#include <stdbool.h>
#include <stdint.h>
#include "orb_geo.h"

#define ORB_PLACES_N 3

typedef struct {
    bool    set;
    int32_t lat_e6, lon_e6;
    char    name[ORB_GEO_PLACE_MAX];   /* "Lisbon", "near Austin" */
} orb_place_rec_t;

/* ---- pure (lifted by tools/host_places_test.py) ---------------------------------- */

/* NULL when slot i may hold this place, else the reason. name NULL or "" is allowed
 * (the OS names it from the city list). */
const char *orb_places_check(int i, int32_t lat_e6, int32_t lon_e6, const char *name);

/* ---- the setting ------------------------------------------------------------------- */

/* Load the stored places. Call once at boot, after orb_home_init(). */
void orb_places_init(void);

/* A copy of slot i; false (and out->set false) for an empty or out-of-range slot. */
bool orb_places_get(int i, orb_place_rec_t *out);

/* Bumped on every successful set or clear of any slot. */
uint32_t orb_places_gen(void);

/* Validate, persist, publish. NULL on success, else the reason. Blocks on NVS: never
 * on the LVGL task. */
const char *orb_places_set(int i, int32_t lat_e6, int32_t lon_e6, const char *name);

/* Empty slot i. NULL on success. */
const char *orb_places_clear(int i);
