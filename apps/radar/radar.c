/* radar.c — the radar app's model: see radar.h. */
#include "radar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void rd_list_begin(rd_list_t *l)
{
    memset(l, 0, sizeof *l);
}

/* weather-maps.json: {"version", "generated", "host", "radar": {"past": [{"time",
 * "path"}...], "nowcast": [...]}, "satellite": ...}. Only radar.past and radar.nowcast
 * are read; anything else is walked past. */
bool rd_list_token(rd_list_t *l, const orb_json_tok_t *t)
{
    if (l->error) return false;
    if (!l->started) {
        l->started = true;
        if (t->type != ORB_JSON_OBJ || t->depth != 0) { l->error = true; return false; }
        return true;
    }
    switch (t->type) {
    case ORB_JSON_END:
        return false;
    case ORB_JSON_ERROR:
        l->error = true;
        return false;
    case ORB_JSON_OBJ:
        if (t->depth == 1 && !strcmp(t->key, "radar")) l->in_radar = true;
        if (t->depth == 3 && l->in) memset(&l->cur, 0, sizeof l->cur);
        return true;
    case ORB_JSON_OBJ_END:
        if (t->depth == 1 && l->in_radar) l->in_radar = false;
        if (t->depth == 3 && l->in && l->cur.t > 0 && l->cur.path[0]) {
            if (l->in == 1 && l->n_past < RD_PAST) l->past[l->n_past++] = l->cur;
            else if (l->in == 2 && l->n_now < RD_NOWCAST) l->now[l->n_now++] = l->cur;
        }
        return true;
    case ORB_JSON_ARR:
        if (t->depth == 2 && l->in_radar)
            l->in = !strcmp(t->key, "past") ? 1 : !strcmp(t->key, "nowcast") ? 2 : 0;
        return true;
    case ORB_JSON_ARR_END:
        if (t->depth == 2) l->in = 0;
        return true;
    case ORB_JSON_NUM:
        if (t->depth == 4 && l->in && !strcmp(t->key, "time")) l->cur.t = (int64_t)t->num;
        return true;
    case ORB_JSON_STR:
        if (t->depth == 4 && l->in && !strcmp(t->key, "path")) {
            if (t->str_trunc || strlen(t->str) >= RD_PATH_MAX || t->str[0] != '/') { l->error = true; return false; }
            memcpy(l->cur.path, t->str, strlen(t->str) + 1);
        }
        return true;
    default:
        return true;
    }
}

bool rd_list_ok(const rd_list_t *l)
{
    if (l->error || l->n_past < 1) return false;
    for (int i = 1; i < l->n_past; i++) if (l->past[i].t <= l->past[i - 1].t) return false;
    for (int i = 0; i < l->n_now; i++)
        if (l->now[i].t <= (i ? l->now[i - 1].t : l->past[l->n_past - 1].t)) return false;
    return true;
}

int32_t rd_e4(int32_t e6)
{
    const int32_t v = e6 >= 0 ? e6 + 50 : e6 - 50;
    return v / 100;                          /* C truncates: half away from zero */
}

static int fmt_e4(char *b, size_t cap, int32_t e6)
{
    const int32_t v = rd_e4(e6);
    const int32_t a = v < 0 ? -v : v;
    return snprintf(b, cap, "%s%d.%04d", v < 0 ? "-" : "", (int)(a / 10000), (int)(a % 10000));
}

