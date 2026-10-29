/**
 * orb_app.c — the app registry (docs/app-platform-plan.md §1.4).
 *
 * One entry lives here since D35 (§35): the shell's own options page — which
 * walks in the carousel but is OS chrome, not an app, tagged
 * ORB_APP_KIND_SHELL, and owns is_home: the long press lands on the menu. The
 * clock and the color picker used to be compiled rows; D35 made them card
 * rows, and D36 (§0.11) moved their containers out of the image and onto the
 * hub's seed: the hub writes the two .orbapp files to the card, and the
 * ordinary install doors do the rest — the same toggles, the same uninstall
 * door and the same C2 quarantine as any app a card brings. A row that can be
 * removed is a row the owner owns.
 *
 * Build order: the shell first (the one compiled row), then the card tier in
 * slot order. The hub seeds the dial first, so the clock leads every installed
 * app. The shared
 * blocking-work task is the shell's orb_jobs, started from main.c before this
 * ladder rather than by any app, so no entry can quietly grow a second stack
 * out of the internal heap the framebuffer also spends and nothing can post to
 * a queue that does not exist yet.
 *
 * Hook calling order on a switch is the order input_router.c used to run
 * inline, and it is deliberately kept: pre-load visibility, then
 * lv_screen_load(), then the post-load re-sync. clock_view_set_bounce() before
 * the load is what parks or starts the tick against the RIGHT view, and the
 * picker's and the menu's *_show() after it is what re-pushes their values from
 * module statics into widgets lv_screen_load() just invalidated.
 */

#include "orb_app.h"

#include <stdio.h>      /* card_fill's snprintf into the row's own buffers */
#include <string.h>

#include "orb_app_wasm.h"   /* P7.2: orb_wasm_iface, registered below as a behaviour module */

#include "esp_heap_caps.h"   /* the card table lives in PSRAM, not internal .bss */

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
    if (clock_themes_init() != ESP_OK) return ESP_ERR_NO_MEM;
    return clock_view_init(app);
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
    .opt_kind    = clock_opt_kind,
    .opt_inc     = clock_opt_inc,
    .opt_sel     = clock_opt_sel,
    .opt_arm     = clock_opt_arm,
    .opt_focus   = clock_opt_focus,
    .opt_preview = clock_opt_preview,
};

static esp_err_t picker_init(orb_app_t *app)
{
    /* D35: the row owns the picker's settings; bind it before the screen builds
     * so the first frame shows the stored color, not the static default. */
    const esp_err_t e = color_picker_init();
    if (e == ESP_OK) color_picker_bind_app(app);
    return e;
}
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
static bool menu_long(orb_app_t *app)
{
    (void)app;
    return menu_view_opt_long();
}

static const orb_app_iface_t s_menu_iface = {
    .init       = menu_init,
    .show       = menu_show,
    .input      = menu_input,
    .armed      = menu_armed,
    .set_armed  = menu_set_armed,
    .service    = menu_service,
    .screen     = menu_screen,
    /* D12: the long press is offered to the page that is showing before the
     * router spends it on going home, and the page that can spend it is this one —
     * an app's options are drawn here, so the open group is this file's state and
     * the app never learns it exists. */
    .opt_long   = menu_long,
};

/* ---- the behaviour-module registry (D1 / C6) ------------------------------
 *
 * The one table a declarative `.orbapp` package is matched against. A card row's
 * `behaviour_kind` is an index here; the module it names is the compiled-in OS
 * code that actually draws the app. D1's whole premise is that the pixel path is
 * priced in hardware and cannot be interpreted, so an installed app never carries
 * drawing code — it carries data (slug, title, settings, the caps the manifest
 * declares) plus this one integer, and the interpreter lives in the firmware.
 *
 * Append-only (C6): IDs are assigned forever and never reused. ORB_BEHAVIOUR_NONE
 * (0) is deliberately absent from the table — it is not a module, it means "this
 * entry names its iface directly", which is how every compiled-in app and every
 * unknown-kind card row behaves. The picker registers as the FIRST real module,
 * reopens D15's reserved-zero clause under its own "first real use" allowance, and
 * reuses s_picker_iface verbatim: the installed app and the compiled-in one are
 * the same code interpreting the same container words, which is the point. */
typedef struct {
    uint32_t                  kind;
    const orb_app_iface_t    *iface;
} orb_behaviour_module_t;

static const orb_behaviour_module_t s_behaviour[] = {
    { ORB_BEHAVIOUR_COLOR_PICKER, &s_picker_iface },
    /* D35 (§35): the dial face registers exactly like the picker — one compiled
     * iface behind one kind id. The seed container for "clock" names kind 3,
     * and the row it fills walks, draws and faults like any other card app. */
    { ORB_BEHAVIOUR_CLOCK,        &s_clock_iface  },
    /* P7.2 (D20): the wasm3 code tier. One compiled iface serves every code-bearing
     * .orbapp; orb_app_wasm.c keys per-app state by flash_slot, so two installed wasm
     * apps sharing this iface are two rows over two M3 runtimes — unlike the picker,
     * which is a C6 singleton. The iface's hooks are the guest's entry points. */
    { ORB_BEHAVIOUR_WASM,         &orb_wasm_iface },
};

