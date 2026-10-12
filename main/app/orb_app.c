/**
 * orb_app.c — the app registry (docs/app-platform-plan.md §1.4).
 *
 * Three entries live here today: the clock (app #0, the C8 homing screen), the
 * color picker (app #1, the first non-clock app and the cheap proof the ABI
 * works), and the shell's own options page — which walks in the carousel but is
 * OS chrome, not an app, and so is tagged ORB_APP_KIND_SHELL rather than being
 * faked into apphood.
 *
 * The build order is the order main.c used to hardcode: clock first, then the
 * picker, then the menu. That order is load-bearing, not incidental — the clock
 * builds the dial that must exist before the router can home to it, and the
 * menu's pages are built after the display exists. The one blocking-work task
 * is the shell's orb_jobs, started from main.c rather than by any app, so no
 * entry can quietly grow a second stack out of the internal heap that the
 * framebuffer also spends. orb_app_init_all() walks the table in order, so the
 * table's order IS the boot order and nobody has to remember it in two places.
 *
 * Hook calling order on a switch is the order input_router.c used to run
 * inline, and it is deliberately kept: pre-load visibility, then
 * lv_screen_load(), then the post-load re-sync. clock_view_set_bounce() before
 * the load is what parks or starts the tick against the RIGHT view, and the
 * picker's and the menu's *_show() after it is what re-pushes their values from
 * module statics into widgets lv_screen_load() just invalidated.
 */

#include "orb_app.h"

#include <string.h>

#include "clock_view.h"
#include "clock_themes.h"
#include "color_picker.h"
#include "menu_view.h"
#include "art_bank.h"

#include "esp_log.h"
#include "lvgl.h"      /* lv_screen_load(): the one widget call the registry makes */

static const char *TAG = "app";

/* ---- per-app state, behind the ABI the shell calls ----------------------- */

/* The clock owns the primary art bank, which art_bank_init() mapped at boot
 * and which nothing unmaps: clock_view.c's image descriptors cache raw
 * pointers into it, so its lifetime outlasts every screen. An app that owns a
 * SECONDARY bank sets art_bytes below, and the map/unmap pair in
 * orb_app_activate()/suspend() is what claims and releases that. */
static esp_err_t clock_activate(orb_app_t *app)
{
    (void)app;
    /* The dial's animation beat is the only thing that costs flush work on
     * every tick, so it runs only while the clock is on the glass. This is
     * today's clock_view_set_bounce(true), reached pre-load exactly where
     * input_router.c called it: the outgoing app is suspended first, so a
     * switch away and a switch back cannot both leave a beat running. */
    clock_view_set_bounce(true);
    return ESP_OK;
}

static void clock_suspend(orb_app_t *app)
{
    (void)app;
    clock_view_set_bounce(false);
}

/* Reveal the dial again without rebuilding it: this is the hook a refused
 * switch uses, and the tick has to come back with it. Leaving it parked would
 * mean a dial that stopped animating because an app it never showed could not
 * fit in the page window — a bug nobody would trace back to here. */
static void clock_resume(orb_app_t *app)
{
    (void)app;
    clock_view_set_bounce(true);
}

static struct _lv_obj_t *clock_screen(orb_app_t *app)
{
    (void)app;
    return clock_view_screen();
}

/* The clock's option answers live in clock_themes.c, not here: B2 rules that the
 * clock app owns the theme picker and the OS only renders the rows it is handed,
 * so the registry's job is to point at them and nothing more. */
static esp_err_t clock_init(orb_app_t *app)
{
    (void)app;
    if (clock_themes_init() != ESP_OK) return ESP_ERR_NO_MEM;
    return clock_view_init();
}

static void clock_service(orb_app_t *app)
{
    (void)app;
    /* Reaps the card walk and the staging verify whether or not the dial is on
     * the glass: a scan that finishes while the owner has rotated away must still
     * land, or the page shows a stale answer until they come back. Repainting is
     * the shell's, on the LVGL task, from these results. */
    clock_themes_service();
}

