#pragma once
/* A streaming reader for the readsb aircraft JSON that adsb.lol and adsb.fi
 * answer with (docs/flight-tracker-design.md §3).
 *
 * Fed in whatever pieces the socket hands over, it never holds the body: one
 * aircraft's worth of fields at a time, emitted to a callback when that
 * object closes. The body is 10-70 KB depending on radius and traffic, and the
 * point of reading it this way is that its size costs nothing but time.
 *
 * Pure C with no IDF include, so tools/host_readsb_test.py compiles it as is. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#define ORB_READSB_DEPTH 8

typedef struct {
    orb_readsb_emit_fn emit;
    void              *ctx;
    /* container stack: '{' or '[' per level */
    char     stack[ORB_READSB_DEPTH];
    int      depth;          /* may exceed ORB_READSB_DEPTH; deeper levels are skipped */
    int      list_depth;     /* depth of the aircraft array, 0 until "ac"/"aircraft" opens */
    /* lexer */
    uint8_t  mode;           /* see orb_readsb.c */
    bool     esc;
    bool     want_key;       /* inside an object, the next string is a key */
    char     key[16];
    uint8_t  key_len;
    bool     key_trunc;
    char     tok[24];        /* scalar text: string body or a bare literal */
    uint8_t  tok_len;
    bool     tok_str;
    /* the aircraft being read */
    orb_ac_t ac;
    bool     have_lat, have_lon, have_track;
    /* totals */
    uint32_t objects;        /* aircraft objects closed                     */
    uint32_t emitted;        /* of those, the ones with hex+lat+lon         */
    bool     error;          /* malformed input; feed() stops consuming      */
} orb_readsb_t;

void orb_readsb_init(orb_readsb_t *r, orb_readsb_emit_fn emit, void *ctx);
/* Returns false once the input is known malformed (r->error). */
bool orb_readsb_feed(orb_readsb_t *r, const char *buf, size_t len);
/* True when the whole document closed cleanly and the aircraft array was seen. */
bool orb_readsb_done(const orb_readsb_t *r);
