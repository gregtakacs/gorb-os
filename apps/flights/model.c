/* The flight tracker's model — contract in flights_model.h. */
#include "model.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void fm_merge_begin(fm_t *m)
{
    for (int i = 0; i < m->n; i++) m->t[i].live = false;
}

void fm_upsert(fm_t *m, const orb_ac_t *ac, float dist_km, float bearing_deg, int64_t now_us)
{
    int k = -1;
    for (int j = 0; j < m->n && k < 0; j++)
        if (!strcmp(m->t[j].ac.hex, ac->hex)) k = j;
    if (k < 0 && m->n < FM_MAX_TRK) k = m->n++;
    if (k < 0) {                         /* full: replace the stalest */
        k = 0;
        for (int j = 1; j < m->n; j++)
            if (m->t[j].t_seen_us < m->t[k].t_seen_us) k = j;
    }
    fm_trk_t *t = &m->t[k];
    t->ac = *ac;
    const float b = bearing_deg * (float)M_PI / 180.0f;
    t->e_km = dist_km * sinf(b);
    t->n_km = dist_km * cosf(b);
    t->t_fix_us = now_us - (int64_t)(ac->seen_pos * 1e6f);
    t->t_seen_us = now_us;
    t->live = true;
}

void fm_merge_end(fm_t *m, int64_t now_us)
{
    for (int j = 0; j < m->n;) {
        if (now_us - m->t[j].t_seen_us > FM_STALE_US) m->t[j] = m->t[--m->n];
        else j++;
    }
}

void fm_project(fm_t *m, float range_km, int64_t now_us, int cx, int cy, int ring_r)
{
    m->norder = 0;
    for (int i = 0; i < m->n; i++) {
        fm_trk_t *t = &m->t[i];
        float e = t->e_km, n = t->n_km;
        if (!isnan(t->ac.track) && !isnan(t->ac.gs) && !t->ac.on_ground) {
            float dt = (float)(now_us - t->t_fix_us) / 1e6f;
            if (dt > FM_DR_CAP_S) dt = FM_DR_CAP_S;
            if (dt < 0) dt = 0;
            const float km = t->ac.gs * 1.852f / 3600.0f * dt;
            const float tr = t->ac.track * (float)M_PI / 180.0f;
            e += km * sinf(tr);
            n += km * cosf(tr);
        }
        t->d_km = sqrtf(e * e + n * n);
        t->drawn = t->d_km <= range_km;
        if (!t->drawn) continue;
        t->x = cx + e / range_km * ring_r;
        t->y = cy - n / range_km * ring_r;
        m->order[m->norder++] = i;
    }
    for (int i = 1; i < m->norder; i++)
        for (int j = i; j > 0 && m->t[m->order[j]].d_km < m->t[m->order[j - 1]].d_km; j--) {
            const int s = m->order[j]; m->order[j] = m->order[j - 1]; m->order[j - 1] = s;
        }
}

int fm_find(const fm_t *m, const char *hex)
{
    if (!hex || !hex[0]) return -1;
    for (int o = 0; o < m->norder; o++)
        if (!strcmp(m->t[m->order[o]].ac.hex, hex)) return o;
    return -1;
}

void fm_sel_step(const fm_t *m, const char *hex, int step, char *hex_out)
{
    if (m->norder <= 0) { hex_out[0] = 0; return; }
    const int at = fm_find(m, hex);
    int next;
    if (at < 0) next = step < 0 ? m->norder - 1 : 0;
    else        next = ((at + (step < 0 ? -1 : 1)) % m->norder + m->norder) % m->norder;
    memcpy(hex_out, m->t[m->order[next]].ac.hex, sizeof m->t[0].ac.hex);
}

bool fm_interesting(const orb_ac_t *a, float dist_km, float bearing_deg)
{
    if (a->squawk == 7500 || a->squawk == 7600 || a->squawk == 7700) return true;
    if (a->on_ground) return false;
    if (dist_km < 9.26f) return true;                                /* 5 nm */
    if (a->has_alt && a->alt_ft < 3000 && dist_km < 18.52f && !isnan(a->track)) {
        const float home = bearing_deg + 180.0f;
        float d = fmodf(fabsf(a->track - home), 360.0f);
        if (d > 180.0f) d = 360.0f - d;
        if (d < 60.0f) return true;
    }
    return false;
}

/* ---- route ---------------------------------------------------------------- */

bool fm_route_callsign_ok(const char *cs)
{
    for (int i = 0; i < 3; i++)
        if (cs[i] < 'A' || cs[i] > 'Z') return false;
    if (cs[3] < '0' || cs[3] > '9') return false;
    int i = 4;
    for (; cs[i]; i++)
        if (i >= 7 || !((cs[i] >= '0' && cs[i] <= '9') || (cs[i] >= 'A' && cs[i] <= 'Z'))) return false;
    return true;
}

/* adsbdb's answer, read from the OS's JSON events (D51): the two flat objects under
 * "flightroute", "origin" and "destination", each an airport code (IATA, else ICAO:
 * upper-case letters and digits, one to four) and a position. */
void fm_route_begin(fm_route_rd_t *r)
{
    memset(r, 0, sizeof *r);
    r->fr_depth = -1;
}

static bool code_ok(const orb_json_tok_t *t)
{
    if (t->type != ORB_JSON_STR || t->str_trunc || !t->str[0] || strlen(t->str) > 4) return false;
    for (const char *c = t->str; *c; c++)
        if (*c < '0' || (*c > '9' && *c < 'A') || *c > 'Z') return false;
    return true;
}