static const orb_app_iface_t s_clock_iface = {
    .init        = clock_init,
    .activate    = clock_activate,
    .suspend     = clock_suspend,
    .resume      = clock_resume,
    .service     = clock_service,
    .screen      = clock_screen,
    /* No armed view and no input: the dial itself stays inert by design, so a
     * press on the clock means nothing rather than arming a list that is not
     * there. The options are the exception and the whole of B1/B2 — the shell
     * renders this page, the clock answers it. */
    .opt_count   = clock_opt_count,
    .opt_label   = clock_opt_label,
    .opt_value   = clock_opt_value,
    .opt_inc     = clock_opt_inc,
    .opt_sel     = clock_opt_sel,
    .opt_arm     = clock_opt_arm,
};

static esp_err_t picker_init(orb_app_t *app)   { (void)app; return color_picker_init(); }
static void      picker_show(orb_app_t *app)   { (void)app; color_picker_show(); }
static void      picker_input(orb_app_t *app, int32_t d, bool p)
{
    (void)app;
    color_picker_apply_input(d, p);
}
static bool      picker_armed(const orb_app_t *app)
{
    (void)app;
    return color_picker_armed();
}
static void      picker_set_armed(orb_app_t *app, bool a)
{
    (void)app;
    color_picker_set_armed(a);
}
static struct _lv_obj_t *picker_screen(orb_app_t *app)
{
    (void)app;
    return color_picker_screen();
}

static const orb_app_iface_t s_picker_iface = {
    .init       = picker_init,
    .show       = picker_show,
    .input      = picker_input,
    .armed      = picker_armed,
    .set_armed  = picker_set_armed,
    .screen     = picker_screen,
};

static esp_err_t menu_init(orb_app_t *app)   { (void)app; return menu_view_init(); }
static void      menu_show(orb_app_t *app)   { (void)app; menu_view_show(); }
static void      menu_input(orb_app_t *app, int32_t d, bool p)
{
    (void)app;
    menu_view_apply_input(d, p);
}
static bool      menu_armed(const orb_app_t *app)
{
    (void)app;
    return menu_view_armed();
}
static void      menu_set_armed(orb_app_t *app, bool a)
{
    (void)app;
    menu_view_set_armed(a);
}
static void      menu_service(orb_app_t *app)
{
    (void)app;
    /* Reaps the card walk and the staging verify. Runs whether or not the menu
     * is visible: a scan that finishes while the user has rotated to the dial
     * must still land, or the list shows a stale answer until they come back. */
    menu_view_service();
}
static struct _lv_obj_t *menu_screen(orb_app_t *app)
{
    (void)app;
    return menu_view_screen();
}

static const orb_app_iface_t s_menu_iface = {
    .init       = menu_init,
    .show       = menu_show,
    .input      = menu_input,
    .armed      = menu_armed,
    .set_armed  = menu_set_armed,
    .service    = menu_service,
    .screen     = menu_screen,
};

/* ---- the table ----------------------------------------------------------- */

/* Index order is the knob's carousel order, and the boot order. The plan's P2
 * pass line wants "clock -> color_picker -> clock", so the dial leads and the
 * shell's page trails: turning from the dial lands on the picker, which is the
 * app that proves the ABI, rather than on the OS menu as it did while the
 * switch statement hardcoded MENU first. Everything else about the two-level
 * model is untouched.
 *
 * The clock's art coordinates are zero because its bank is the PRIMARY mapping:
 * orb_theme_boot() picked the slot and art_bank_init() mapped it before
 * display_init(), and clock_view.c reads it through the base that mapping hands
 * out. Only a SECONDARY bank goes through the fields below. */
