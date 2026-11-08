#pragma once
/**
 * orb_rotation — the owner's carousel: the order the apps stand in, and the rotation
 * that moves the dial between them on its own (app-platform-plan §59, owner 2026-11-07).
 *
 * The order is a list of slugs. The registry's one walk gather ranks the installed apps
 * by it (orb_rotation_rank), so the carousel, the app list and the knob's neighbour all
 * agree; an app the list does not name stands after the named ones, in slot order.
 *
 * The rotation is on or off, an interval, and the apps that take part. The router asks
 * orb_rotation_due() on each poll; it moves only from an app that takes part, only once
 * the knob has been left alone for a whole interval, and to the next app that takes part
 * in the carousel's order.
 *
 * The decisions are pure (no NVS, no LVGL), so tools/host_rotation_test.py compiles this
 * file as shipped; persistence is behind ORB_ROTATION_NVS.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORB_ROTATION_MAX       16      /* slugs in the order and in the rotation */
#define ORB_ROTATION_SLUG      24      /* ORBAPP_SLUG_MAX */
#define ORB_ROTATION_MIN_S     10u     /* the shortest interval */
#define ORB_ROTATION_MAX_S     86400u  /* the longest: a day */
#define ORB_ROTATION_DEF_S     60u

typedef struct {
    bool     on;
    uint32_t every_s;
    int      n;                                        /* apps taking part */
    char     apps[ORB_ROTATION_MAX][ORB_ROTATION_SLUG];
} orb_rotation_t;

/** Load what NVS holds (or the defaults: no order, rotation off, 60 s, no apps). */
void orb_rotation_init(void);

/** The owner's order, as stored: n slugs into out. */
int  orb_rotation_order(char out[][ORB_ROTATION_SLUG], int max);
/** Replace the order from a comma-separated slug list. NULL = accepted; else why not. */
const char *orb_rotation_set_order(const char *csv);
/** An app's rank in the order: its index, or ORB_ROTATION_MAX + fallback when unnamed. */
int  orb_rotation_rank(const char *slug, int fallback);

void orb_rotation_get(orb_rotation_t *out);
/** Replace the rotation. NULL = accepted; else why not (an interval out of range). */
const char *orb_rotation_set(bool on, uint32_t every_s, const char *apps_csv);
bool orb_rotation_takes_part(const char *slug);

/** The pure decision. walk: the carousel's slugs in order; cur: the one on the glass
 *  (-1 = not an app); quiet_us: how long the knob and the glass have been left alone.
 *  Answers the walk index to move to, or -1 to stay. */
int  orb_rotation_pick(const orb_rotation_t *r, const char *const *walk, int n, int cur,
                       int64_t quiet_us);

/** Split a comma-separated slug list into out (trimmed, no empties, no repeats). */
int  orb_rotation_csv(const char *csv, char out[][ORB_ROTATION_SLUG], int max);
