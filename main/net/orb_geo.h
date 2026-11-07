#pragma once
/* Home-location arithmetic (flight-tracker-design §6.4): coordinates and plus
 * codes in, the nearest big city and its zone out.
 *
 * Pure C with no IDF include, like orb_readsb.c, so tools/host_geo_test.py
 * compiles the shipped file and the generated table and drives them. orb_home.c
 * owns the stored record; this file only computes.
 *
 * Positions are int32 millionths of a degree (1e-6, ~0.1 m) everywhere: no
 * float crosses an API, so a stored home round-trips exactly. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_GEO_NAME_MAX  24      /* a city name, NUL included          */
#define ORB_GEO_PLACE_MAX 40      /* "1234 km NW of <name>", NUL included */

/* The generated table (main/net/orb_cities.c, tools/make_cities.py). */
typedef struct {
    int16_t  lat, lon;            /* 0.01 degree                         */
    uint16_t name;                /* offset into ORB_GEO_NAMES            */
    uint8_t  zone;                /* index into ORB_GEO_ZONES             */
    char     cc[2];               /* ISO 3166 country, "US" (unterminated) */
} orb_geo_city_t;

typedef struct {
    const char *iana;
    uint8_t     rule;             /* ORB_GEO_RULES index; bit 7 = approximate */
} orb_geo_zone_t;

extern const char *const    ORB_GEO_RULES[];
extern const orb_geo_zone_t ORB_GEO_ZONES[];
extern const int            ORB_GEO_N_ZONES;
extern const char           ORB_GEO_NAMES[];
extern const orb_geo_city_t ORB_GEO_CITIES[];
extern const int            ORB_GEO_N_CITIES;

/* In range: lat within +-90, lon within +-180 (both inclusive). */
bool orb_geo_valid(int32_t lat_e6, int32_t lon_e6);

/* "33.4484, -112.074", "33.4484 -112.074" or "33.4484,-112.074": two signed
 * decimals of at most 6 places, comma and/or spaces between. NULL on success,
 * else the reason, worded for the dial. */
const char *orb_geo_parse_coords(const char *s, int32_t *lat_e6, int32_t *lon_e6);

/* A full Open Location Code ("8M48FWX3+QQ", "85GCQ2XC+2V", padded "7FG49Q00+")
 * to the centre of its cell. Case-insensitive. Short codes ("FWX3+QQ Phoenix")
 * need a reference point the orb does not have and are refused with that
 * reason. NULL on success. */
const char *orb_geo_olc_decode(const char *code, int32_t *lat_e6, int32_t *lon_e6);

/* Great-circle distance in metres. */
uint32_t orb_geo_dist_m(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6);

/* Index of the nearest table city, or -1 when the table is empty; *dist_m
 * (optional) gets its distance. */
int orb_geo_nearest(int32_t lat_e6, int32_t lon_e6, uint32_t *dist_m);

const char *orb_geo_city_name(int city);
int         orb_geo_city_zone(int city);           /* -1 for a bad index */
/* The city's ISO country into out ("US"); "" for a bad index. */
void        orb_geo_city_cc(int city, char out[3]);
const char *orb_geo_zone_iana(int zone);           /* "" for a bad index */
const char *orb_geo_zone_posix(int zone);          /* "" for a bad index */
bool        orb_geo_zone_exact(int zone);
int         orb_geo_zone_find(const char *iana);   /* -1 when absent     */

/* What the dial calls a home: the city's own name within 15 km, "near <city>"
 * within 80 km, else "<n> km <compass> of <city>". */
void orb_geo_place(int32_t lat_e6, int32_t lon_e6, char *out, size_t outsz);

/* "33.448400, -112.074000" style, 6 places, for labels and JSON. */
void orb_geo_fmt(int32_t v_e6, char *out, size_t outsz);