const orb_app_iface_t *orb_app_behaviour_iface(uint32_t behaviour_kind)
{
    for (size_t i = 0; i < sizeof s_behaviour / sizeof s_behaviour[0]; i++)
        if (s_behaviour[i].kind == behaviour_kind) return s_behaviour[i].iface;
    return NULL;   /* ORB_BEHAVIOUR_NONE and any unregistered id */
}

bool orb_app_behaviour_known(uint32_t behaviour_kind)
{
    return orb_app_behaviour_iface(behaviour_kind) != NULL;
}

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
/* D35 (§35) took the clock and the HSB editor out of this table; D36 (§0.11)
 * took their containers out of the image. They are card rows now — the same
 * compiled ifaces the entries above registered, reached through behaviour kinds
 * 3 and 1, installed into flash slots by the ordinary card installer the way
 * any other card app installs (the hub seeds the files). What is left
 * here is the shell row — the one entry that is chrome, cannot be uninstalled,
 * and cannot be turned off. It owns is_home, so the long press lands on the
 * menu (the amended C8) and no app can ever take the dial's homing away.
 *
 * The caps note that led this table before moved into the two seed
 * containers: the clock now *declares* themes and sd_read, and the picker
 * declares nothing — the honest picture of what each one imports, in the one
 * place D19 says declarations live. */
static orb_app_t s_apps[] = {
    {
        .slug       = "menu",
        .alias      = NULL,
        .title      = "Menu",
        .kind       = ORB_APP_KIND_SHELL,
        .is_home    = true,          /* C8 as amended by D35: long press lands here */
        .art_offset = 0,
        .art_bytes  = 0,
        .art_slot   = -1,
        .flash_slot = -1,          /* chrome: no flash slot, no toggle, no uninstall */
        .iface      = &s_menu_iface,
        .installed  = true,
        .enabled    = true,
    },
};

#define ORB_APP_N (sizeof(s_apps) / sizeof(s_apps[0]))

/* ---- the service tier (D36): settings with no face ------------------------
 *
 * USB Files is an OS feature with a page of options and no screen to walk to.
 * It goes here rather than into a table inside menu_view.c because the menu
 * already asks the registry what exists, and a second source for that question
 * is the bug where the menu lists a row the knob disagrees about.
 *
 * It is deliberately NOT a card row and NOT a fourth behaviour kind. A card
 * row's index keys its grant, fault and enable words in NVS (P4a), so filing an
 * OS feature under slot 1 would hand it the retired clock's words; and a kind
 * id is how a package names the module that interprets ITS data, which is not
 * what is happening — this row carries no data, only code. So it sits outside
 * the ledger span (indices >= ORB_APP_MENU_BASE), is invisible to the walkable
 * gather below (which walks s_apps and s_card and never looks here), and is
 * refused by orb_app_activate() on its own predicate rather than by being
 * absent from a lookup.
 */
#if CONFIG_ORB_USB_MTP || CONFIG_ORB_USB_MSC
/* One row for the feature, exported by whichever mode this build chose —
 * orb_usb_mtp.c or orb_usb_msc.c, never both, since the two are a Kconfig choice.
 * The tier is written as a table rather than a bare reference so the next
 * settings-only OS feature lands in the same list instead of inventing its own. */
extern orb_app_t orb_app_usb_files_row;   /* main/usb/orb_usb_{mtp,msc}.c */
static orb_app_t *const s_service[] = { &orb_app_usb_files_row };
#else
static orb_app_t *const s_service[] = { NULL };   /* the tier, empty */
#endif
#define ORB_APP_SERVICE_N ((int)(sizeof(s_service) / sizeof(s_service[0])))

size_t orb_app_menu_count(void)
{
    size_t n = 0;
    for (int i = 0; i < ORB_APP_SERVICE_N; i++)
        if (s_service[i] && s_service[i]->enabled && !s_service[i]->failed) n++;
    return n;
}

const orb_app_t *orb_app_menu_at(size_t idx)
{
    for (int i = 0; i < ORB_APP_SERVICE_N; i++) {
        if (!s_service[i] || !s_service[i]->enabled || s_service[i]->failed) continue;
        if (idx-- == 0) return s_service[i];
    }
    return NULL;
}

bool orb_app_is_menu_only(const orb_app_t *app)
{
    if (!app) return false;
    for (int i = 0; i < ORB_APP_SERVICE_N; i++)
        if (s_service[i] == app) return true;
    return false;
}

static orb_app_t *s_current;

