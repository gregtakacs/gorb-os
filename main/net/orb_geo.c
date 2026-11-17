/* Home-location arithmetic — contract in orb_geo.h. */
#include "orb_geo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool orb_geo_valid(int32_t lat_e6, int32_t lon_e6)
{
    return lat_e6 >= -90000000 && lat_e6 <= 90000000 &&
           lon_e6 >= -180000000 && lon_e6 <= 180000000;
}

/* One signed decimal of at most 6 places, as millionths. Integer arithmetic
 * only: "33.4484" must become exactly 33448400, which a float parse does not
 * promise. Returns the position after it, or NULL. */
static const char *dec_e6(const char *p, int32_t *out)
{
    bool neg = false;
    if (*p == '-' || *p == '+') neg = (*p++ == '-');
    int64_t whole = 0, frac = 0;
    int nd = 0, nf = 0;
    while (*p >= '0' && *p <= '9') {
        if (++nd > 3) return NULL;
        whole = whole * 10 + (*p++ - '0');
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (++nf > 6) return NULL;
            frac = frac * 10 + (*p++ - '0');
        }
    }
    if (!nd && !nf) return NULL;
    for (int k = nf; k < 6; k++) frac *= 10;
    const int64_t v = whole * 1000000 + frac;
    *out = (int32_t)(neg ? -v : v);
    return p;
}

const char *orb_geo_parse_coords(const char *s, int32_t *lat_e6, int32_t *lon_e6)
{
    int32_t la, lo;
    while (*s == ' ') s++;
    const char *p = dec_e6(s, &la);
    if (!p) return "latitude is not a number";
    bool sep = false;
    while (*p == ' ' || *p == ',') { sep = true; p++; }
    if (!sep) return "put a comma between the two";
    p = dec_e6(p, &lo);
    if (!p) return "longitude is not a number";
    while (*p == ' ') p++;
    if (*p) return "extra characters after longitude";
    if (la < -90000000 || la > 90000000) return "latitude is past +-90";
    if (lo < -180000000 || lo > 180000000) return "longitude is past +-180";
    *lat_e6 = la;
    *lon_e6 = lo;
    return NULL;
}

/* ---- the city table ---------------------------------------------------------- */

/* Single precision on purpose: the S3's FPU is single-only, and double trig
 * would link several KB of soft-float libm into a slot with little room. A
 * float haversine is good to metres at these scales; the answers are rounded
 * to kilometres. */
uint32_t orb_geo_dist_m(int32_t lat1_e6, int32_t lon1_e6, int32_t lat2_e6, int32_t lon2_e6)
{
    const float k = (float)(M_PI / 180e6);
    const float la1 = lat1_e6 * k, la2 = lat2_e6 * k;
    const float dla = (float)(lat2_e6 - lat1_e6) * k, dlo = (float)(lon2_e6 - lon1_e6) * k;
    const float h = sinf(dla / 2) * sinf(dla / 2) + cosf(la1) * cosf(la2) * sinf(dlo / 2) * sinf(dlo / 2);
    return (uint32_t)(6371000.0f * 2 * asinf(sqrtf(h > 1 ? 1 : h)) + 0.5f);
}

int orb_geo_nearest(int32_t lat_e6, int32_t lon_e6, uint32_t *dist_m)
{
    /* A flat-earth scan to pick the candidate (cheap, and right except across
     * the antimeridian, which the wrap below handles), then one great-circle
     * distance for the answer. */
    const float c = cosf((float)lat_e6 * (float)(M_PI / 180e6));
    const float la = lat_e6 / 1e4f, lo = lon_e6 / 1e4f;   /* 0.01 deg units */
    int best = -1;
    float bd = 0;
    for (int i = 0; i < ORB_GEO_N_CITIES; i++) {
        const float dy = ORB_GEO_CITIES[i].lat - la;
        float dx = ORB_GEO_CITIES[i].lon - lo;
        if (dx > 18000) dx -= 36000;
        if (dx < -18000) dx += 36000;
        const float cdx = dx * (c < 0.05f ? 0.05f : c);
        const float d = dy * dy + cdx * cdx;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best >= 0 && dist_m)
        *dist_m = orb_geo_dist_m(lat_e6, lon_e6, ORB_GEO_CITIES[best].lat * 10000,
                                 ORB_GEO_CITIES[best].lon * 10000);
    return best;
}

const char *orb_geo_city_name(int city)
{
    if (city < 0 || city >= ORB_GEO_N_CITIES) return "";
    return ORB_GEO_NAMES + ORB_GEO_CITIES[city].name;
}

/* ASCII case fold; bytes past 0x7f (an accented letter's UTF-8) compare as they are. */
static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

