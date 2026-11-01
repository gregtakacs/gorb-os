#pragma once
/* The flight feed service (docs/flight-tracker-design.md §5): polls a readsb
 * provider over HTTPS in the background, whatever app is on the glass, and
 * publishes the nearest aircraft as a snapshot the tracker's view copies out.
 *
 * Runs as its own task below every display-facing priority, with its stack and
 * its tables in PSRAM, and skips a poll rather than compete: a card session,
 * a hub transfer or a thin internal pool each turn a poll into a counted skip.
 * A flight picture ten seconds old is invisible; a held beat on the dial is not. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "orb_readsb.h"

#define ORB_FLIGHTS_MAX 24          /* nearest kept per poll: a 466 px scope's worth */

typedef enum { ORB_FLIGHTS_ADSB_LOL = 0, ORB_FLIGHTS_ADSB_FI = 1 } orb_flights_src_t;

typedef struct {
    bool     enabled;
    double   lat, lon;
    uint16_t radius_nm;
    uint32_t period_ms;            /* while the tracker is on the glass          */
    uint32_t bg_period_ms;         /* while it is not; 0 = no background polling */
    bool     visible;              /* the tracker is the app on the glass        */
    bool     boost;                /* background rises to ORB_FLIGHTS_BOOST_MS   */
    bool     hide_ground;          /* drop on-ground aircraft at the parse       */
    uint8_t  source;               /* orb_flights_src_t */
    bool     keep_alive;
} orb_flights_cfg_t;

/* The background rate while the last picture held something worth watching
 * (docs/flight-tracker-design.md §5): within 5 nm, under 3,000 ft within 10 nm
 * and closing on home, or an emergency squawk. */
#define ORB_FLIGHTS_BOOST_MS 20000

typedef struct {
    orb_ac_t ac;
    float    dist_km;
    float    bearing_deg;           /* from home, true */
} orb_flight_t;

typedef struct {
    uint32_t polls, ok, fail;
    uint32_t skip_net, skip_jobs, skip_card, skip_heap;
    uint32_t skip_rest;             /* both providers resting after a 4xx    */
    uint32_t refused;               /* 4xx answers: each rests its provider  */
    uint32_t failovers;             /* polls sent to the other provider      */
    uint8_t  last_src;              /* who answered the last poll            */
    uint32_t opens;                 /* TLS handshakes                        */
    int      last_status;
    int      last_err;
    uint32_t last_open_ms, last_total_ms, max_total_ms;
    uint32_t last_bytes, last_objects, last_kept;
    size_t   poll_int_free_min;     /* internal heap low-water across polls  */
    size_t   poll_int_largest_min;
    int64_t  last_ok_us;            /* esp_timer time of the last good poll  */
    uint32_t seq;                   /* bumps with every published picture    */
    bool     interesting;           /* the last picture held a boost reason  */
} orb_flights_stats_t;

esp_err_t orb_flights_init(void);
void      orb_flights_get_cfg(orb_flights_cfg_t *out);
esp_err_t orb_flights_set_cfg(const orb_flights_cfg_t *cfg);   /* starts/stops; kicks a poll */
void      orb_flights_stats(orb_flights_stats_t *out);
/* Copies the latest snapshot; returns the count, its age in ms via *age_ms and
 * its sequence number via *seq (either may be NULL). */
int       orb_flights_snapshot(orb_flight_t *out, int max, uint32_t *age_ms, uint32_t *seq);
/* Bench only (the debug door): provider 0 refuses, to drive the failover. */
void      orb_flights_bench_refuse0(bool on);
/* The bench's JSON: config, stats, jank deltas and the snapshot. */
int       orb_flights_json(char *buf, size_t cap);