/* ---- the card table (P4): installed .orbapp apps, beside the compiled shell --
 *
 * A card app draws nothing until its declared behaviour_kind names a
 * compiled-in module (D1/C6): orb_app_behaviour_iface() turns that integer into
 * the orb_app_iface_t that builds its widgets. A qualified card row — used,
 * enabled, not failed, iface non-NULL — walks the carousel exactly like a
 * compiled entry; orb_app_walk_gather() below is the one place that ordering is
 * defined, so neighbour()/count()/service_all()/disarm_all() all agree on the
 * walkable set. A row whose iface is NULL (ORB_BEHAVIOUR_NONE or a kind this
 * firmware build does not register) is NEVER walkable and NEVER activated: it
 * belongs in the ledger only — the status endpoint, the APPS page, orb_app_
 * find_slug() for the ?to= route, and above all the broker's grant words keyed by
 * orb_app_index_of(). Hence a parallel table at indices ORB_APP_BASE.., exactly
 * the slack orb_svc.c reserves (ORB_SVC_ROWS = 3 compiled + 8 apps). The three
 * compiled entries stay the whole of s_apps, so every host registry-test pin on
 * count()/rows()/index_of() is unchanged when nothing is installed.
 *
 * Strings and settings are copied into this entry's OWN buffers on upsert, so a
 * caller may hand a stack orb_app_t whose slug/title/settings point at the manifest
 * bytes it is about to free; the resident row keeps valid pointers after.
 *
 * The table is one PSRAM block, not an internal .bss array. Eight entries each
 * carrying a settings_store[ORB_APP_SETTINGS_MAX] of ~64 B descriptors total ~9 KB,
 * and orb_theme.c and art_bank.c already establish that any block of this size is
 * heap_caps_malloc'd from MALLOC_CAP_SPIRAM here — the device boots with only ~7 KB
 * of largest internal block, so a 9 KB static is not spare it can afford. Readers
 * run the compiled tier even when s_card is NULL (no block = nothing installed);
 * only the writers force the allocation, and a failed allocation is the same answer
 * as an empty table to the ledger. */
typedef struct {
    orb_app_t          app;                 /* the ledger-visible record        */
    bool               used;                /* this card slot holds an install  */
    char               slug_store[ORB_APP_SLUG_MAX];
    char               title_store[ORB_APP_TITLE_MAX];
    /* S6: the container's release stamp, held here for the same reason the two
     * above are — the record on the card must outlive the caller's temporary, and
     * the ledger's rows all point at this table's own bytes. "" is a real answer
     * and not a missing one: it means the package names no release (§4.1). */
    char               version_store[ORB_APP_VERSION_MAX];
    orb_setting_desc_t settings_store[ORB_APP_SETTINGS_MAX];
} card_entry_t;

static card_entry_t *s_card;   /* PSRAM, one block; NULL until a writer needs it */

/* Allocate the card table from PSRAM on first use, zeroed. Idempotent: the writers
 * call it, and the readers only touch s_card once a writer has run. Returns NULL
 * only if the block cannot be had, which leaves the ledger showing the compiled
 * three — the pre-P4 answer — never a half-formed table. */
