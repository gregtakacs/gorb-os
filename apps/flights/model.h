#pragma once
/* The flight tracker's model: the contact table, dead reckoning, projection,
 * selection, the boost test and the route check (docs/flight-tracker-design.md
 * §5-6).
 *
 * Pure C with no LVGL or IDF include, like orb_readsb.c, so
 * tools/host_flights_model_test.py compiles the shipped file and drives it.
 * flights_view.c draws what this decides; orb_flights.c asks it whether a
 * picture is worth a faster background poll. */
#include <stdbool.h>
#include <stdint.h>
#include "readsb.h"

#define FM_MAX_TRK  32                 /* contacts kept between polls        */
#define FM_STALE_US (60LL * 1000000)   /* a missed contact dims, then drops   */
#define FM_DR_CAP_S 90.0f              /* dead reckoning stops extrapolating  */

typedef struct {
    orb_ac_t ac;
    float    e_km, n_km;     /* east/north of home at t_fix  */
    int64_t  t_fix_us;       /* when that position was true  */
    int64_t  t_seen_us;      /* the last poll that carried it */
    bool     live;           /* in the latest picture        */
    float    x, y, d_km;     /* this frame's projection      */
    bool     drawn;          /* inside the range this frame  */
} fm_trk_t;

typedef struct {
    fm_trk_t t[FM_MAX_TRK];
    int      n;
    int      order[FM_MAX_TRK];   /* drawn contacts, nearest first */
    int      norder;
} fm_t;

/* One poll's merge: begin, one upsert per aircraft in the picture, end. A
 * contact the picture did not carry stays, not live, until FM_STALE_US. */
void fm_merge_begin(fm_t *m);
void fm_upsert(fm_t *m, const orb_ac_t *ac, float dist_km, float bearing_deg, int64_t now_us);
void fm_merge_end(fm_t *m, int64_t now_us);

/* Advance every contact to now (dead reckoning along track at ground speed,
 * capped at FM_DR_CAP_S; ground traffic and contacts without a track stay
 * put), project onto a ring of radius ring_r px around (cx, cy) for range_km,
 * and sort the drawn ones nearest first into m->order. */
void fm_project(fm_t *m, float range_km, int64_t now_us, int cx, int cy, int ring_r);

/* Where the contact with this hex sits in m->order, or -1. */
int  fm_find(const fm_t *m, const char *hex);
/* The selection one step from hex in nearest-first order, wrapping at both
 * ends: from none (or a contact no longer drawn) a step forward takes the
 * nearest and a step back the farthest. "" only when nothing is drawn.
 * hex_out holds 8 bytes. */
void fm_sel_step(const fm_t *m, const char *hex, int step, char *hex_out);

/* The drawn contacts the view seats, at most `max` of them: the nearest, then stacked
 * lowest first so a higher aircraft draws over a lower one (flight-tracker-design §12.5):
 * the ground under everything, a contact with no altitude above it, then by feet, the
 * nearer first at a tie. Writes indices into m->t; returns how many. */
int  fm_stack(const fm_t *m, int max, int *idx);
/* Where a card that keeps clear stands, as its vertical offset from home: its box at home
 * is centred (cx, cy) with half sizes (hw, hh); mid is the glass's middle row. A contact
 * at (x, y) within `margin` of the box sends it to its mirror across the middle; it comes
 * home only once the contact is clear by twice the margin, and between the two it stays
 * where it was (`was`), so a contact on the edge never makes it flicker. */
int  fm_card_dy(int cx, int cy, int hw, int hh, int mid, int x, int y, int was, int margin);


/* Worth the background boost (§5): an emergency squawk, airborne within 5 nm,
 * or under 3,000 ft within 10 nm with its track within 60 deg of home. */
bool fm_interesting(const orb_ac_t *ac, float dist_km, float bearing_deg);

/* ---- route: where a flight is going, when adsbdb knows -------------------
 * adsbdb.com answers a callsign with the route that callsign is scheduled to
 * fly. The answer is a schedule, not a flight plan: a reused flight number
 * names last season's route (bench, 2026-11-01: Envoy 3776 over Fort Worth
 * came back Boston to Chicago), so a route is shown only when the aircraft is
 * plausibly on it. */
typedef struct {
    char  from[5], to[5];          /* IATA, or ICAO when the airport has none */
    float from_lat, from_lon, to_lat, to_lon;
} fm_route_t;

/* Worth asking about: an ICAO airline callsign, at most seven characters:
 * three letters, a digit, then up to three letters or digits ("AAL1393",
 * "SWA12A"). A
 * registration ("N570AM") has no route to look up, and adsbdb takes seconds
 * to say so. */
bool fm_route_callsign_ok(const char *cs);

/* adsbdb's /v0/callsign/<cs> answer, read from the OS's JSON events (D51): begin,
 * take every event, end. end() is false when it names no route (an unknown callsign,
 * a missing airport or position). */
typedef struct {
    int     fr_depth;             /* depth of "flightroute"'s own events; -1 outside it */
    int     end;                  /* 1 origin, 2 destination, 0 neither */
    bool    saw_fr;
    char    iata[2][5], icao[2][5];
    bool    iata_ok[2], icao_ok[2], has_lat[2], has_lon[2];
    float   lat[2], lon[2];
} fm_route_rd_t;
void fm_route_begin(fm_route_rd_t *r);
void fm_route_take(fm_route_rd_t *r, const orb_json_tok_t *t);
bool fm_route_end(const fm_route_rd_t *r, fm_route_t *out);

/* True when an aircraft at (lat, lon) is plausibly flying this route.
 * Where: within 100 km of either end, or within 150 km (or 15% of the
 * route's length, whichever is more) of the great circle between them and
 * not past either end. Which way, when the track is known (not NAN): near the
 * origin it is leaving (heading away, or climbing); near the destination it
 * is arriving (heading toward it, or descending, which a downwind leg is);
 * between them it is heading toward the destination. A flight number flown
 * the other way back (bench, 2026-11-01: "CLE > DFW" on a jet climbing east
 * out of DFW) fails the direction, not the position. baro_rate NAN = unknown. */
bool fm_route_plausible(const fm_route_t *r, double lat, double lon, float track, float baro_rate);
