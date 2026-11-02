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

/* ---- Open Location Code ------------------------------------------------------
 * The public spec (github.com/google/open-location-code): base 20 over
 * "23456789CFGHJMPQRVWX". Ten digits are five lat/lon pairs (20 deg down to
 * 1/8000 deg); each digit after that splits the cell 5 rows by 4 columns. '+'
 * sits after the eighth; '0' pads a code shorter than eight. Worked in integer
 * units of 1/(8000*3125*...) per step so the centre is exact before the one
 * final division. */
static const char OLC_ALPHA[] = "23456789CFGHJMPQRVWX";

static int olc_val(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    const char *f = c ? strchr(OLC_ALPHA, c) : NULL;
    return f ? (int)(f - OLC_ALPHA) : -1;
}

const char *orb_geo_olc_decode(const char *code, int32_t *lat_e6, int32_t *lon_e6)
{
    char d[16];
    int n = 0, plus = -1, pad = 0;
    for (const char *p = code; *p; p++) {
        if (*p == ' ') { if (plus >= 0) break; continue; }   /* "XXXX+XX City" stops here */
        if (*p == '+') {
            if (plus >= 0) return "more than one +";
            plus = n;
            continue;
        }
        if (*p == '0') {
            if (plus >= 0) return "padding after the +";
            pad++;
            continue;
        }
        if (olc_val(*p) < 0) return "not a plus-code character";
        if (pad) return "a digit after the 0 padding";
        if (n >= 15) return "too many digits";
        d[n++] = *p;
    }
    if (plus < 0) return "no + in the code";
    if (plus + pad < 8) return "short code: needs the full 8 before the +";
    if (plus + pad > 8) return "+ is not after the 8th character";
    if (pad && (pad % 2 || n != plus)) return "bad padding";
    if (n == 9) return "one digit after the + is not a code";
    if (n < 2) return "too few digits";
    if (olc_val(d[0]) > 8) return "first character is past the pole";
    if (olc_val(d[1]) > 17) return "second character is past 180";

    /* Pairs: lat/lon in units of 1/8000 deg (the 5th pair's resolution). */
    static const int64_t PAIR[5] = { 160000, 8000, 400, 20, 1 };   /* 20 deg .. 1/8000 deg */
    int64_t lat = 0, lon = 0;
    int i = 0;
    for (; i < n && i < 10; i += 2) {
        lat += olc_val(d[i]) * PAIR[i / 2];
        if (i + 1 < n) lon += olc_val(d[i + 1]) * PAIR[i / 2];
    }
    /* Cell size so far, same units; then the grid digits refine it. Every
     * step multiplies the units by 5 (lat) / 4 (lon) so the sums stay exact. */
    int64_t lat_cell = PAIR[(n < 10 ? n : 10) / 2 - 1], lon_cell = lat_cell;
    int64_t lat_den = 8000, lon_den = 8000;
    for (; i < n; i++) {
        const int v = olc_val(d[i]);
        lat *= 5; lat_den *= 5;
        lon *= 4; lon_den *= 4;
        lat += (v / 4) * lat_cell;
        lon += (v % 4) * lon_cell;
    }
    /* centre: origin + half a cell, then shift from the (-90, -180) origin */
    const double la = (double)(2 * lat + lat_cell) / (double)(2 * lat_den) - 90.0;
    const double lo = (double)(2 * lon + lon_cell) / (double)(2 * lon_den) - 180.0;
    *lat_e6 = (int32_t)llround((la > 90.0 ? 90.0 : la) * 1e6);
    *lon_e6 = (int32_t)llround(lo * 1e6);
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

int orb_geo_city_zone(int city)
{
    if (city < 0 || city >= ORB_GEO_N_CITIES) return -1;
    return ORB_GEO_CITIES[city].zone;
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