static card_entry_t *card_table(void)
{
    if (!s_card)
        s_card = heap_caps_calloc(ORB_APP_CAP, sizeof *s_card,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_card;
}

/* Copy row's scalar fields into slot s, re-pointing slug/title/settings at s's
 * own stable buffers. The scalar copy is by value (orb_app_t is small and POD);
 * the three pointers are then fixed up. kind comes from row so a card app is
 * tagged ORB_APP_KIND_APP; is_home is forced false (C8 as amended by D35: homing is
 * the shell row's, and no package can claim it) and
 * iface is DERIVED here from behaviour_kind, never taken from the caller — see the
 * D1/C6 note at the assignment. behaviour_kind itself rides in from row (the slot
 * header), which is the word the dispatch keys on. */
static void card_fill(card_entry_t *s, const orb_app_t *row)
{
    const orb_setting_desc_t *settings = row->settings;
    int n = row->n_settings;
    s->app = *row;                       /* scalars + the caller's pointers      */

    s->slug_store[0] = '\0';
    if (row->slug) snprintf(s->slug_store, sizeof s->slug_store, "%s", row->slug);
    s->app.slug = s->slug_store;

    s->title_store[0] = '\0';
    if (row->title) snprintf(s->title_store, sizeof s->title_store, "%s", row->title);
    s->app.title = s->title_store;

    /* S6: the third re-pointed string, same rule as the two above — and the same
     * empty-is-honest answer. A scan row that never stamped one reads "" here, and
     * the hub prints that row's version as the library's, because the card has no
     * opinion of its own to contradict it. */
    s->version_store[0] = '\0';
    if (row->version)
        snprintf(s->version_store, sizeof s->version_store, "%s", row->version);
    s->app.version = s->version_store;

    if (n > ORB_APP_SETTINGS_MAX) n = ORB_APP_SETTINGS_MAX;
    if (settings && n > 0)
        memcpy(s->settings_store, settings, (size_t)n * sizeof(orb_setting_desc_t));
    s->app.settings     = s->settings_store;
    s->app.n_settings   = n;
    s->app.alias        = NULL;            /* a card app has no legacy name      */
    /* D1/C6: the iface comes from the compiled-in module its behaviour_kind
     * names, NOT from the caller — a card package carries data only. A kind the
     * firmware registers resolves to that module's iface and the app is walkable;
     * ORB_BEHAVIOUR_NONE or an unknown id resolves to NULL and the row stays
     * ledger-only, so nothing can ever draw a screen nobody wrote. The caller's
     * iface field is deliberately ignored here (card_fill's whole reason to
     * re-point the string fields): a stack row from a scan cannot smuggle an
     * iface into the resident record. */
    s->app.iface        = orb_app_behaviour_iface(s->app.behaviour_kind);
    s->app.is_home      = false;           /* home is the shell's (C8 as amended)  */
    s->app.installed    = true;
    s->app.enabled      = true;            /* owner ruling, bench 2026-10-29: installed IS
                                            * enabled. There is no separate on/off state a
                                            * surface can toggle; the enable word survives
                                            * only as the durable off-record the recovery
                                            * door writes, and apply_enable lifts it. */
    s->app.active       = false;
    /* The entry index IS the identity (P4a): stamp it so a row placed by the
     * fallback path still knows its own key, and an upserted row's word keys
     * match orb_app_index_of() without the caller having asked. */
    s->app.flash_slot   = (int)(s - s_card);
    s->used             = true;
}

static card_entry_t *card_find(const char *slug)
{
    if (!slug || !*slug || !s_card) return NULL;
    for (int i = 0; i < ORB_APP_CAP; i++)
        if (s_card[i].used && strcmp(s_card[i].slug_store, slug) == 0)
            return &s_card[i];
    return NULL;
}

int orb_app_card_upsert(const orb_app_t *row)
{
    if (!row || !row->slug || !row->slug[0]) return -1;
    if (!card_table()) return -1;                       /* no PSRAM: table full    */

    card_entry_t *s = card_find(row->slug);           /* re-install: keep the index */
    if (!s) {
        /* P4a: a card row lives at the index its FLASH slot names, so the
         * broker's g/c/x/r/e words for ORB_APP_BASE+slot always belong to this
         * app and to no successor. A caller that names no slot (the host
         * sentinel's bare upsert) falls back to the first free entry. */
        if (row->flash_slot >= 0 && row->flash_slot < ORB_APP_CAP &&
            !s_card[row->flash_slot].used)
            s = &s_card[row->flash_slot];
    }
    if (!s) {
        for (int i = 0; i < ORB_APP_CAP; i++)
            if (!s_card[i].used) { s = &s_card[i]; break; }
    }
    if (!s) return -1;                                /* B5: table full, slug new   */
    const int idx = ORB_APP_BASE + (int)(s - s_card);
    card_fill(s, row);
    return idx;
}

bool orb_app_card_has(const char *slug) { return card_find(slug) != NULL; }

int orb_app_card_count(void)
{
    if (!s_card) return 0;                              /* no block = nothing installed */
    int n = 0;
    for (int i = 0; i < ORB_APP_CAP; i++) if (s_card[i].used) n++;
    return n;
}

void orb_app_card_reset(void)
{
    if (s_card) memset(s_card, 0, (size_t)ORB_APP_CAP * sizeof *s_card);
}

/* ---- the enable path and the uninstall door (P4a) --------------------------
 * The registry answers RAM state only; persistence is the broker's (the e<slot>
 * word in orb.grants), because the sentinel forbids NVS in the installer and
 * the broker already owns every per-row NVS key. Contexts: set_enabled runs on
 * the worker (via orb_app_enable) or the httpd task; card_remove only on the
 * worker / boot ladder — neither touches a widget. */

bool orb_app_is_card(const orb_app_t *app)
{
    if (!app || !s_card) return false;
    return app >= (const orb_app_t *)&s_card[0] &&
           app <  (const orb_app_t *)&s_card[ORB_APP_CAP];
}

esp_err_t orb_app_set_enabled(const orb_app_t *app, bool on)
{
    if (!app) return ESP_ERR_INVALID_ARG;
    if (!orb_app_is_card(app))
        return ESP_ERR_NOT_SUPPORTED;   /* the compiled tier and the shell chrome
                                         * ARE the dial: no row to turn off */
    orb_app_t *a = (orb_app_t *)app;
    a->enabled = on;
    return ESP_OK;
}

bool orb_app_card_remove(const char *slug)
{
    card_entry_t *s = card_find(slug);
    if (!s) return false;
    /* The knob may not be holding a row that vanishes under it: the caller has
     * to carousel away (or home) first. The uninstall door sends the dial home
     * itself and waits for the switch before it ever reaches here (D38). */
    if (s_current == &s->app || s->app.active) return false;
    memset(s, 0, sizeof *s);            /* used=false: entry free, ledger forgets */
    return true;
}

/* ---- the combined walkable order (D1/C6) ----------------------------------
 *
 * The one definition of "what the knob can stop on": the compiled entries in
 * table order, then the qualified card rows in slot order. A card row is
 * qualified when it is used, enabled, not failed, and its behaviour_kind named a
 * registered module — so iface is non-NULL and there is code to run it. That
 * last clause is the whole point of the registry: an installed package cannot
 * walk the dial unless the firmware actually ships the module it names, exactly
 * as an unknown vocabulary kind is a skipped row in a theme (theme_schema.py:23).
 *
 * The count and the gather walk the SAME predicate so neighbour(), count(),
 * service_all() and disarm_all() can never disagree about the walkable set, and
 * a caller cannot get a pointer from one that the others refuse. With no card
 * installed the list is exactly the compiled walkable tier, so every existing
 * registry pin on these numbers holds unchanged. */
static size_t orb_app_walk_gather(orb_app_t **list, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < ORB_APP_N && n < max; i++)
        if (s_apps[i].enabled && !s_apps[i].failed) list[n++] = &s_apps[i];
    if (s_card)
        for (int k = 0; k < ORB_APP_CAP && n < max; k++)
            if (s_card[k].used && s_card[k].app.enabled &&
                !s_card[k].app.failed && s_card[k].app.iface)
                list[n++] = &s_card[k].app;
    return n;
}