/* Does `name` have a word starting with `q`? "york" finds "New York"; "san j" finds
 * "San Jose". Words start at the name's start or after a space, hyphen or apostrophe. */
static bool word_prefix(const char *name, const char *q)
{
    for (const char *w = name; *w; w++) {
        if (w != name && w[-1] != ' ' && w[-1] != '-' && w[-1] != '\'') continue;
        const char *a = w, *b = q;
        while (*b && *a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}

int orb_geo_city_search(const char *q, int *out, int max)
{
    while (q && *q == ' ') q++;
    if (!q || !q[0] || max <= 0) return 0;
    int n = 0;
    /* two passes: names that START with the query first, then any word in them */
    for (int pass = 0; pass < 2 && n < max; pass++)
        for (int c = 0; c < ORB_GEO_N_CITIES && n < max; c++) {
            const char *nm = ORB_GEO_NAMES + ORB_GEO_CITIES[c].name;
            const char *a = nm, *b = q;
            while (*b && *a && fold((unsigned char)*a) == fold((unsigned char)*b)) { a++; b++; }
            const bool starts = !*b;
            if (pass == 0 ? starts : (!starts && word_prefix(nm, q))) out[n++] = c;
        }
    return n;
}

void orb_geo_city_ll(int city, int32_t *lat_e6, int32_t *lon_e6)
{
    if (city < 0 || city >= ORB_GEO_N_CITIES) { *lat_e6 = *lon_e6 = 0; return; }
    *lat_e6 = (int32_t)ORB_GEO_CITIES[city].lat * 10000;
    *lon_e6 = (int32_t)ORB_GEO_CITIES[city].lon * 10000;
}

int orb_geo_city_zone(int city)
{
    if (city < 0 || city >= ORB_GEO_N_CITIES) return -1;
    return ORB_GEO_CITIES[city].zone;
}

void orb_geo_city_cc(int city, char out[3])
{
    out[0] = out[1] = out[2] = 0;
    if (city < 0 || city >= ORB_GEO_N_CITIES) return;
    out[0] = ORB_GEO_CITIES[city].cc[0];
    out[1] = ORB_GEO_CITIES[city].cc[1];
}

const char *orb_geo_zone_iana(int zone)
{
    if (zone < 0 || zone >= ORB_GEO_N_ZONES) return "";
    return ORB_GEO_ZONES[zone].iana;
}

const char *orb_geo_zone_posix(int zone)
{
    if (zone < 0 || zone >= ORB_GEO_N_ZONES) return "";
    return ORB_GEO_RULES[ORB_GEO_ZONES[zone].rule & 0x7f];
}

bool orb_geo_zone_exact(int zone)
{
    return zone >= 0 && zone < ORB_GEO_N_ZONES && !(ORB_GEO_ZONES[zone].rule & 0x80);
}

int orb_geo_zone_find(const char *iana)
{
    if (!iana) return -1;
    for (int z = 0; z < ORB_GEO_N_ZONES; z++)
        if (!strcmp(ORB_GEO_ZONES[z].iana, iana)) return z;
    return -1;
}

void orb_geo_place(int32_t lat_e6, int32_t lon_e6, char *out, size_t outsz)
{
    uint32_t d = 0;
    const int c = orb_geo_nearest(lat_e6, lon_e6, &d);
    if (c < 0) { snprintf(out, outsz, "home"); return; }
    const char *name = orb_geo_city_name(c);
    if (d < 15000) { snprintf(out, outsz, "%s", name); return; }
    if (d < 80000) { snprintf(out, outsz, "near %s", name); return; }
    /* Bearing FROM the city TO home, so "120 km NW of Denver" reads right. */
    const float k = (float)(M_PI / 180e6);
    const float la1 = ORB_GEO_CITIES[c].lat * 1e4f * k, la2 = lat_e6 * k;
    const float dlo = (lon_e6 - ORB_GEO_CITIES[c].lon * 1e4f) * k;
    float b = atan2f(sinf(dlo) * cosf(la2), cosf(la1) * sinf(la2) - sinf(la1) * cosf(la2) * cosf(dlo));
    b = b * 180.0f / (float)M_PI;
    if (b < 0) b += 360.0f;
    static const char *const PT[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    snprintf(out, outsz, "%u km %s of %s", (unsigned)((d + 500) / 1000),
             PT[(int)((b + 22.5f) / 45.0f) & 7], name);
}

void orb_geo_fmt(int32_t v_e6, char *out, size_t outsz)
{
    const uint32_t a = (uint32_t)(v_e6 < 0 ? -(int64_t)v_e6 : v_e6);
    snprintf(out, outsz, "%s%u.%06u", v_e6 < 0 ? "-" : "", (unsigned)(a / 1000000),
             (unsigned)(a % 1000000));
}