static orb_app_t s_apps[] = {
    {
        .slug       = "clock",
        .alias      = NULL,
        .title      = "Clock",
        .kind       = ORB_APP_KIND_APP,
        .is_home    = true,          /* C8: long-press always returns here */
        .art_offset = 0,
        .art_bytes  = 0,             /* primary bank, mapped at boot */
        .art_slot   = -1,
        .iface      = &s_clock_iface,
        .installed  = true,
        .enabled    = true,
    },
    {
        .slug       = "color_picker",
        .alias      = "hue",         /* what /api/v1/screen and the PWA call it */
        .title      = "HSB",
        .kind       = ORB_APP_KIND_APP,
        .is_home    = false,
        .art_offset = 0,
        .art_bytes  = 0,             /* vector-only: LVGL draws the whole face */
        .art_slot   = -1,
        .iface      = &s_picker_iface,
        .installed  = true,
        .enabled    = true,
    },
    {
        .slug       = "menu",
        .alias      = NULL,
        .title      = "Menu",
        .kind       = ORB_APP_KIND_SHELL,
        .is_home    = false,
        .art_offset = 0,
        .art_bytes  = 0,
        .art_slot   = -1,
        .iface      = &s_menu_iface,
        .installed  = true,
        .enabled    = true,
    },
};

#define ORB_APP_N (sizeof(s_apps) / sizeof(s_apps[0]))

static orb_app_t *s_current;
/* ---- the API the shell calls --------------------------------------------- */

size_t orb_app_count(void)
{
    /* Only walkable entries count: a disabled app stays in the table so the
     * APPS page can still show it with its reason, but the carousel must not
     * stop on a blank screen. The count is computed, not cached, because
     * P4's enable/disable flips these flags at runtime and a cached count is
     * one more thing to forget to update. */
    size_t n = 0;
    for (size_t i = 0; i < ORB_APP_N; i++)
        if (s_apps[i].enabled && !s_apps[i].failed) n++;
    return n;
}

const orb_app_t *orb_app_at(size_t idx)
{
    /* The carousel's accessor, so idx walks the WALKABLE list, not the array:
     * asking for slot 2 of a table whose second entry is disabled must not
     * hand back a screen nobody can leave. */
    if (idx >= ORB_APP_N) return NULL;
    if (!s_apps[idx].enabled || s_apps[idx].failed) return NULL;
    return &s_apps[idx];
}

const orb_app_t *orb_app_find_slug(const char *slug)
{
    if (!slug || !*slug) return NULL;
    /* Exact slug first, everywhere, before any alias: two entries must never
     * disagree about which one an old name means, and an alias is a
     * compatibility concession, not a second identity. */
    for (size_t i = 0; i < ORB_APP_N; i++)
        if (strcmp(s_apps[i].slug, slug) == 0)
            return &s_apps[i];
    for (size_t i = 0; i < ORB_APP_N; i++)
        if (s_apps[i].alias && strcmp(s_apps[i].alias, slug) == 0)
            return &s_apps[i];
    return NULL;
}

const orb_app_t *orb_app_home(void)
{
    for (size_t i = 0; i < ORB_APP_N; i++)
        if (s_apps[i].is_home) return &s_apps[i];
    return NULL;   /* a table with no home is a build error, not a runtime one */
}

const orb_app_t *orb_app_current(void) { return s_current; }

int orb_app_index_of(const orb_app_t *app)
{
    if (!app) return -1;
    for (size_t i = 0; i < ORB_APP_N; i++)
        if (&s_apps[i] == app) return (int)i;
    return -1;
}

esp_err_t orb_app_init_all(void)
{
    /* Table order is boot order, which is what main.c used to hardcode and
     * what still matters: the clock's init creates the worker task the theme
     * path needs, and it must exist before anything posts to it. */
    for (size_t i = 0; i < ORB_APP_N; i++) {
        orb_app_t *app = &s_apps[i];
        if (!app->enabled || app->failed) continue;
        if (!app->iface || !app->iface->init || !app->iface->screen) {
            ESP_LOGE(TAG, "%s: no init/screen hook — disabled", app->slug);
            app->enabled = false;
            continue;
        }
        /* No alignment check here on purpose: art_bank_map_extra() is the one
         * thing that knows what the MMU will accept, and it refuses a ragged
         * offset or size with ESP_ERR_INVALID_SIZE. A second validator in the
         * registry would disagree with the loader the first time the page size
         * changed, and the app would lose its art silently. */
        const esp_err_t err = app->iface->init(app);
        if (err != ESP_OK) {
            /* A dial that cannot build one screen still tells the time: the
             * entry is taken out of the carousel with its reason logged, and
             * the boot continues. ESP_ERROR_CHECK here would turn a broken
             * app into a bricked device. */
            ESP_LOGE(TAG, "%s: init failed (%s) — dropped from the carousel",
                     app->slug, esp_err_to_name(err));
            app->enabled = false;
            app->failed  = true;
            continue;
        }
        app->installed = true;
    }
    ESP_LOGI(TAG, "%u app(s) built, %d walkable",
             (unsigned)ORB_APP_N, (int)orb_app_count());
    return ESP_OK;
}