/* ---- the API the shell calls --------------------------------------------- */

size_t orb_app_count(void)
{
    /* Only walkable entries count: a disabled app stays in the table so the
     * APPS page can still show it with its reason, but the carousel must not
     * stop on a blank screen. The count is computed, not cached, because
     * P4's enable/disable flips these flags at runtime and a cached count is
     * one more thing to forget to update. Card rows join only once enabled AND
     * their behaviour_kind resolved to a registered module (walkable gather);
     * with nothing installed s_card is empty and this is the compiled count. */
    orb_app_t *list[ORB_APP_N + ORB_APP_CAP];
    return orb_app_walk_gather(list, sizeof list / sizeof list[0]);
}

const orb_app_t *orb_app_at(size_t idx)
{
    /* The carousel's accessor, so idx walks the WALKABLE list, not the array:
     * asking for slot 2 of a table whose second entry is disabled must not
     * hand back a screen nobody can leave.
     *
     * DENSE, and gathered through the one walkable predicate the neighbour and
     * count paths use. The positional shortcut this replaces (`s_apps[idx]`,
     * bounded by ORB_APP_N) was honest while the compiled tier was the whole
     * carousel, and became a silent gap the day D35 moved the clock and the
     * picker into the card table: orb_app_count() counts card rows, so every
     * caller that loops `i < orb_app_count()` — menu_view's app_rows/app_row,
     * which is the menu's whole dynamic run — asked for the card rows at ranks
     * the compiled array could not answer, got NULL, and drew a menu with no app
     * rows on a dial whose apps are installed, enabled, and on the glass. Bench,
     * orb-B 2026-10-25. The ledger's positional accessor stays orb_app_row(). */
    orb_app_t *list[ORB_APP_N + ORB_APP_CAP];
    const size_t n = orb_app_walk_gather(list, sizeof list / sizeof list[0]);
    return idx < n ? list[idx] : NULL;
}

size_t orb_app_rows(void)
{
    /* Raw row span, disabled and FAILED entries included: the owner's
     * ledger (/api/v1/status, the APPS page) is exactly about the apps
     * the carousel hides. The carousel keeps using orb_app_count(). Card
     * apps appear here and nowhere the knob can reach.
     *
     * P4a: the span is POSITIONAL — every compiled index plus the whole card
     * table — so index == orb_app_index_of() == the broker's key for every
     * row a caller enumerates. A sparse table (slot 2 and slot 5 installed,
     * 3 and 4 free) answers row() with NULL for the holes; ledger walkers
     * skip them. A rank-dense span would drift on every uninstall and hand
     * a removed app's grant, fault and enable words to its successor —
     * exactly the misfiling D19 forbids.
     *
     * D35 drew the boundary at ORB_APP_BASE, not ORB_APP_N: the compiled tier
     * shrank to the shell alone, but the indices the clock and picker held as
     * compiled rows (1 and 2) are retired keys, never reused. The span is the
     * key space, and the key space does not renumber. */
    return ORB_APP_BASE + ORB_APP_CAP;
}

const orb_app_t *orb_app_row(size_t i)
{
    if (i < ORB_APP_N) return &s_apps[i];
    if (i < ORB_APP_BASE) return NULL;                  /* retired compiled keys */
    const size_t p = i - ORB_APP_BASE;
    if (p >= ORB_APP_CAP) return NULL;
    return (s_card && s_card[p].used) ? &s_card[p].app : NULL;   /* hole: no row
                                                                  * at this slot */
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
    /* Card apps resolve too — ?to=<card-slug> and GET /api/v1/screen name them —
     * but only after the compiled tier, so an installed app can never shadow the
     * shell. Since D1/C6 (and for the seeded pair since D35) a card row's iface
     * comes from the behaviour module its header names, so a row found here can
     * be a live screen, not only a ledger line; an unregistered kind stays
     * ledger-only exactly as before. */
    for (int k = 0; s_card && k < ORB_APP_CAP; k++)
        if (s_card[k].used && strcmp(s_card[k].slug_store, slug) == 0)
            return &s_card[k].app;
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
    /* Service rows answer at ORB_APP_MENU_BASE and above, past the ledger span,
     * so nothing that keys NVS words by index can meet one and mistake it for a
     * card's slot. It is an address, not a key. */
    for (int i = 0; i < ORB_APP_SERVICE_N; i++)
        if (s_service[i] == app) return ORB_APP_MENU_BASE + i;
    /* Card rows get a stable index at ORB_APP_BASE + slot, which is what keys
     * the broker's grant word — pointer-matched so the answer cannot drift from
     * the row the ledger handed out. */
    for (int k = 0; s_card && k < ORB_APP_CAP; k++)
        if (s_card[k].used && &s_card[k].app == app) return ORB_APP_BASE + k;
    return -1;
}

