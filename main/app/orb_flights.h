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
    uint32_t period_ms;
    uint8_t  source;               /* orb_flights_src_t */
    bool     keep_alive;
} orb_flights_cfg_t;

typedef struct {
    orb_ac_t ac;
    float    dist_km;
    float    bearing_deg;           /* from home, true */
} orb_flight_t;

typedef struct {
    uint32_t polls, ok, fail;
    uint32_t skip_net, skip_jobs, skip_card, skip_heap;
    uint32_t opens;                 /* TLS handshakes                        */
    int      last_status;
    int      last_err;
    uint32_t last_open_ms, last_total_ms, max_total_ms;
    uint32_t last_bytes, last_objects, last_kept;
    size_t   poll_int_free_min;     /* internal heap low-water across polls  */
    size_t   poll_int_largest_min;
    int64_t  last_ok_us;            /* esp_timer time of the last good poll  */
} orb_flights_stats_t;

esp_err_t orb_flights_init(void);
void      orb_flights_get_cfg(orb_flights_cfg_t *out);
esp_err_t orb_flights_set_cfg(const orb_flights_cfg_t *cfg);   /* starts/stops; kicks a poll */
void      orb_flights_stats(orb_flights_stats_t *out);
/* Copies the latest snapshot; returns the count and its age in ms via *age_ms. */
int       orb_flights_snapshot(orb_flight_t *out, int max, uint32_t *age_ms);
/* The bench's JSON: config, stats, jank deltas and the snapshot. */
int       orb_flights_json(char *buf, size_t cap);
