/* orb_setui — an app's settings schema, parsed once and drawn (orb_setui.h). */
#include "orb_setui.h"

#include <stdio.h>
#include <string.h>

#include "orb_json.h"

static const char *const TYPES[ORB_SETUI_TYPES] = {
    "int", "number", "enum", "multi", "toggle", "text", "place", "secret", "color",
};
static const char *const DIST[3] = { "km", "mi", "nm" };

/* §9.1: the twelve colours the dial steps, in that order, one name each. The single
 * source of truth for the palette — the hub copies these names as text (tabs.js), and
 * host_setui_test.py pins the two tables against each other. An app's colour setting
 * stores whichever of these its owner picked, as the int itself. */
static const struct { uint32_t rgb; const char *name; } s_palette[ORB_SETUI_PALETTE] = {
    { 0xD62828, "Ruby" },     { 0xF77F00, "Ember" },    { 0xFFC300, "Amber" },
    { 0x80B918, "Lime" },     { 0x0FA36B, "Emerald" },  { 0x0FBFB1, "Teal" },
    { 0x2E93E4, "Sky" },      { 0x2D5BD0, "Sapphire" }, { 0x5B4BFF, "Indigo" },
    { 0x9B43E0, "Violet" },   { 0xD93B8E, "Magenta" },  { 0xEDEDF3, "Pearl" },
};

const char *orb_setui_palette_name(int i)
{
    return (i >= 0 && i < ORB_SETUI_PALETTE) ? s_palette[i].name : "";
}

uint32_t orb_setui_palette_rgb(int i)
{
    return (i >= 0 && i < ORB_SETUI_PALETTE) ? s_palette[i].rgb : 0;
}

int orb_setui_palette_find(uint32_t rgb)
{
    for (int i = 0; i < ORB_SETUI_PALETTE; i++) if (s_palette[i].rgb == rgb) return i;
    return -1;
}

const char *orb_setui_special(const orb_setui_t *ui, const orb_setui_row_t *r, int i)
{
    return orb_setui_choice(ui, r, i, 0);
}

int orb_setui_color_pos(const orb_setui_row_t *r, int32_t v)
{
    if (v < 0) {                                  /* a special it declared, -(v + 1) 0-based */
        const int i = -(v + 1);
        return i < r->n_choices ? i : -1;
    }
    const int p = orb_setui_palette_find((uint32_t)v);
    return p >= 0 ? r->n_choices + p : r->max;                  /* else the custom slot */
}

int32_t orb_setui_color_val(const orb_setui_row_t *r, int pos, int32_t cur)
{
    if (pos < r->n_choices) return pos < 0 ? cur : -(pos + 1);
    if (pos < r->n_choices + ORB_SETUI_PALETTE) return (int32_t)orb_setui_palette_rgb(pos - r->n_choices);
    return cur;                                   /* "custom": the hub's value stands */
}

size_t orb_setui_bytes(size_t json_len)
{
    return sizeof(orb_setui_t) + json_len + 1;
}

/* A choice's text into the pool; its offset + 1, or 0 when the pool is full. */
static uint16_t pool_put(orb_setui_t *ui, const char *s)
{
    const size_t n = strlen(s) + 1;
    if ((size_t)ui->pool_used + n > ui->pool_cap) return 0;
    memcpy(ui->pool + ui->pool_used, s, n);
    const uint16_t at = (uint16_t)(ui->pool_used + 1);
    ui->pool_used = (uint16_t)(ui->pool_used + n);
    return at;
}

