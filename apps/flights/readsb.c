/* The readsb schema over the OS's JSON tokens — contract in readsb.h. */
#include "readsb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void ac_reset(orb_readsb_t *r)
{
    memset(&r->ac, 0, sizeof r->ac);
    r->ac.track = NAN;
    r->ac.gs = NAN;
    r->ac.baro_rate = NAN;
    r->ac.squawk = -1;
    r->have_lat = r->have_lon = r->have_track = false;
}

void orb_readsb_init(orb_readsb_t *r, orb_readsb_emit_fn emit, void *ctx)
{
    memset(r, 0, sizeof *r);
    r->emit = emit;
    r->ctx = ctx;
    r->list_depth = -1;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    while (n && dst[n - 1] == ' ') n--;          /* readsb pads callsigns to 8 */
    dst[n] = 0;
}

/* One scalar at the aircraft's own level: keep it if it is ours. */
static void take_field(orb_readsb_t *r, const orb_json_tok_t *t)
{
    if (t->key_trunc) return;
    const char *k = t->key;
    orb_ac_t *a = &r->ac;
    const bool str = t->type == ORB_JSON_STR, num = t->type == ORB_JSON_NUM;
    const double v = t->num;
    if (!strcmp(k, "hex")) {
        if (str) copy_str(a->hex, sizeof a->hex, t->str);
    } else if (!strcmp(k, "flight")) {
        if (str) copy_str(a->flight, sizeof a->flight, t->str);
    } else if (!strcmp(k, "t")) {
        if (str) copy_str(a->type, sizeof a->type, t->str);
    } else if (!strcmp(k, "lat")) {
        if (num) { a->lat = v; r->have_lat = true; }
    } else if (!strcmp(k, "lon")) {
        if (num) { a->lon = v; r->have_lon = true; }
    } else if (!strcmp(k, "alt_baro")) {
        if (str) {
            if (!strcmp(t->str, "ground")) { a->on_ground = true; a->has_alt = true; a->alt_ft = 0; }
        } else if (num) { a->alt_ft = (int32_t)v; a->has_alt = true; }
    } else if (!strcmp(k, "track")) {
        if (num) { a->track = (float)v; r->have_track = true; }
    } else if (!strcmp(k, "true_heading")) {
        if (!r->have_track && num) a->track = (float)v;   /* track wins in any order */
    } else if (!strcmp(k, "gs")) {
        if (num) a->gs = (float)v;
    } else if (!strcmp(k, "baro_rate")) {
        if (num) a->baro_rate = (float)v;
    } else if (!strcmp(k, "squawk")) {
        if (str && t->str[0]) a->squawk = (int16_t)atoi(t->str);
        else if (num) a->squawk = (int16_t)v;
    } else if (!strcmp(k, "seen_pos")) {
        if (num) a->seen_pos = (float)v;
    } else if (!strcmp(k, "dbFlags")) {
        if (num) a->military = ((long)v & 1) != 0;
    }
}

bool orb_readsb_token(orb_readsb_t *r, const orb_json_tok_t *t)
{
    if (r->error) return false;
    switch (t->type) {
    case ORB_JSON_ERROR:
        r->error = true;
        return false;
    case ORB_JSON_ARR:
        /* The array under "ac" (adsb.lol) or "aircraft" (adsb.fi) in the top object. */
        if (t->depth == 1 && r->list_depth < 0 &&
            (!strcmp(t->key, "ac") || !strcmp(t->key, "aircraft")))
            r->list_depth = t->depth;
        break;
    case ORB_JSON_OBJ:
        if (r->list_depth >= 0 && t->depth == r->list_depth + 1) { r->in_ac = true; ac_reset(r); }
        break;
    case ORB_JSON_OBJ_END:
        if (r->in_ac && t->depth == r->list_depth + 1) {
            r->in_ac = false;
            r->objects++;
            if (r->ac.hex[0] && r->have_lat && r->have_lon) {
                r->emitted++;
                if (r->emit) r->emit(&r->ac, r->ctx);
            }
        }
        if (t->depth == 0) r->closed = true;
        break;
    case ORB_JSON_ARR_END:
        if (t->depth == 0) r->closed = true;
        break;
    case ORB_JSON_END:
        break;
    default:                                      /* a scalar */
        if (r->in_ac && t->depth == r->list_depth + 2) take_field(r, t);
        break;
    }
    return true;
}

bool orb_readsb_done(const orb_readsb_t *r)
{
    return !r->error && r->closed && r->list_depth >= 0;
}