esp_err_t orb_app_init_all(void)
{
    /* D35 left one compiled row: the menu. The card tier builds after it, in
     * slot order. The hub seeds the dial first, so the clock still leads every installed app
     * and the shared orb_jobs worker — started before this whole ladder by
     * main.c — already exists before any row posts to it. */
    for (size_t i = 0; i < ORB_APP_N; i++) {
        orb_app_t *app = &s_apps[i];
        /* C2: an app the broker marked failed — two unexpected resets with
         * it on the glass — does not build this boot. Its reason stays in
         * the broker until the APPS page (P4) can show it; the home entry is
         * deliberately exempt. Under D35 that entry is the menu — chrome that
         * builds from tables and cannot fault — so the exemption protects the
         * one destination long press can always reach (the counter still runs
         * and the status endpoint shows it, which is how a faulting home stays
         * visible without being fatal). */
        if (orb_svc_faulted(app) && !app->is_home) {
            app->failed = true;
            ESP_LOGE(TAG, "%s: %u faults — booting without it (%s)",
                     app->slug, (unsigned)orb_svc_fault_count(app),
                     orb_svc_fault_reason(app));
        } else if (orb_svc_faulted(app)) {
            ESP_LOGE(TAG, "%s: faulting HOME entry — it still boots; there is "
                         "no other destination", app->slug);
        }
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
        app->init_done = true;
    }

    /* The service tier (D36), built last among the compiled rows and by its own
     * short rule: it needs an init hook and the option hooks, and it is the one
     * tier that must NOT be required to have a screen() — that is what makes it
     * a service row rather than an app. It is not fault-gated either: the broker
     * has no words for a row outside the ledger span, and an OS feature with no
     * code to fault has no reason to be. */
    for (int i = 0; i < ORB_APP_SERVICE_N; i++) {
        orb_app_t *svc = s_service[i];
        if (!svc) continue;
        if (!svc->iface || !svc->iface->init) {
            ESP_LOGE(TAG, "service row %d has no init hook — not listed", i);
            continue;
        }
        const esp_err_t err = svc->iface->init(svc);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "service row %s: init failed (%s) — not listed",
                     svc->slug, esp_err_to_name(err));
            svc->failed = true;
            continue;
        }
        svc->init_done = true;
    }

    /* The card table, built after the compiled tier so an installed app never
     * init()s ahead of the clock's worker task (the boot-order rule above).
     * card_fill() leaves every card disabled until the run-on-boot decision
     * lands, so on a real flash this loop is inert and nothing is installed; the
     * registry's host sentinel enables a row to prove an installed app's iface
     * init/screen hooks run exactly like a compiled entry's. A card row here
     * always has an iface — the caller resolved behaviour_kind to a registered
     * module — so a NULL iface means an unregistered kind: it stays ledger-only
     * (never built, never walkable), the validated-not-drawn rule made literal. */
    for (int k = 0; s_card && k < ORB_APP_CAP; k++) {
        orb_app_t *app = &s_card[k].app;
        if (!s_card[k].used || !app->enabled || app->failed) continue;
        /* C2, uniform for the card tier: two unexpected resets with this row on
         * the glass and it does not build this boot — no is_home exemption lives
         * here, because no card row is home (D35 moved homing to the chrome). The
         * compiled loop above reaches the same verdict through orb_svc_faulted;
         * a wasm row reaches it through apply_code's load skip and this loop's
         * failed init. The seeded clock and picker quarantine exactly like every
         * other app now that they are rows in this loop. */
        if (orb_svc_faulted(app)) {
            app->failed = true;
            ESP_LOGE(TAG, "%s: %u faults — booting without it (%s)",
                     app->slug, (unsigned)orb_svc_fault_count(app),
                     orb_svc_fault_reason(app));
            continue;
        }
        if (!app->iface || !app->iface->init || !app->iface->screen) {
            ESP_LOGW(TAG, "%s: no registered behaviour module (kind %lu) — ledger only",
                     app->slug, (unsigned long)app->behaviour_kind);
            app->enabled = false;
            continue;
        }
        /* C6 singleton rule: a behaviour module builds its widgets once. A row
         * whose module is already built — two installed containers naming one
         * kind, or the seeded clock beside a later install of kind 3 — rides
         * the first row's screen instead of re-running init() and stranding the
         * first widget tree (color_picker_init builds a fresh s_scr and leaks
         * the old one). Two
         * rows, one face: the carousel may show the same live module twice,
         * and the ledger keeps each row's own grant, fault and enable words. */
        bool built = false;
        for (size_t i = 0; i < ORB_APP_N; i++)
            if (s_apps[i].iface == app->iface && s_apps[i].enabled && !s_apps[i].failed)
                built = true;
        for (int j = 0; j < k && !built; j++)
            if (s_card[j].used && s_card[j].app.installed && s_card[j].app.enabled &&
                s_card[j].app.iface == app->iface)
                built = true;
        if (built) {
            app->installed = true;
            app->init_done = true;   /* the module is built — by its first row */
            ESP_LOGI(TAG, "%s: reuses its behaviour module's built screen — walkable",
                     app->slug);
            continue;
        }
        const esp_err_t err = app->iface->init(app);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: init failed (%s) — dropped from the carousel",
                     app->slug, esp_err_to_name(err));
            app->enabled = false;
            app->failed  = true;
            continue;
        }
        app->installed = true;
        app->init_done = true;
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
    if (orb_app_is_menu_only(app)) {
        /* D36: no face to load, so there is nothing to activate. The press that
         * would have landed here came from the menu's app run, and the menu
         * gives it to the row's own option hooks instead — which is the whole
         * reason this tier exists: a settings page and a state line, without a
         * carousel slot, an art bank, or a ledger word. */
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (app->failed)  return ESP_ERR_INVALID_STATE;
    if (!app->enabled) return ESP_ERR_NOT_SUPPORTED;
    /* D1/C6: an installed card app whose declared behaviour_kind no firmware
     * module registered has no iface — nothing was ever built to draw it. The
     * gather already keeps it out of the carousel; refusing it here too closes
     * the direct-activate path (?to=<card-slug>, a route) so a below-ABI package
     * can never reach the iface derefs below and take the dial down with it. */
    if (!app->iface) return ESP_ERR_NOT_SUPPORTED;

    /* D35's open follow-up (bench orb-B 2026-10-21): a card row whose enable
     * word landed AFTER the boot gather — the picker re-enabled from /apps, a
     * seeder enable pressed before any reboot — was never init()ed, and its
     * screen() answer is NULL until it is. Gather only builds rows enabled at
     * the moment it runs. Build once here: both callers of this function, the
     * router's poll and the menu's press, run on the LVGL task, which is the
     * one task widget construction is allowed to happen on. A wasm row's init
     * checks residency only, so this is where the native kinds get their face. */
    if (!app->init_done) {
        if (app->iface->init && app->iface->init(app) != ESP_OK) {
            ESP_LOGE(TAG, "%s: lazy init refused — cannot be shown", app->slug);
            return ESP_FAIL;
        }
        app->init_done = true;
    }

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
    /* §4.2's auto-revoke, in the one place that knows what left the glass:
     * the app whose view was suspended cannot also hold the dial awake.
     * Deliberately after the suspend hook — an app finishing real work in
     * its suspend gets its last word on power too, and then loses it. */
    if (old) orb_svc_app_suspended(old);

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
    /* C2: the reset record follows the glass, so the slot it bills for an
     * unowned reset is the one that was actually up when it landed. */
    orb_svc_current_note(app);
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
     * measured, which is why nothing in this loop loads a screen. The combined
     * gather is used so a qualified card app reaps its jobs too; its iface is
     * non-NULL by the gather's own condition, so the deref is safe. */
    orb_app_t *list[ORB_APP_N + ORB_APP_CAP];
    const size_t n = orb_app_walk_gather(list, sizeof list / sizeof list[0]);
    for (size_t k = 0; k < n; k++) {
        orb_app_t *app = list[k];
        if (app->iface->service) app->iface->service(app);
    }
}