void fm_route_take(fm_route_rd_t *r, const orb_json_tok_t *t)
{
    if (t->type == ORB_JSON_OBJ && r->fr_depth < 0 && !strcmp(t->key, "flightroute")) {
        r->fr_depth = t->depth;
        r->saw_fr = true;
        return;
    }
    if (r->fr_depth < 0) return;
    if (t->type == ORB_JSON_OBJ_END && t->depth == r->fr_depth) { r->fr_depth = -1; return; }
    if (t->type == ORB_JSON_OBJ && t->depth == r->fr_depth + 1) {
        r->end = !strcmp(t->key, "origin") ? 1 : !strcmp(t->key, "destination") ? 2 : 0;
        return;
    }
    if (t->type == ORB_JSON_OBJ_END && t->depth == r->fr_depth + 1) { r->end = 0; return; }
    if (!r->end || t->depth != r->fr_depth + 2 || t->key_trunc) return;
    const int e = r->end - 1;
    if (!strcmp(t->key, "iata_code")) {
        r->iata_ok[e] = code_ok(t);
        if (r->iata_ok[e]) memcpy(r->iata[e], t->str, strlen(t->str) + 1);
    } else if (!strcmp(t->key, "icao_code")) {
        r->icao_ok[e] = code_ok(t);
        if (r->icao_ok[e]) memcpy(r->icao[e], t->str, strlen(t->str) + 1);
    } else if (!strcmp(t->key, "latitude") && t->type == ORB_JSON_NUM && isfinite(t->num)) {
        r->lat[e] = (float)t->num;
        r->has_lat[e] = true;
    } else if (!strcmp(t->key, "longitude") && t->type == ORB_JSON_NUM && isfinite(t->num)) {
        r->lon[e] = (float)t->num;
        r->has_lon[e] = true;
    }
}

bool fm_route_end(const fm_route_rd_t *r, fm_route_t *out)
{
    if (!r->saw_fr) return false;
    fm_route_t rt;
    memset(&rt, 0, sizeof rt);
    char *code[2] = { rt.from, rt.to };
    float *lat[2] = { &rt.from_lat, &rt.to_lat }, *lon[2] = { &rt.from_lon, &rt.to_lon };
    for (int e = 0; e < 2; e++) {
        if (!r->iata_ok[e] && !r->icao_ok[e]) return false;
        if (!r->has_lat[e] || !r->has_lon[e]) return false;
        if (fabsf(r->lat[e]) > 90.0f || fabsf(r->lon[e]) > 180.0f) return false;
        memcpy(code[e], r->iata_ok[e] ? r->iata[e] : r->icao[e], 5);
        *lat[e] = r->lat[e];
        *lon[e] = r->lon[e];
    }
    *out = rt;
    return true;
}

/* Central angle between two points, radians (haversine). */
static float arc(float la1, float lo1, float la2, float lo2)
{
    const float a = sinf((la2 - la1) / 2), b = sinf((lo2 - lo1) / 2);
    const float h = a * a + cosf(la1) * cosf(la2) * b * b;
    return 2.0f * asinf(sqrtf(h > 1.0f ? 1.0f : h));
}

static float course(float la1, float lo1, float la2, float lo2)
{
    return atan2f(sinf(lo2 - lo1) * cosf(la2),
                  cosf(la1) * sinf(la2) - sinf(la1) * cosf(la2) * cosf(lo2 - lo1));
}

/* The angle between a track (deg) and a course (rad), 0..180 deg. */
static float off_course(float track_deg, float course_rad)
{
    float d = fmodf(fabsf(track_deg - course_rad * 180.0f / (float)M_PI), 360.0f);
    return d > 180.0f ? 360.0f - d : d;
}

bool fm_route_plausible(const fm_route_t *r, double lat, double lon, float track, float baro_rate)
{
    const float R = 6371.0f, k = (float)(M_PI / 180.0);
    const float la1 = r->from_lat * k, lo1 = r->from_lon * k;
    const float la2 = r->to_lat * k, lo2 = r->to_lon * k;
    const float la3 = (float)lat * k, lo3 = (float)lon * k;
    const bool dir = !isnan(track);
    const float d13 = arc(la1, lo1, la3, lo3), d23 = arc(la2, lo2, la3, lo3);
    const bool at_from = d13 * R < 100.0f, at_to = d23 * R < 100.0f;
    if (at_from || at_to) {
        if (!dir) return true;
        const bool leaving  = off_course(track, course(la1, lo1, la3, lo3)) < 90.0f || baro_rate > 0.0f;
        const bool arriving = off_course(track, course(la3, lo3, la2, lo2)) < 90.0f || baro_rate < 0.0f;
        return (at_from && leaving) || (at_to && arriving);
    }
    const float d12 = arc(la1, lo1, la2, lo2);
    const float dth = course(la1, lo1, la3, lo3) - course(la1, lo1, la2, lo2);
    if (cosf(dth) <= 0.0f) return false;                                 /* behind the origin */
    const float xt = asinf(sinf(d13) * sinf(dth));                       /* off the line */
    const float off = fmaxf(150.0f, 0.15f * d12 * R);
    if (fabsf(xt) * R > off) return false;
    const float c = cosf(d13) / cosf(xt);
    const float at = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);     /* along it */
    if (at > d12) return false;                                          /* past the end */
    return !dir || off_course(track, course(la3, lo3, la2, lo2)) < 90.0f;
}