size_t rd_frame_url(char *buf, size_t cap, const char *source, const char *path, int z,
                    int32_t lat_e6, int32_t lon_e6, bool smooth)
{
    char la[16], lo[16];
    fmt_e4(la, sizeof la, lat_e6);
    fmt_e4(lo, sizeof lo, lon_e6);
    const int k = snprintf(buf, cap, "%s%s/512/%d/%s/%s/255/%d_0.png", source, path, z, la, lo, smooth ? 1 : 0);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

size_t rd_list_url(char *buf, size_t cap, const char *source)
{
    const int k = snprintf(buf, cap, "%s/public/weather-maps.json", source);
    return k > 0 && (size_t)k < cap ? (size_t)k : 0;
}

/* The natural log, for the Mercator y: the guest's libm has none (apps/sdk/libc/math.h).
 * x = m * 2^e with m in [1, 2), and ln m = 2 atanh((m - 1) / (m + 1)), whose series is
 * done to the last bit by its 25th term at t <= 1/3. */
double rd_ln(double x)
{
    union { double d; uint64_t u; } b = { .d = x };
    if (!(x > 0)) return -1e300;
    const int e = (int)((b.u >> 52) & 0x7FF) - 1023;
    b.u = (b.u & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;    /* m in [1, 2) */
    const double t = (b.d - 1) / (b.d + 1), t2 = t * t;
    double term = t, sum = 0;
    for (int k = 1; k < 60; k += 2) {
        sum += term / k;
        term *= t2;
    }
    return 2 * sum + e * 0.69314718055994530942;
}

static double pow2(int z) { return (double)(1u << z); }

/* Web Mercator, in pixels of a world 512 * 2^z wide. */
static void world(int z, int32_t lat_e6, int32_t lon_e6, double *px, double *py)
{
    const double n = (double)RD_IMG * pow2(z);
    double lat = lat_e6 / 1e6;
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    const double s = sin(lat * M_PI / 180.0);
    *px = (lon_e6 / 1e6 + 180.0) / 360.0 * n;
    *py = (0.5 - rd_ln((1 + s) / (1 - s)) / (4 * M_PI)) * n;
}

/* Python's round(): ties to even. LibreWXR's origin is Python's. */
double rd_round_even(double v)
{
    const double f = floor(v), d = v - f;
    if (d > 0.5) return f + 1;
    if (d < 0.5) return f;
    return fmod(f, 2.0) == 0 ? f : f + 1;
}

void rd_window(int z, int32_t img_lat_e6, int32_t img_lon_e6, int32_t map_lat_e6,
               int32_t map_lon_e6, int32_t *sx, int32_t *sy)
{
    double ix, iy, mx, my;
    world(z, rd_e4(img_lat_e6) * 100, rd_e4(img_lon_e6) * 100, &ix, &iy);
    world(z, map_lat_e6, map_lon_e6, &mx, &my);
    const double n = (double)RD_IMG * pow2(z);
    double ox = rd_round_even(ix - RD_IMG / 2), oy = rd_round_even(iy - RD_IMG / 2);
    if (oy < 0) oy = 0;
    if (oy > n - RD_IMG) oy = n - RD_IMG;
    /* the image is not wrapped here: a place within 256 px of the date line at z5 is
     * 2,800 km wide of it, and the server's wrap would put the window across it anyway */
    *sx = (int32_t)floor(mx - ox - RD_GLASS / 2.0 + 0.5);
    *sy = (int32_t)floor(my - oy - RD_GLASS / 2.0 + 0.5);
}

/* The place's point in the image, in image pixels from its corner. */
static void place_in_image(int z, int32_t img_lat_e6, int32_t img_lon_e6, int32_t map_lat_e6,
                           int32_t map_lon_e6, double *ux, double *uy)
{
    double ix, iy, mx, my;
    world(z, rd_e4(img_lat_e6) * 100, rd_e4(img_lon_e6) * 100, &ix, &iy);
    world(z, map_lat_e6, map_lon_e6, &mx, &my);
    const double n = (double)RD_IMG * pow2(z);
    double ox = rd_round_even(ix - RD_IMG / 2), oy = rd_round_even(iy - RD_IMG / 2);
    if (oy < 0) oy = 0;
    if (oy > n - RD_IMG) oy = n - RD_IMG;
    *ux = mx - ox;
    *uy = my - oy;
}

void rd_window_k(int z, int k, int32_t img_lat_e6, int32_t img_lon_e6, int32_t map_lat_e6,
                 int32_t map_lon_e6, int32_t *sx, int32_t *sy, int32_t *dx, int32_t *dy)
{
    double ux, uy;
    place_in_image(z, img_lat_e6, img_lon_e6, map_lat_e6, map_lon_e6, &ux, &uy);
    /* glass x <-> image sx + (x - dx) / k, and the glass's centre (233) <-> the place */
    *sx = (int32_t)floor(ux - (RD_GLASS / 2.0) / k) - 1;
    *sy = (int32_t)floor(uy - (RD_GLASS / 2.0) / k) - 1;
    *dx = (int32_t)floor(k * (*sx - ux) + RD_GLASS / 2.0 + 0.5);
    *dy = (int32_t)floor(k * (*sy - uy) + RD_GLASS / 2.0 + 0.5);
}

int32_t rd_radius_m(int step, int32_t lat_e6)
{
    const double c = cos(lat_e6 / 1e6 * M_PI / 180.0);
    return (int32_t)floor(156543.03392 * c / pow2(rd_map_z(step)) * (RD_GLASS / 2) + 0.5);
}

void rd_time_text(char *buf, size_t cap, int minutes)
{
    if (!minutes) snprintf(buf, cap, "Now");
    else snprintf(buf, cap, "%s%d min", minutes < 0 ? "-" : "+", minutes < 0 ? -minutes : minutes);
}

void rd_range_text(char *buf, size_t cap, int32_t radius_m, int unit)
{
    const double km = radius_m / 1000.0;
    const double v = unit == 1 ? km / 1.609344 : unit == 2 ? km / 1.852 : km;
    snprintf(buf, cap, "%d %s", (int)floor(v + 0.5), unit == 1 ? "mi" : unit == 2 ? "nm" : "km");
}

/* Scope's look, byte for byte what radar-bake.js's valuesBytes(preset('scope')) writes
 * (tools/host_radar_test.py compares them), so a radar without a theme's values draws as
 * the shelf's starter does. */
uint16_t rd_roles_at(const rd_values_t *v, int step)
{
    uint16_t on = v->role_on;
    for (int r = 0; r < 16; r++) if (v->step_from[r] > step) on &= (uint16_t)~(1u << r);
    return on;
}

void rd_values_default(rd_values_t *v)
{
    static const uint32_t PAL[16] = {
        0xFF0A2233, 0xFF101A22, 0xFF18242E, 0xFF0E2C40, 0xFF1F5A80, 0xFF4A5966,
        0xFF6B7B88, 0xFF3D4A56, 0xFF6A7886, 0xFF2A5470, 0xFFB8C4CE, 0xFFE0B040,
        0xFF5B6B78, 0xFF3A4855, 0xFF2F3B46, 0 };
    memset(v, 0, sizeof *v);
    memcpy(v->map_pal, PAL, sizeof PAL);
    v->width[4] = 1; v->width[5] = 1; v->width[6] = 2; v->width[7] = 1; v->width[8] = 2;
    v->width[9] = 1; v->width[12] = 2; v->width[13] = 1; v->width[14] = 1;
    v->dash[7] = 2;                                        /* state lines long-dashed */
    v->role_on = 0x7FFF;
    /* the roads by step: motorways and trunks everywhere, primary from Area (60 km),
     * secondary from City (30 km), tertiary at Close alone (owner, 2026-11-07, twice: DFW
     * and London far too dense) */
    v->step_from[5] = 3; v->step_from[13] = 4; v->step_from[14] = 5;
    v->mark_on = 0x09;                                     /* cities and airports */
    v->names = 4;
    v->map_order = 0;
    v->rain_order = 1;
    /* rain: green, yellow, orange, red, magenta, over the 10 dBZ noise floor (value 84) */
    static const struct { int dbz; uint32_t argb; } RAMP[] = {
        { 10, 0x9900C850 }, { 20, 0xBF00E060 }, { 30, 0xD9F0E020 }, { 40, 0xE8F09020 },
        { 50, 0xF0E83020 }, { 60, 0xFFE020D0 }, { 70, 0xFFFFFFFF } };
    const int n = (int)(sizeof RAMP / sizeof RAMP[0]);
    for (int i = 0; i < 256; i++) {
        const double dbz = i / 2.0 - 32.0;
        if (i < 84) { v->rain[i] = 0; continue; }
        int k = 0;
        while (k < n - 1 && dbz >= RAMP[k + 1].dbz) k++;
        v->rain[i] = RAMP[k].argb;
    }
}
