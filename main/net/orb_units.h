#pragma once
/* The orb's units: one OS setting every app reads, never an app's own
 * (weather-plan §0 ruling 3, owner 2026-11-06). Temperature, speed and
 * distance, set once for the orb; and since OS v1.2.16 the time format, 24- or
 * 12-hour (owner, 2026-11-06: a clock's format is the orb's, not an app's).
 *
 * Two doors write it, both through orb_units_set(): the dial's UNITS page and
 * POST /api/v1/units, which the hub's Orb tab drives. Apps read it through the
 * free `units` and `units_gen` imports (apps/sdk/orb.h); OS code reads it here.
 * A unit is not sensitive, so no cap gates it.
 *
 * The numbering is the app ABI: orb.h's ORB_UNIT_* carry the same values, and a
 * new choice is an append. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { ORB_UNITS_TEMP = 0, ORB_UNITS_SPEED, ORB_UNITS_DIST, ORB_UNITS_TIME, ORB_UNITS_KINDS };

enum { ORB_UNIT_C = 0, ORB_UNIT_F = 1, ORB_UNIT_TEMP_N };
enum { ORB_UNIT_KMH = 0, ORB_UNIT_MPH = 1, ORB_UNIT_KN = 2, ORB_UNIT_SPEED_N };
enum { ORB_UNIT_KM = 0, ORB_UNIT_MI = 1, ORB_UNIT_NM = 2, ORB_UNIT_DIST_N };
enum { ORB_UNIT_24H = 0, ORB_UNIT_12H = 1, ORB_UNIT_TIME_N };

typedef struct {
    uint8_t temp;     /* ORB_UNIT_C, ORB_UNIT_F */
    uint8_t speed;    /* ORB_UNIT_KMH, ORB_UNIT_MPH, ORB_UNIT_KN */
    uint8_t dist;     /* ORB_UNIT_KM, ORB_UNIT_MI, ORB_UNIT_NM */
    uint8_t time;     /* ORB_UNIT_24H, ORB_UNIT_12H */
} orb_units_t;

/* Load the stored choice; metric when nothing is stored or it is unreadable.
 * Call once on the boot ladder after `storage` is initialised and before the
 * apps build. */
void orb_units_init(void);

/* A copy of the choice in force. Never fails: before init it is metric. */
void orb_units_get(orb_units_t *out);

/* Starts at 1 and is bumped on every change, so a reader polls one word and
 * re-reads only when it moved (the orb_home_gen rule). */
uint32_t orb_units_gen(void);

/* Validate, persist, publish. NULL on success, else the reason; a refused
 * choice leaves the old one standing. Blocks on NVS: never on the LVGL task. */
const char *orb_units_set(const orb_units_t *u);

/* Pure, for the dial, the route and tools/host_units_test.py. */
bool orb_units_valid(const orb_units_t *u);
int  orb_units_count(int kind);                  /* choices for a kind, 0 for none */
const char *orb_units_key(int kind, int v);      /* "c", "kmh", "nm"; NULL out of range */
const char *orb_units_label(int kind, int v);    /* "°C", "km/h", "nm" for the dial */
int  orb_units_parse(int kind, const char *key); /* the value a key names, -1 for none */