static void copy(char *dst, size_t cap, const char *src)
{
    size_t n = 0;                                    /* the writer held it to fit */
    while (n + 1 < cap && src[n]) n++;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool orb_setui_parse(const char *json, size_t len, orb_setui_t *ui)
{
    const uint16_t cap = (uint16_t)(len + 1 > 0xFFFF ? 0xFFFF : len + 1);
    memset(ui, 0, sizeof *ui);
    ui->pool_cap = cap;
    orb_json_t j;
    orb_json_tok_t t;
    orb_json_begin(&j, json, len);
    if (orb_json_next(&j, &t) != ORB_JSON_ARR || t.depth != 0) return false;
    orb_setui_row_t *r = NULL;
    for (;;) {
        const int32_t k = orb_json_next(&j, &t);
        if (k == ORB_JSON_ERROR) return false;
        if (k == ORB_JSON_END || (k == ORB_JSON_ARR_END && t.depth == 0)) break;
        if (k == ORB_JSON_OBJ && t.depth == 1) {           /* a setting */
            if (ui->n >= ORB_SETUI_MAX) return false;
            r = &ui->row[ui->n++];
            r->type = ORB_SETUI_TYPES;                         /* none yet */
            r->step = 1;
            continue;
        }
        if (k == ORB_JSON_OBJ_END && t.depth == 1) {
            if (!r || r->type >= ORB_SETUI_TYPES || !r->id[0]) return false;
            r = NULL;
            continue;
        }
        if (!r || t.depth != 2) continue;
        if (k == ORB_JSON_ARR && (!strcmp(t.key, "choices") || !strcmp(t.key, "specials"))) {
            /* strings, or {"km", "mi", "nm"} objects, each a choice. A color setting
             * carries no choices: the same slots hold its specials' names, in order. */
            for (;;) {
                const int32_t c = orb_json_next(&j, &t);
                if (c == ORB_JSON_ARR_END && t.depth == 2) break;
                if (c == ORB_JSON_ERROR || c == ORB_JSON_END) return false;
                if (r->n_choices >= ORB_SETUI_CHOICES) return false;
                uint16_t *slot = r->choice[r->n_choices];
                if (c == ORB_JSON_STR) {
                    slot[0] = slot[1] = slot[2] = pool_put(ui, t.str);
                    if (!slot[0]) return false;
                } else if (c == ORB_JSON_OBJ) {
                    for (;;) {
                        const int32_t d = orb_json_next(&j, &t);
                        if (d == ORB_JSON_OBJ_END) break;
                        if (d != ORB_JSON_STR) return false;
                        for (int u = 0; u < 3; u++)
                            if (!strcmp(t.key, DIST[u]) && !(slot[u] = pool_put(ui, t.str))) return false;
                    }
                    if (!slot[0] || !slot[1] || !slot[2]) return false;
                } else return false;
                r->n_choices++;
            }
            continue;
        }
        if (k == ORB_JSON_ARR || k == ORB_JSON_OBJ) {       /* links: the hub's */
            if (orb_json_skip(&j) == ORB_JSON_ERROR) return false;
            continue;
        }
        if (k == ORB_JSON_STR) {
            if (!strcmp(t.key, "id")) copy(r->id, sizeof r->id, t.str);
            else if (!strcmp(t.key, "label")) copy(r->label, sizeof r->label, t.str);
            else if (!strcmp(t.key, "unit")) copy(r->unit, sizeof r->unit, t.str);
            else if (!strcmp(t.key, "type")) {
                for (int i = 0; i < ORB_SETUI_TYPES; i++) if (!strcmp(t.str, TYPES[i])) r->type = (uint8_t)i;
                if (r->type >= ORB_SETUI_TYPES) return false;
            } else if (!strcmp(t.key, "show")) {
                r->show = !strcmp(t.str, "both") ? (ORB_SETUI_DIAL | ORB_SETUI_HUB)
                        : !strcmp(t.str, "dial") ? ORB_SETUI_DIAL
                        : !strcmp(t.str, "hub") ? ORB_SETUI_HUB : 0;
            }
        } else if (k == ORB_JSON_NUM) {
            const int32_t v = (int32_t)t.num;
            if (!strcmp(t.key, "min")) r->min = v;
            else if (!strcmp(t.key, "max")) r->max = v;
            else if (!strcmp(t.key, "step")) r->step = v > 0 ? v : 1;
            else if (!strcmp(t.key, "decimals")) r->decimals = (uint8_t)(v > 0 && v <= 3 ? v : 0);
        }
    }
    /* the ranges the knob walks: an enum and a toggle over their choices, a multi
     * over its bits (the dial edits it a choice at a time) */
    for (int i = 0; i < ui->n; i++) {
        orb_setui_row_t *q = &ui->row[i];
        if (q->type == ORB_SETUI_ENUM || q->type == ORB_SETUI_TOGGLE) {
            if (q->n_choices < 2) return false;
            q->min = 0;
            q->max = q->n_choices - 1;
        } else if (q->type == ORB_SETUI_MULTI) {
            if (!q->n_choices) return false;
            q->min = 0;
            q->max = (int32_t)((1u << q->n_choices) - 1u);
        } else if (q->type == ORB_SETUI_COLOR) {
            /* the knob's positions, not the stored range: the specials the app named,
             * then the twelve palette colours, then "custom" for what the hub set */
            if (q->n_choices > ORB_SETUI_SPECIALS) return false;
            q->min = 0;
            q->max = q->n_choices + ORB_SETUI_PALETTE;
        }
    }
    return true;
}

const char *orb_setui_choice(const orb_setui_t *ui, const orb_setui_row_t *r, int i, int dist)
{
    if (!r || i < 0 || i >= r->n_choices) return "?";
    const uint16_t at = r->choice[i][dist >= 0 && dist < 3 ? dist : 0];
    return at ? ui->pool + at - 1 : "?";
}

void orb_setui_value(const orb_setui_t *ui, const orb_setui_row_t *r, int32_t v, int dist,
                  char *out, size_t n)
{
    if (!n) return;
    out[0] = 0;
    switch (r->type) {
    case ORB_SETUI_INT:
    case ORB_SETUI_NUMBER: {
        const char *sp = r->unit[0] && r->unit[0] != '%' && strncmp(r->unit, "\xC2\xB0", 2) ? " " : "";
        if (r->type == ORB_SETUI_NUMBER && r->decimals) {
            int32_t k = 1;
            for (int i = 0; i < r->decimals; i++) k *= 10;
            const int32_t a = v < 0 ? -v : v;
            snprintf(out, n, "%s%ld.%0*ld%s%s", v < 0 ? "-" : "", (long)(a / k), (int)r->decimals,
                     (long)(a % k), sp, r->unit);
        } else snprintf(out, n, "%ld%s%s", (long)v, sp, r->unit);
        break;
    }
    case ORB_SETUI_ENUM:
    case ORB_SETUI_TOGGLE:
        snprintf(out, n, "%s", orb_setui_choice(ui, r, v, dist));
        break;
    case ORB_SETUI_COLOR: {
        const char *nm = NULL;
        if (v < 0) nm = (-v - 1) < r->n_choices ? orb_setui_special(ui, r, -v - 1) : NULL;
        else {
            const int p = orb_setui_palette_find((uint32_t)v);
            if (p >= 0) nm = s_palette[p].name;
        }
        snprintf(out, n, "%s", nm ? nm : "custom");            /* §9: never raw hex here */
        break;
    }
    case ORB_SETUI_MULTI: {
        int on = 0;
        for (int i = 0; i < r->n_choices; i++) on += (v >> i) & 1;
        if (!on) { snprintf(out, n, "none"); break; }
        size_t used = 0;
        bool fits = true;
        for (int i = 0; i < r->n_choices && fits; i++) {
            if (!((v >> i) & 1)) continue;
            const int w = snprintf(out + used, n - used, "%s%s", used ? ", " : "", orb_setui_choice(ui, r, i, dist));
            if (w < 0 || (size_t)w >= n - used) fits = false;
            else used += (size_t)w;
        }
        if (!fits) snprintf(out, n, "%d of %d", on, (int)r->n_choices);
        break;
    }
    default:
        break;
    }
}
