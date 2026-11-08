#pragma once
/* orb_setui — an app's settings schema, parsed once and drawn (app-platform-plan §57).
 *
 * The app declares its settings in app.json; tools/orbapp.py checks them and writes the
 * schema region, compact JSON naming each setting's type, label, how it shows and, for
 * the kinds the knob edits, its range, step, unit, decimals and choices. The OS parses
 * that region here into fixed rows the dial's options page reads, and formats a value
 * the way the hub does. The help, links, host and header ride the region for the hub
 * (GET /api/v1/apps/settings) and are not kept here.
 *
 * Pure C over orb_json, no IDF include: tools/host_ui_test.py compiles it as it is and
 * feeds it the writer's own regions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_SETUI_MAX       16      /* ORBAPP_SETTINGS_MAX: one row per declared setting */
#define ORB_SETUI_CHOICES   16      /* orbapp.py CHOICES_MAX: a multi's bitmask fits 16 bits */
#define ORB_SETUI_LABEL     24      /* orbapp.py LABEL_MAX: bytes, NUL included */
#define ORB_SETUI_UNIT       9      /* orbapp.py UNIT_MAX + NUL */

/* orbapp.py UI_TYPES, in that order. */
enum { ORB_SETUI_INT = 0, ORB_SETUI_NUMBER, ORB_SETUI_ENUM, ORB_SETUI_MULTI, ORB_SETUI_TOGGLE,
       ORB_SETUI_TEXT, ORB_SETUI_PLACE, ORB_SETUI_SECRET, ORB_SETUI_TYPES };

/* Where a setting shows: orbapp.py SHOWS as bits (both = dial | hub, none = 0). */
enum { ORB_SETUI_DIAL = 1, ORB_SETUI_HUB = 2 };

typedef struct {
    char     id[16];                 /* the NVS key, as the settings table names it */
    char     label[ORB_SETUI_LABEL];
    char     unit[ORB_SETUI_UNIT];      /* int, number: a suffix ("°", "%"), or "" */
    uint8_t  type;                   /* ORB_SETUI_* */
    uint8_t  show;                   /* ORB_SETUI_DIAL | ORB_SETUI_HUB */
    uint8_t  decimals;               /* number: the value is stored times 10^decimals */
    uint8_t  n_choices;              /* enum, multi, toggle */
    int32_t  min, max, step;         /* int, number: the stored range and the knob's step */
    /* each choice's label for the orb's distance unit (km, mi, nm), as offsets + 1 into
     * the pool; a plain choice names one label three times */
    uint16_t choice[ORB_SETUI_CHOICES][3];
} orb_setui_row_t;

typedef struct orb_setui {
    uint8_t      n;                  /* rows, in the manifest's order */
    orb_setui_row_t row[ORB_SETUI_MAX];
    uint16_t     pool_used, pool_cap;
    char         pool[];             /* the choices' text, NUL-terminated each */
} orb_setui_t;

/** The bytes an orb_setui_t for a region of `json_len` bytes needs: its choices' text
 *  is never longer than the region that carried it. */
size_t orb_setui_bytes(size_t json_len);

/** Parse a schema region into `ui` (orb_setui_bytes(len) bytes). False when it is not
 *  the writer's shape: not an array of objects, a type it does not know, more rows or
 *  choices than the tables hold. */
bool orb_setui_parse(const char *json, size_t len, orb_setui_t *ui);

/** Choice i's label in distance unit `dist` (0 km, 1 mi, 2 nm); "?" out of range. */
const char *orb_setui_choice(const orb_setui_t *ui, const orb_setui_row_t *r, int i, int dist);

/** A stored value as the dial and the hub show it: "1.5 x", "25 mi", "on", "Mon, Wed",
 *  "2 of 5". Text kinds (text, place, secret) are not drawn here and answer "". */
void orb_setui_value(const orb_setui_t *ui, const orb_setui_row_t *r, int32_t v, int dist,
                  char *out, size_t n);
