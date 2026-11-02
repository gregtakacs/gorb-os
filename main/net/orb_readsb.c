/* Streaming readsb reader — contract in orb_readsb.h. */
#include "orb_readsb.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { M_VALUE, M_STRING, M_LITERAL, M_AFTER };

static bool in_aircraft(const orb_readsb_t *r)
{
    /* The aircraft object's own fields: one level inside the array. */
    return r->list_depth > 0 && r->depth == r->list_depth + 1 &&
           r->depth <= ORB_READSB_DEPTH && r->stack[r->depth - 1] == '{';
}

static void ac_reset(orb_readsb_t *r)
{
    memset(&r->ac, 0, sizeof r->ac);
    r->ac.track = NAN;
    r->ac.gs = NAN;
    r->ac.baro_rate = NAN;
    r->ac.squawk = -1;
    r->have_lat = r->have_lon = r->have_track = false;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    while (n && dst[n - 1] == ' ') n--;          /* readsb pads callsigns to 8 */
    dst[n] = 0;
}

static bool num(const orb_readsb_t *r, double *out)
{
    if (r->tok_str || !r->tok_len) return false;
    char *end;
    const double v = strtod(r->tok, &end);
    if (end == r->tok || *end) return false;     /* null, true, false */
    *out = v;
    return true;
}

/* One scalar finished at the aircraft's own level: keep it if it is ours. */
static void take_field(orb_readsb_t *r)
{
    if (r->key_trunc) return;
    const char *k = r->key;
    orb_ac_t *a = &r->ac;
    double v;
    if (!strcmp(k, "hex")) {
        if (r->tok_str) copy_str(a->hex, sizeof a->hex, r->tok);
    } else if (!strcmp(k, "flight")) {
        if (r->tok_str) copy_str(a->flight, sizeof a->flight, r->tok);
    } else if (!strcmp(k, "t")) {
        if (r->tok_str) copy_str(a->type, sizeof a->type, r->tok);
    } else if (!strcmp(k, "lat")) {
        if (num(r, &v)) { a->lat = v; r->have_lat = true; }
    } else if (!strcmp(k, "lon")) {
        if (num(r, &v)) { a->lon = v; r->have_lon = true; }
    } else if (!strcmp(k, "alt_baro")) {
        if (r->tok_str) {
            if (!strcmp(r->tok, "ground")) { a->on_ground = true; a->has_alt = true; a->alt_ft = 0; }
        } else if (num(r, &v)) { a->alt_ft = (int32_t)v; a->has_alt = true; }
    } else if (!strcmp(k, "track")) {
        if (num(r, &v)) { a->track = (float)v; r->have_track = true; }
    } else if (!strcmp(k, "true_heading")) {
        if (!r->have_track && num(r, &v)) a->track = (float)v;   /* track wins in any order */
    } else if (!strcmp(k, "gs")) {
        if (num(r, &v)) a->gs = (float)v;
    } else if (!strcmp(k, "baro_rate")) {
        if (num(r, &v)) a->baro_rate = (float)v;
    } else if (!strcmp(k, "squawk")) {
        if (r->tok_str && r->tok_len) a->squawk = (int16_t)atoi(r->tok);
        else if (num(r, &v)) a->squawk = (int16_t)v;
    } else if (!strcmp(k, "seen_pos")) {
        if (num(r, &v)) a->seen_pos = (float)v;
    } else if (!strcmp(k, "dbFlags")) {
        if (num(r, &v)) a->military = ((long)v & 1) != 0;
    }
}

static void value_done(orb_readsb_t *r)
{
    if (in_aircraft(r)) take_field(r);
    r->mode = M_AFTER;
}

static bool open_container(orb_readsb_t *r, char c)
{
    /* The array under "ac" (adsb.lol) or "aircraft" (adsb.fi) in the top object. */
    if (c == '[' && r->depth == 1 && r->list_depth == 0 &&
        (!strcmp(r->key, "ac") || !strcmp(r->key, "aircraft")))
        r->list_depth = 2;
    if (r->depth < ORB_READSB_DEPTH) r->stack[r->depth] = c;
    r->depth++;
    if (in_aircraft(r)) ac_reset(r);
    r->want_key = (c == '{');
    r->mode = M_VALUE;
    return true;
}

