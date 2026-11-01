/* The flight tracker's model — contract in flights_model.h. */
#include "flights_model.h"

#include <math.h>
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

static bool on_glass(const fm_rect_t *a, int cx, int cy, int r)
{
    for (int c = 0; c < 4; c++) {
        const int px = (c & 1) ? a->x2 : a->x1, py = (c & 2) ? a->y2 : a->y1;
        if ((px - cx) * (px - cx) + (py - cy) * (py - cy) >= r * r) return false;
    }
    return true;
}

static bool clear_of(const fm_rect_t *a, const fm_rect_t *placed, int np)
{
    for (int q = 0; q < np; q++)
        if (!(a->x2 < placed[q].x1 - 2 || a->x1 > placed[q].x2 + 2 ||
              a->y2 < placed[q].y1 - 1 || a->y1 > placed[q].y2 + 1))
            return false;
    return true;
}

bool fm_place_label(const fm_rect_t *placed, int np, int x, int y, int w, int h,
                    int cx, int cy, int glass_r, fm_rect_t *out)
{
    const bool right = x >= cx;
    const int cand[4][2] = {
        { right ? x + 8 : x - 8 - w, y - h / 2 },
        { right ? x - 8 - w : x + 8, y - h / 2 },
        { x - w / 2, y - 10 - h },
        { x - w / 2, y + 10 },
    };
    for (int k = 0; k < 4; k++) {
        const fm_rect_t a = { cand[k][0], cand[k][1], cand[k][0] + w, cand[k][1] + h };
        if (on_glass(&a, cx, cy, glass_r) && clear_of(&a, placed, np)) {
            *out = a;
            return true;
        }
    }
    return false;
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
