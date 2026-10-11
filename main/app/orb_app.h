#pragma once
/**
 * orb_app — the app registry (docs/app-platform-plan.md §1.4).
 *
 * One compile-time table replaces three hardcoded couplings: main.c's fixed
 * *_init() order, input_router.c's INPUT_SCREEN_* switches, and orb_net.c's
 * to=clock|menu|hue name test. The shell consults the table; nobody reaches
 * into an app by name any more.
 *
 * Every hook runs on the LVGL task. The router's 10 ms poll is the only
 * caller, and it is the one context allowed to touch widgets, so an app's
 * activate/resume/input need no locking of their own. service() is the
 * exception by design: it reaps the app's own worker jobs, so it must stay
 * safe to run while the app is suspended (C7). It may never repaint: a hidden
 * app committing rows is the cadence collapse the fps findings measured, and
 * the OS drops and logs those marks.
 *
 * Compiled-in entries only, for now. The .orbapp manifest reader, the `apps`
 * partition catalog, the enable/failed states and the install path arrive in
 * P4; the fields they need (slug, art offset/size, entry point) are already
 * here so the ABI does not move underneath them, and the entries stay
 * enumerable so a slug resolves to a slot rather than an integer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct _lv_obj_t;

typedef struct orb_app orb_app_t;

/** The hooks the shell calls, all on the LVGL task. A hook an app does not
 *  need is NULL and the shell skips it.
 *
 *  Two hooks here are not in §1.4's paper list — `init` and `screen` — and
 *  both exist only because these entries are compiled in. §1.4's `activate`
 *  doubles as the build step because a VM-tier app builds its widgets from the
 *  container it was handed; a compiled app's widgets are built by its own
 *  *_init() on the boot ladder, where main.c can order the clock's worker task
 *  before anything that needs it. `screen` is §1.4's activation returning its
 *  owning obj, spelled out for the tier that already has one. Both disappear
 *  from the OS's view when the app arrives on the card; neither may be used to
 *  decide anything about scheduling, arming or residency. */
typedef struct {
    /** Build this app's widgets. Called once, on the boot ladder, in table
     *  order, before the LVGL task exists. Returns the reason it cannot run;
     *  the shell logs it and leaves the entry disabled rather than booting
     *  into a half-built dial. */
    esp_err_t (*init)(orb_app_t *app);
    /** Prepare to be the visible app, before its screen is loaded: claim its
     *  secondary art bank, restore NVS-backed model into statics. Returns the
     *  reason it cannot, in which case the shell leaves the dial on whatever
     *  was showing. Must not rebuild widgets activate has built before — M1's
     *  second-switch-under-60-ms pass line depends on that. */
    esp_err_t (*activate)(orb_app_t *app);
    /** The app's LVGL screen, for lv_screen_load(). Compiled tier only. */
    struct _lv_obj_t *(*screen)(orb_app_t *app);
    /** Park the tick and release the secondary art bank, so the app stops
     *  costing flush work while it is off the glass. Widgets stay built. */
    void      (*suspend)(orb_app_t *app);
    /** Restart the tick plus a clean repaint, for a reveal that did not go
     *  through activate because the app never suspended (C7). */
    void      (*resume)(orb_app_t *app);
    /** Re-push the app's model into its widgets, after the screen load. LVGL
     *  invalidates on load, and the picker and the menu keep their values in
     *  module statics rather than in the widget tree, so without this a trip
     *  to the dial and back reverts the chosen color to the last frame. */
    void      (*show)(orb_app_t *app);
    /** One poll's worth of knob input, while this app owns the knob. */
    void      (*input)(orb_app_t *app, int32_t delta, bool pressed);
    /** True while the app owns the turn rather than the shell's carousel. */
    bool      (*armed)(const orb_app_t *app);
    void      (*set_armed)(orb_app_t *app, bool armed);
    /** Reap finished worker jobs. Runs even while suspended (C7). */
    void      (*service)(orb_app_t *app);

    /* Options, one face per app (B1/B2). The shell renders the page and owns
     * the list; the app answers the rows. Not wired until the opt_* hooks
     * render through the shell — the P2 line in §8 — so the clock's settings
     * tiles are still painted by its own view code today. */
    int          (*opt_count)(orb_app_t *app);
    const char *(*opt_label)(orb_app_t *app, int idx);
    const char *(*opt_value)(orb_app_t *app, int idx);
    void         (*opt_inc)(orb_app_t *app, int idx, int32_t steps);
    void         (*opt_sel)(orb_app_t *app, int idx);
    void         (*opt_arm)(orb_app_t *app, bool armed);
} orb_app_iface_t;

