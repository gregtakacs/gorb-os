#pragma once
/* The orb's home location: one OS setting that drives the time zone and every
 * app that needs a position (flight-tracker-design §6.4, owner ruling
 * 2026-11-01).
 *
 * One door writes it, through orb_home_set(): POST /api/v1/home, which the
 * hub's Orb tab drives (a city, the browser's location, a map pin,
 * coordinates or a plus code). The dial had a HOME page until 2026-11-01; a
 * character-per-press editor was no way to say where you live.
 * Apps read it through the broker's `home` entry behind CAP_LOCATION; OS code
 * reads it here.
 *
 * Setting a home sets the zone with it: the caller's zone when it sends one,
 * else the nearest big city's (the hub sends none). The zone can still be
 * written past the home (POST /api/v1/config, the orb's own /timezone page),
 * so the record remembers which zone string it wrote, and "the zone follows
 * home" is a comparison, not a flag that can drift. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "orb_geo.h"

#define ORB_HOME_ZONE_MAX 40      /* an IANA name, NUL included */
#define ORB_HOME_TZ_MAX   65      /* NET_TZ_MAX + 1             */

typedef struct {
    bool     set;
    int32_t  lat_e6, lon_e6;
    char     place[ORB_GEO_PLACE_MAX];   /* "Phoenix", "near Phoenix", "120 km N of Denver" */
    char     zone[ORB_HOME_ZONE_MAX];    /* IANA name of the zone it set, "" if none/unknown */
    char     tz[ORB_HOME_TZ_MAX];        /* the exact tz string it wrote, "" = left the zone alone */
} orb_home_t;

/* What a position would become, computed and not stored: the dial's confirm
 * page and GET /api/v1/home?lat=&lon= both show this before anything is set. */
typedef struct {
    int32_t     lat_e6, lon_e6;   /* the point it was asked about           */
    char        place[ORB_GEO_PLACE_MAX];
    const char *zone;             /* IANA, from the nearest city            */
    const char *tz;               /* what would be stored: the zone table's
                                   * key when it has the zone, else the rule */
    bool        exact;            /* false: the rule only approximates it   */
    uint32_t    city_m;           /* distance to that city                  */
} orb_home_prop_t;

/* Load the stored record. Call once after orb_net_start() (the zone it may
 * compare against is loaded there). */
void orb_home_init(void);

/* A copy of the record; false (and out->set false) when no home is set. */
bool orb_home_get(orb_home_t *out);

/* Bumped on every successful set or clear, so a reader polls a word and
 * copies the record only when it moved. */
uint32_t orb_home_gen(void);

void orb_home_propose(int32_t lat_e6, int32_t lon_e6, orb_home_prop_t *out);

/* Validate, set the zone, persist, publish — the net_tz_set() order: nothing
 * the orb would refuse reaches flash, and a refused zone leaves the old home
 * standing. Blocks on NVS: never on the LVGL task.
 *   place: NULL or "" = name it from the city list.
 *   tz:    NULL = the nearest city's zone; "keep" = leave the zone alone;
 *          else any value /api/v1/config accepts (a table key or a rule).
 *   zone:  the IANA name the caller's tz stands for, for display (the hub
 *          sends the browser's); NULL = work it out where possible.
 * NULL on success, else the reason. */
const char *orb_home_set(int32_t lat_e6, int32_t lon_e6, const char *place,
                         const char *tz, const char *zone);

/* Forget the home. The zone stays as it is. NULL on success. */
const char *orb_home_clear(void);

/* True when a home is set and the zone in force is the one it wrote. */
bool orb_home_zone_follows(void);

/* The zone in force, named for a person: the home's IANA name while the zone
 * follows home, else the zone table's city label or the raw rule. */
void orb_home_zone_label(char *out, size_t outsz);
