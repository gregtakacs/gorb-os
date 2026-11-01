#pragma once
/* The flight tracker's model: the contact table, dead reckoning, projection,
 * label placement and the boost test (docs/flight-tracker-design.md §5-6).
 *
 * Pure C with no LVGL or IDF include, like orb_readsb.c, so
 * tools/host_flights_model_test.py compiles the shipped file and drives it.
 * flights_view.c draws what this decides; orb_flights.c asks it whether a
 * picture is worth a faster background poll. */
#include <stdbool.h>
#include <stdint.h>
#include "orb_readsb.h"

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

typedef struct { int x1, y1, x2, y2; } fm_rect_t;

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

/* A label of w x h px for a dot at (x, y): the first of right, left, above,
 * below (the side away from the centre first) whose corners stay inside
 * glass_r of (cx, cy) and that clears every placed rect by 2 px across and 1 px
 * down. False when no spot is free; the contact then goes unlabelled. */
bool fm_place_label(const fm_rect_t *placed, int np, int x, int y, int w, int h,
                    int cx, int cy, int glass_r, fm_rect_t *out);

/* Worth the background boost (§5): an emergency squawk, airborne within 5 nm,
 * or under 3,000 ft within 10 nm with its track within 60 deg of home. */
bool fm_interesting(const orb_ac_t *ac, float dist_km, float bearing_deg);