/** Claim or release this app's secondary art bank, if it has one.
 *
 *  An app with no bank (art_bytes 0) costs the page window nothing and is the
 *  common case: color_picker is LVGL-drawn, and the carousel is text under
 *  ruling C5. A banked app is mapped here on activation and released on
 *  suspend, which is the whole point of the secondary slots — a parked app
 *  must not hold MMU pages another app is being refused for.
 *
 *  A failed claim is a refusal to switch, not a switch into garbage:
 *  art_bank_extra_ptr() returning NULL and a compositor reading it anyway is
 *  the silent garbage paint art_bank.h fences off, and a dial that shows a
 *  corrupt frame is worse than a dial that stays on the clock. */
static esp_err_t app_art_claim(orb_app_t *app)
{
    if (app->art_bytes == 0) return ESP_OK;
    if (app->art_slot >= 0)  return ESP_OK;   /* already resident; re-mapping
                                               * would bill a second region */
    int slot = -1;
    const esp_err_t err = art_bank_map_extra(app->art_offset, app->art_bytes, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: art bank refused (%s) at offset %lu — not activated",
                 app->slug, esp_err_to_name(err), (unsigned long)app->art_offset);
        return err;
    }
    app->art_slot = slot;
    return ESP_OK;
}

static void app_art_release(orb_app_t *app)
{
    if (app->art_slot < 0) return;
    /* Anything the app cached out of this mapping must be gone already: the
     * compositor holds that lifetime, exactly as clock_view.c holds the
     * primary's never-unmap promise. */
    const esp_err_t err = art_bank_unmap_extra(app->art_slot);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "%s: secondary bank %d refused to unmap (%s)",
                 app->slug, app->art_slot, esp_err_to_name(err));
    app->art_slot = -1;
}

/** Put the app that never left the glass back after a switch was refused:
 *  re-claim the bank its suspend released, and restart what it parked. The
 *  screen itself was never swapped, so there is nothing to load. `resume` is
 *  the hook for exactly this (C7); `activate` is the fallback for a compiled
 *  app that only spells its visibility in the pre-load hook. */
static void app_restore(orb_app_t *app)
{
    const esp_err_t back = app_art_claim(app);
    if (back != ESP_OK)
        /* The budget that refused the incoming bank is the same budget, so this
         * can fail. An app on the glass with no bank reads NULL from
         * art_bank_extra_ptr(), which is the silent garbage paint art_bank.h
         * fences off — say it loudly rather than letting the dial report it. */
        ESP_LOGE(TAG, "%s: could not restore its art bank (%s) — "
                     "the dial may paint without its pixels",
                 app->slug, esp_err_to_name(back));
    if (app->iface->resume) app->iface->resume(app);
    else if (app->iface->activate) app->iface->activate(app);
}