void orb_app_disarm_all(void)
{
    /* Every entry is disarmed together rather than only the one leaving, so an
     * app can never be left armed off-screen and then steal the next turn from
     * the view actually showing — the failure input_router.c guarded against by
     * hand when it named the menu and the picker in disarm_all(). */
    orb_app_t *list[ORB_APP_N + ORB_APP_CAP];
    const size_t n = orb_app_walk_gather(list, sizeof list / sizeof list[0]);
    for (size_t k = 0; k < n; k++) {
        orb_app_t *app = list[k];
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
     * The walk is over the combined order (compiled tier then qualified card
     * rows), the same predicate count() reports, so a qualified installed app
     * sits in the ring beside the compiled pair and an unqualified one (iface
     * NULL — no module this firmware registers) is skipped exactly like a
     * disabled entry. Position is found by pointer, not orb_app_index_of(),
     * because a card row's index is ORB_APP_BASE+slot while this walk is dense
     * over only the walkable set.
     *
     * One step per call, in the direction turned: a fast burst reports several
     * detents and must not fly past every screen and stop somewhere nobody
     * aimed at, which is why input_router feeds this one step per poll. */
    orb_app_t *list[ORB_APP_N + ORB_APP_CAP];
    const size_t n = orb_app_walk_gather(list, sizeof list / sizeof list[0]);
    if (n == 0) return NULL;

    int i = -1;
    for (size_t k = 0; k < n; k++)
        if (list[k] == from) { i = (int)k; break; }
    if (i < 0) return list[0];   /* nothing visible yet: the first walkable, the dial */

    /* The gather already dropped every unwalkable entry, so a step never needs
     * to skip: one modular move lands on the next thing the knob can stop on,
     * and a table of one walkable entry returns itself (the ring folds back). */
    i = (int)(((long)i + step) % (long)n);
    if (i < 0) i += (int)n;
    return list[i];
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

orb_opt_kind_t orb_app_opt_kind(const orb_app_t *app, int idx)
{
    /* NULL is the whole pre-D12 ABI: an app that never heard of nesting answers
     * OPT_ROW for every row, and the page renders exactly as it did before this
     * hook existed. The range check is the wrappers' own, not the shell's — a
     * question about a row that does not exist must not reach an app that is
     * counting its rows from a card walk that has not landed yet. */
    if (!app || !app->iface || !app->iface->opt_kind) return OPT_ROW;
    if (idx < 0 || idx >= orb_app_opt_count(app)) return OPT_ROW;
    const orb_opt_kind_t k = app->iface->opt_kind((orb_app_t *)app, idx);
    /* Anything that is not one of the three is a plain row. An app that answers
     * with an uninitialised int would otherwise buy itself a page whose rows the
     * owner can hear the knob move past and cannot see. */
    return (k == OPT_GROUP || k == OPT_ITEM) ? k : OPT_ROW;
}

void orb_app_opt_arm(const orb_app_t *app, bool armed)
{
    if (!app || !app->iface || !app->iface->opt_arm) return;
    app->iface->opt_arm((orb_app_t *)app, armed);
}

bool orb_app_opt_long(const orb_app_t *app)
{
    /* No hook means the entry has nothing to collapse, and the long press is the
     * router's to spend on going home (C8). */
    if (!app || !app->iface || !app->iface->opt_long) return false;
    return app->iface->opt_long((orb_app_t *)app);
}

void orb_app_opt_focus(const orb_app_t *app, int idx)
{
    /* A NULL hook is the whole pre-preview ABI: the shell moved its cursor and
     * this entry has no reason to hear about it. The bounds check is the
     * wrappers' own, like opt_kind's: a focus on a row the app did not count
     * must not reach it, because the count is the app's and the cursor is the
     * shell's, and the two agree only through this door. */
    if (!app || !app->iface || !app->iface->opt_focus) return;
    if (idx < 0 || idx >= orb_app_opt_count(app)) return;
    app->iface->opt_focus((orb_app_t *)app, idx);
}

const void *orb_app_opt_preview(const orb_app_t *app, int idx, uint16_t *w,
                                uint16_t *h, uint32_t *stamp)
{
    /* Every refusal here answers NULL — no hook, no entry, an out-of-range row —
     * and NULL is the shell's own "text-only" answer, the state the whole tree
     * was in before previews existed. The extents and the stamp are written only
     * with a plane; a caller that reads them on a NULL reads its own stale
     * frame, so they are zeroed first. */
    if (w) *w = 0;
    if (h) *h = 0;
    if (stamp) *stamp = 0;
    if (!app || !app->iface || !app->iface->opt_preview) return NULL;
    if (idx < 0 || idx >= orb_app_opt_count(app)) return NULL;
    return app->iface->opt_preview((orb_app_t *)app, idx, w, h, stamp);
}

bool orb_app_share_gate(bool press)
{
    /* D38: the USB share window's claim on the knob. This is the one input
     * question that is NOT asked of the current screen: during a window the
     * glass paints no frames, so the screen the freeze caught is history, and
     * the shell's share card (menu_view's PG_SHARE) speaks for the whole dial
     * — press or hold is the stop, turns are spent. The router asks the
     * registry rather than naming the view (C6); the registry's answer is
     * false on every build and every poll that holds no window. */
    return menu_view_share_gate(press);
}

bool orb_app_has_options(const orb_app_t *app)
{
    /* Whether an app OFFERS a page is decided by what it declares, not by the
     * rows the page currently answers. Asking opt_count() here is a closed loop
     * the dial can never leave the first time: the clock's count is 0 until a
     * card walk lands, the walk is posted by clock_opt_arm(), and that runs only
     * when the owner presses the row that this very decision hides. So the row
     * never appears, and the owner who has four themes on the card sees a menu
     * with no Clock entry at all — the exact reachability this whole layer is
     * for. Declaring the hooks is the honest signal that there is something to
     * show; the rows fill in once the first walk lands. */
    if (!app || !app->enabled || app->failed) return false;
    return app->iface && app->iface->opt_count && app->iface->opt_label
           && app->iface->opt_arm;
}