static bool close_container(orb_readsb_t *r, char c)
{
    if (r->depth == 0) return false;
    const char open = r->depth <= ORB_READSB_DEPTH ? r->stack[r->depth - 1] : 0;
    if (open && ((c == '}') != (open == '{'))) return false;
    if (in_aircraft(r)) {
        r->objects++;
        if (r->ac.hex[0] && r->have_lat && r->have_lon) {
            r->emitted++;
            if (r->emit) r->emit(&r->ac, r->ctx);
        }
    }
    r->depth--;
    r->mode = M_AFTER;
    return true;
}

void orb_readsb_init(orb_readsb_t *r, orb_readsb_emit_fn emit, void *ctx)
{
    memset(r, 0, sizeof *r);
    r->emit = emit;
    r->ctx = ctx;
    r->mode = M_VALUE;
}

bool orb_readsb_feed(orb_readsb_t *r, const char *buf, size_t len)
{
    for (size_t i = 0; i < len && !r->error; i++) {
        const char c = buf[i];
        switch (r->mode) {
        case M_STRING:
            if (r->esc) {
                r->esc = false;
                /* An escape keeps its letter: no field we read can carry one
                 * that matters, and \uXXXX only has to not end the string. */
            } else if (c == '\\') {
                r->esc = true;
                continue;
            } else if (c == '"') {
                if (r->want_key) {
                    r->key[r->key_len] = 0;
                    r->want_key = false;
                    r->mode = M_AFTER;           /* expects ':' */
                } else {
                    r->tok[r->tok_len] = 0;
                    value_done(r);
                }
                continue;
            }
            if (r->want_key) {
                if (r->key_len + 1 < sizeof r->key) r->key[r->key_len++] = c;
                else r->key_trunc = true;
            } else if (r->tok_len + 1 < sizeof r->tok) {
                r->tok[r->tok_len++] = c;
            }
            continue;
        case M_LITERAL:
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\n' ||
                c == '\r' || c == '\t') {
                r->tok[r->tok_len] = 0;
                value_done(r);
                break;                           /* re-read c as punctuation */
            }
            if (r->tok_len + 1 < sizeof r->tok) r->tok[r->tok_len++] = c;
            else r->error = true;                /* no number is 23 chars long */
            continue;
        default:
            break;
        }

        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;

        if (r->mode == M_AFTER) {
            if (c == ':') { r->mode = M_VALUE; continue; }
            if (c == ',') {
                const bool obj = r->depth && r->depth <= ORB_READSB_DEPTH &&
                                 r->stack[r->depth - 1] == '{';
                r->want_key = obj;
                r->mode = M_VALUE;
                continue;
            }
            if (c == '}' || c == ']') { if (!close_container(r, c)) r->error = true; continue; }
            r->error = true;
            continue;
        }

        /* M_VALUE: a key (in an object, after '{' or ','), or a value. */
        if (c == '"') {
            r->mode = M_STRING;
            r->esc = false;
            if (r->want_key) { r->key_len = 0; r->key_trunc = false; }
            else { r->tok_len = 0; r->tok_str = true; }
            continue;
        }
        if (c == '{' || c == '[') { open_container(r, c); continue; }
        if (c == '}' || c == ']') {               /* an empty container */
            if (!close_container(r, c)) r->error = true;
            continue;
        }
        if (r->want_key) { r->error = true; continue; }
        r->mode = M_LITERAL;
        r->tok_str = false;
        r->tok_len = 0;
        r->tok[r->tok_len++] = c;
    }
    return !r->error;
}

bool orb_readsb_done(const orb_readsb_t *r)
{
    return !r->error && r->depth == 0 && r->list_depth > 0 && r->mode == M_AFTER;
}