esp_err_t orb_app_activate(const orb_app_t *app_cp)
{
    /* The table is this module's own storage, so the const on the way in is a
     * promise to callers, not a limit on the registry: only these functions
     * write the state fields, and only from the LVGL task. */
    orb_app_t *app = (orb_app_t *)app_cp;
    if (orb_app_index_of(app) < 0) return ESP_ERR_INVALID_ARG;
    if (app->failed)  return ESP_ERR_INVALID_STATE;
    if (!app->enabled) return ESP_ERR_NOT_SUPPORTED;

    orb_app_t *old = s_current;
    if (old == app) {
        /* Re-revealing the app already on the glass still runs the hooks: the
         * long-press on the dial is exactly this case, and today it forces a
         * clean repaint through clock_view_set_bounce(true). Skipping the work
         * because "it is already showing" is how a held knob stops meaning
         * anything. */
        app_art_claim(app);
        if (app->iface->activate) app->iface->activate(app);
        struct _lv_obj_t *scr = app->iface->screen ? app->iface->screen(app) : NULL;
        if (scr) lv_screen_load(scr);
        if (app->iface->show) app->iface->show(app);
        return ESP_OK;
    }

    /* Out first, then the new bank, then the new view. Parking the outgoing
     * app before the claim matters: if two banked apps trade places, releasing
     * nothing first can push the second over the page budget the first is
     * still holding. */
    if (old && old->iface->suspend) old->iface->suspend(old);
    if (old) app_art_release(old);

    const esp_err_t art_err = app_art_claim(app);
    if (art_err != ESP_OK) {
        /* Put back what we took, or the dial is left showing a view whose tick has
         * been parked and whose art is gone. */
        if (old) app_restore(old);
        return art_err;
    }

    if (app->iface->activate) {
        const esp_err_t err = app->iface->activate(app);
        if (err != ESP_OK) {
            app_art_release(app);
            ESP_LOGE(TAG, "%s: activate refused (%s) — staying on %s",
                     app->slug, esp_err_to_name(err), old ? old->slug : "nothing");
            if (old) app_restore(old);
            return err;
        }
    }

    struct _lv_obj_t *scr = app->iface->screen ? app->iface->screen(app) : NULL;
    if (!scr) {
        /* Same rule as the two refusals above: the outgoing app was suspended
         * already, so leaving it parked while it is still the screen on the
         * glass is a frozen dial, not a refusal. */
        app_art_release(app);
        if (old) app_restore(old);
        ESP_LOGE(TAG, "%s: no screen to load — leaving the dial where it was", app->slug);
        return ESP_ERR_NOT_FOUND;
    }
    lv_screen_load(scr);
    /* After the load, deliberately: lv_screen_load() invalidates the whole
     * screen, and this is what pushes the module-static model back into the
     * widgets so a value survives a trip away and back. */
    if (app->iface->show) app->iface->show(app);

    if (old) old->active = false;
    app->active   = true;
    s_current     = app;
    return ESP_OK;
}

void orb_app_set_armed(const orb_app_t *app, bool armed)
{
    if (app && app->iface->set_armed) app->iface->set_armed((orb_app_t *)app, armed);
}

bool orb_app_arm_current(void)
{
    /* False means the entry on the glass has no armed view at all — the dial's
     * case. The router logs the arming only when it happened, because a press
     * that silently did nothing is exactly the "knob appears not to work"
     * failure clock_view's inertness is designed to avoid. */
    if (!s_current || !s_current->iface->set_armed) return false;
    s_current->iface->set_armed(s_current, true);
    return true;
}

bool orb_app_current_armed(void)
{
    if (!s_current || !s_current->iface->armed) return false;
    return s_current->iface->armed(s_current);
}

void orb_app_input(int32_t delta, bool pressed)
{
    if (s_current && s_current->iface->input)
        s_current->iface->input(s_current, delta, pressed);
}

void orb_app_service_all(void)
{
    /* Every enabled entry, not just the visible one: C7 keeps a parked app's
     * worker jobs reaping, so a refresh posted before a rotation still lands.
     * Repainting from here would be the cadence collapse the fps findings
     * measured, which is why nothing in this loop loads a screen. */
    for (size_t i = 0; i < ORB_APP_N; i++) {
        orb_app_t *app = &s_apps[i];
        if (!app->enabled || app->failed) continue;
        if (app->iface->service) app->iface->service(app);
    }
}

void orb_app_disarm_all(void)
{
    /* Every entry is disarmed together rather than only the one leaving, so an
     * app can never be left armed off-screen and then steal the next turn from
     * the view actually showing — the failure input_router.c guarded against by
     * hand when it named the menu and the picker in disarm_all(). */
    for (size_t i = 0; i < ORB_APP_N; i++) {
        orb_app_t *app = &s_apps[i];
        if (!app->enabled || app->failed) continue;
        if (app->iface->set_armed) app->iface->set_armed(app, false);
    }
}

