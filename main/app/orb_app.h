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

/** What an options row IS, for the shell's one level of nesting (ruling D12,
 *  2026-10-12; §1.4's frozen names). The app owns this answer — the shell owns
 *  what to do with it: which rows to draw, where the cursor may go, and whether
 *  a group is open.
 *
 *  Contiguous-section convention: an OPT_ITEM belongs to the nearest preceding
 *  OPT_GROUP, and GROUPs are top-level only — an ITEM may never sit inside an
 *  ITEM. A page whose shape breaks that renders the offending row as a plain
 *  OPT_ROW, because a row that is silently invisible is the "knob appears not to
 *  work" failure in list form. */
typedef enum {
    OPT_ROW = 0,    /* an ordinary row: press it, spin it */
    OPT_GROUP,      /* a header that opens or closes the ITEMS below it */
    OPT_ITEM        /* a row inside the nearest preceding OPT_GROUP */
} orb_opt_kind_t;

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
    /** What KIND of row this is, for the shell's one level of nesting (D12).
     *  NULL — the entire pre-D12 ABI, and every app that has not adopted
     *  sub-menus — means every row is OPT_ROW and the page is flat. */
    orb_opt_kind_t (*opt_kind)(orb_app_t *app, int idx);
    /** A long press arrived while this entry owned the knob. True means "I took
     *  it", and the router then does NOT home — ruling D12's one exception to
     *  C8, for closing an open option group without losing the page. NULL, the
     *  answer of every entry with nothing nested to collapse, takes nothing. */
    bool         (*opt_long)(orb_app_t *app);
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

/* ---- options, one face per app (B1/B2) ------------------------------------
 *
 * The shell renders the page and owns the list; the app answers the rows. This
 * is the only way any of those hooks may be called: the shell goes through
 * these, so a NULL hook is a skipped row rather than a crash, and no caller
 * outside the registry ever reads iface->opt_* itself.
 *
 * The row model is deliberately small. The cursor, the clamp, the window of
 * visible rows, the highlight and the hint all belong to the shell — they are
 * the same on every page, and an app that decided them would drift from the
 * one place the knob's feel is set. What belongs to the app is only what each
 * row says and what acting on it does.
 *
 * Every call runs on the LVGL task, from the router's poll. An answer must
 * therefore be a read of state the app already has: opt_label() and opt_value()
 * may not touch the card, the network or NVS, because the same callback is the
 * dial's animation. Anything that has to block goes through orb_jobs_post() and
 * lands in the app's own copy, which its service() hook reaps. */

/** How many rows this entry offers. 0 when it declares none, which is what
 *  keeps an app out of the shell's top level without a second flag. */
int orb_app_opt_count(const orb_app_t *app);
/** The row's name, and its value rendered after it. Both must outlive the
 *  call: they are handed to a label, not copied. */
const char *orb_app_opt_label(const orb_app_t *app, int idx);
const char *orb_app_opt_value(const orb_app_t *app, int idx);
/** What the row at idx IS (D12). A NULL hook answers OPT_ROW for every row, which
 *  is what keeps every pre-D12 answer — including color_picker's, which declares
 *  no options at all — the flat page it always was. An answer that is not one of
 *  the three kinds is OPT_ROW too: an app that returns garbage gets a plain row,
 *  not a page that hides rows the owner cannot reach. */
orb_opt_kind_t orb_app_opt_kind(const orb_app_t *app, int idx);
/** Rotate `steps` detents on row idx, and press it. */
void orb_app_opt_inc(const orb_app_t *app, int idx, int32_t steps);
void orb_app_opt_sel(const orb_app_t *app, int idx);
/** Gate the knob like a screen: armed means the app is being edited, not just
 *  displayed. The shell calls it on entry and on leave, including on the long
 *  press that goes home, so nothing can be left armed off-screen. */
void orb_app_opt_arm(const orb_app_t *app, bool armed);
/** Offer a long press to the entry that owns the knob, before the router spends
 *  it on going home (C8). True means the entry took it — D12: an open option
 *  group closed and the page stayed up — and the router must not home. Called on
 *  the LVGL task, from the router's poll, so like every other answer here it may
 *  not block. */
bool orb_app_opt_long(const orb_app_t *app);
/** True while this entry has anything to show on the options page. */
bool orb_app_has_options(const orb_app_t *app);

#ifdef __cplusplus
}
#endif