/** How a table entry participates in the knob's carousel. */
typedef enum {
    ORB_APP_KIND_APP = 0,   /* a registered app: clock, color_picker, ... */
    ORB_APP_KIND_SHELL      /* OS chrome (the options page). Walks in the
                             * carousel, is not an app, cannot be uninstalled,
                             * and owns no art. */
} orb_app_kind_t;

struct orb_app {
    const char         *slug;      /* wire identity: ?to=<slug>, NVS key, dir name */
    const char         *alias;     /* one older name still accepted on the wire,
                                    * or NULL. "hue" is the color picker's: the
                                    * bench scripts and the PWA pages were written
                                    * against it, and a registry that breaks them
                                    * for a tidier spelling has cost more than it
                                    * saved. Matched only after the exact slug. */
    const char         *title;     /* what a page header shows */
    orb_app_kind_t      kind;
    bool                is_home;   /* C8: the long-press returns here */

    /* Art coordinates inside the `assets` partition. Both zero means the app
     * is vector-only and claims no bank — color_picker's case. A non-zero
     * size is claimed through art_bank_map_extra() at activate and released
     * through art_bank_unmap_extra() at suspend, and the compositor reads it
     * through art_bank_extra_ptr(app->art_slot). Offset and size are 64 KB
     * aligned because the MMU page is, and the reservation budgeting them is
     * booked at install time with orb_art_reserve(). */
    uint32_t            art_offset;
    uint32_t            art_bytes;
    int                 art_slot;  /* the secondary slot claimed; -1 = none */

    const orb_app_iface_t *iface;

    /* Runtime state, written on the LVGL task only. P4 moves enabled/failed
     * into NVS beside the catalog; they live here now so the shell's read is
     * the same field either way. */
    bool                installed;
    bool                enabled;
    bool                active;
    uint8_t             fault_count;   /* C2: two consecutive faults mark failed */
    bool                failed;
};

#define ORB_APP_CAP 8   /* B5: cap 8 installed, hard, no repartition */

/** Build every enabled app in table order. Call after display_init and before
 *  input_router_init: the clock must exist before the router loads it. */
esp_err_t orb_app_init_all(void);

/** Table access. Index order IS the knob's carousel order. */
size_t            orb_app_count(void);
const orb_app_t  *orb_app_at(size_t idx);
/** Slug lookup for the shell and for GET /api/v1/screen. NULL means no such
 *  app, and the caller refuses with a named reason rather than guessing a
 *  slot. Matches the exact slug and any registered alias, never a
 *  caller-supplied integer. */
const orb_app_t  *orb_app_find_slug(const char *slug);
/** The C8 homing screen: the entry flagged is_home (the clock), never an index
 *  a caller can get wrong. */
const orb_app_t  *orb_app_home(void);
/** The entry on the glass; NULL before the first activate. */
const orb_app_t  *orb_app_current(void);
/** Index of an entry, for the carousel's wraparound; -1 if not in the table. */
int               orb_app_index_of(const orb_app_t *app);

/** The one way to change what the glass shows: suspends the outgoing entry,
 *  activates the incoming one, loads its screen. Call on the LVGL task only —
 *  lv_screen_load() may not be called elsewhere. */
esp_err_t orb_app_activate(const orb_app_t *app);

/** Arm/disarm an entry; the shell's long-press disarms everything first. */
void orb_app_set_armed(const orb_app_t *app, bool armed);
/** Arm the entry on the glass. False means it has no armed view at all — the
 *  dial's case — and the router stays silent instead of logging an arming that
 *  did not happen. */
bool orb_app_arm_current(void);
/** Disarm every walkable entry at once, so nothing can be left armed
 *  off-screen and steal the next turn from the view that is showing. */
void orb_app_disarm_all(void);
/** The carousel's next stop from `from` in direction `step` (+1/-1), skipping
 *  anything unwalkable and wrapping. NULL when nothing can be shown. `from`
 *  NULL means the first walkable entry, which is the dial. */
const orb_app_t *orb_app_neighbour(const orb_app_t *from, int step);
/** True while the entry on the glass owns the turn. */
bool orb_app_current_armed(void);
/** Deliver one poll's worth of knob events to the entry on the glass. */
void orb_app_input(int32_t delta, bool pressed);
/** Reap every app's worker jobs, active or not (C7). Call every poll. */
void orb_app_service_all(void);

#ifdef __cplusplus
}
#endif