const orb_app_t *orb_app_neighbour(const orb_app_t *from, int step)
{
    /* The carousel's wraparound, and the reason it lives here: the answer
     * depends on which entries are walkable, which is the table's business and
     * changes at runtime when P4 disables an app. `from` NULL means the first
     * entry, which is what the boot ask wants — the dial, since the table leads
     * with it.
     *
     * One step per call, in the direction turned: a fast burst reports several
     * detents and must not fly past every screen and stop somewhere nobody
     * aimed at, which is why input_router feeds this one step per poll. */
    const size_t n = ORB_APP_N;
    int i = orb_app_index_of(from);
    if (i < 0) {
        /* Nothing visible yet: the first walkable entry, from the top. */
        for (size_t k = 0; k < n; k++)
            if (s_apps[k].enabled && !s_apps[k].failed) return &s_apps[k];
        return NULL;
    }
    /* Skip over anything unwalkable, but never loop forever: at most n hops in
     * the given direction, so a table of one walkable entry returns itself and
     * a table of none returns NULL rather than spinning the LVGL task. */
    for (size_t hops = 0; hops < n; hops++) {
        i = (int)(((long)i + step) % (long)n);
        if (i < 0) i += (int)n;
        if (s_apps[i].enabled && !s_apps[i].failed) return &s_apps[i];
    }
    return NULL;
}

/* ---- options, one face per app (B1/B2) ------------------------------------
 *
 * The one place the opt_* hooks are read. Each dispatches only if the entry
 * supplies the hook, which is what lets an app declare two rows and another
 * declare none without the shell carrying a case for each.
 *
 * Bounds and NULL are decided here rather than in the shell: a hook that
 * returns a row index outside what it counted is the app's bug, and the shell
 * must still show something coherent. So the count is re-read on every call
 * rather than cached — the clock's number changes when a card walk finishes and
 * an options page that had five rows cannot be allowed to press the sixth. */

int orb_app_opt_count(const orb_app_t *app)
{
    if (!app || !app->enabled || app->failed) return 0;
    if (!app->iface || !app->iface->opt_count) return 0;
    const int n = app->iface->opt_count((orb_app_t *)app);
    return n < 0 ? 0 : n;
}

const char *orb_app_opt_label(const orb_app_t *app, int idx)
{
    if (!app || !app->iface || !app->iface->opt_label) return "";
    if (idx < 0 || idx >= orb_app_opt_count(app)) return "";
    const char *s = app->iface->opt_label((orb_app_t *)app, idx);
    return s ? s : "";
}

const char *orb_app_opt_value(const orb_app_t *app, int idx)
{
    if (!app || !app->iface || !app->iface->opt_value) return "";
    if (idx < 0 || idx >= orb_app_opt_count(app)) return "";
    const char *s = app->iface->opt_value((orb_app_t *)app, idx);
    return s ? s : "";
}

void orb_app_opt_inc(const orb_app_t *app, int idx, int32_t steps)
{
    if (!app || !app->iface || !app->iface->opt_inc) return;
    if (idx < 0 || idx >= orb_app_opt_count(app)) return;
    app->iface->opt_inc((orb_app_t *)app, idx, steps);
}

void orb_app_opt_sel(const orb_app_t *app, int idx)
{
    if (!app || !app->iface || !app->iface->opt_sel) return;
    if (idx < 0 || idx >= orb_app_opt_count(app)) return;
    app->iface->opt_sel((orb_app_t *)app, idx);
}

void orb_app_opt_arm(const orb_app_t *app, bool armed)
{
    if (!app || !app->iface || !app->iface->opt_arm) return;
    app->iface->opt_arm((orb_app_t *)app, armed);
}

bool orb_app_has_options(const orb_app_t *app)
{
    return orb_app_opt_count(app) > 0;
}
