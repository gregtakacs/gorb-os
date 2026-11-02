#pragma once
/* A streaming reader for the readsb aircraft JSON that adsb.lol and adsb.fi
 * answer with (docs/flight-tracker-design.md §3).
 *
 * D46: the OS tokenizes the body (orb_json, natively, over the bytes it holds); this
 * file is only the schema — which keys are an aircraft's and what they mean — fed one
 * token at a time, one aircraft's fields kept, emitted when that object closes.
 *
 * Pure C: the app builds it against apps/sdk/orb.h, and tools/host_readsb_test.py
 * builds it against main/app/orb_json.h, whose token is the same type. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __wasm__
#include "orb.h"          /* orb_json_tok_t, ORB_JSON_* */
#else
#include "orb_json.h"
#endif

typedef struct {
    char     hex[8];        /* ICAO 24-bit id, "~" prefix kept (a TIS-B track) */
    char     flight[9];     /* callsign, trailing spaces trimmed; "" if absent */
    char     type[5];       /* ICAO type designator ("B738"); "" if absent     */
    double   lat, lon;
    int32_t  alt_ft;        /* barometric; 0 when on_ground                     */
    bool     on_ground;     /* alt_baro was the string "ground"                 */
    bool     has_alt;
    float    track;         /* deg true; NAN when neither track nor true_heading */
    float    gs;            /* kt; NAN when absent                              */
    float    baro_rate;     /* ft/min; NAN when absent                          */
    int16_t  squawk;        /* -1 when absent                                   */
    float    seen_pos;      /* s since the position was heard; 0 when absent    */
    bool     military;      /* dbFlags bit 0                                    */
} orb_ac_t;

/* Called once per aircraft object that carried hex, lat and lon. */
typedef void (*orb_readsb_emit_fn)(const orb_ac_t *ac, void *ctx);

typedef struct {
    orb_readsb_emit_fn emit;
    void              *ctx;
    int      list_depth;     /* depth of the aircraft array's own events, -1 until seen */
    bool     in_ac;          /* inside one aircraft object */
    bool     closed;         /* the document's outermost container closed */
    orb_ac_t ac;
    bool     have_lat, have_lon, have_track;
    uint32_t objects;        /* aircraft objects closed                     */
    uint32_t emitted;        /* of those, the ones with hex+lat+lon         */
    bool     error;          /* malformed input; tokens stop being taken     */
} orb_readsb_t;

void orb_readsb_init(orb_readsb_t *r, orb_readsb_emit_fn emit, void *ctx);
/* One event from the OS's JSON tokenizer (D46). Returns false once the input is known
 * malformed (r->error) — a tokenizer error included. */
bool orb_readsb_token(orb_readsb_t *r, const orb_json_tok_t *t);
/* True when the whole document closed cleanly and the aircraft array was seen. */
bool orb_readsb_done(const orb_readsb_t *r);
