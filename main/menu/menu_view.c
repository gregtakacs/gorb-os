/**
 * menu_view.c — the dial's options screen (see header for the contract).
 *
 * Rendered as a small pool of row objects repainted from module statics, the way
 * color_picker.c does it, rather than as an lv_list: the dial is 466x466 and the
 * router owns the only knob, so a real scrollable container would fight the
 * gesture that is supposed to drive it.
 *
 * ---- why there is a task in here -------------------------------------------
 *
 * What still blocks on this screen is NVS: the two DISPLAY setters and the grant
 * writes, each of which can hit a sector erase. A theme walk used to
 * be here too; B2 gave the theme picker to the clock app, and its card work runs
 * on the shell's one worker (main/app/orb_jobs.c) rather than on a task this file
 * owns, so the dial's repaint is never held up by a directory read.
 *
 * The reason for a worker at all is unchanged: input_router's poll is an lv_timer
 * on the LVGL task, and that same callback is the ONLY reader of the knob and the
 * only thing that repaints the dial. Blocking it freezes the screen and swallows
 * the knob for as long as the write takes.
 *
 * This module takes no locks of its own.
 */

#include "menu_view.h"
#include "board_config.h"
#include "knob.h"
#include "knob_edit.h"     /* the edit mode's step maths (docs/knob-edit-plan.md) */
#include "esp_timer.h"     /* the edit mode's turn speed */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"         /* EXT_RAM_BSS_ATTR: the QR plane lives in PSRAM */
#include "orb_jobs.h"
#include "libs/qrcode/qrcodegen.h"   /* ABOUT: the phone door (LVGL's copy, CONFIG_LV_USE_QRCODE) */
#include "orb_home.h"
#include "orb_units.h"          /* ABOUT: the zone, named from the home that set it */
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_app_desc.h"   /* ABOUT: the running image's own stamp */

#include "orb_net.h"
#include "orb_sd.h"
#include "orb_app.h"
#include "themes_page.h"
#include "orb_theme.h"     /* theme-cache-plan: why an app's theme is missing */
#include "theme_strip.h"   /* §61: the Themes row opens the theme carousel */     /* §58: an app's Themes sub-menu */
#include "orb_rotation.h"     /* §59: the owner's rotation */
#include "orb_app_install.h"   /* APPS: the uninstall and fault-clear doors (P5.3) */
#include "orb_app_scan.h"      /* D38: the APPS catalog walks the card too */
#include "idle_dim.h"          /* DISPLAY submenu: the two runtime settings */
#include "input_router.h"     /* exit -> input_router_menu_exit() */
#include "nvs.h"               /* the top-page cursor memory, the router's page_write idiom */
#if CONFIG_ORB_USB_MSC
#include "orb_usb_msc.h"      /* PG_SHARE: the share window's one fact (D38) */
#endif

static const char *TAG = "menu";

/* Rows painted at once. The dial is round: content stays inside a centred
 * ~320 px column so the curve never clips a label. */
#define ROW_MAX      5

/* ---- the model: one table, one row per page (plan §7.1, P5.2) --------------
 *
 * The table is the only place that says what a page IS: how many rows it draws,
 * each row's label, its title and hint, what a press does, and where "<- back"
 * returns to and on which row. The switches this replaces (page_rows, row_text,
 * activate's page arms, go_back, page_title, page_hint) had to agree about every
 * page by hand and nothing enforced it — go_back spelled each page's parent and
 * landing row a second time. Now the agreements are one definition deep, and
 * the one an editor can still miss (the table growing without the top page's
 * exit-row arithmetic growing with it) is a compile error below.
 *
 * The top page is its first row: s_top carries the OS's own entries, and
 * rows_top() adds the dynamic ones — the service pages (USB Drive, Sound),
 * then exit. APPS and ABOUT landed as s_top entries in P5.3 without this shape
 * changing at all, which is the point of the table. */

typedef enum {
    PG_LANDING = 0,   /* inert title; the knob belongs to the router   */
    PG_TOP,           /* s_top, then the service rows, exit */
    PG_APP,           /* a service page's options, an app's Themes sub-menu, or an
                       * installed app's one page: its settings, its Themes and
                       * Permissions rows and its doors (2026-11-07) */
    PG_DISPLAY,       /* the two idle-dim settings                    */
    PG_TO_RETIRED,    /* was the timeout presets; the timeout is knob-edited now
                       * (docs/knob-edit-plan.md). Kept so the saved menu position's
                       * page numbers keep their meaning; never restored or entered */
    PG_PERMS,         /* retired 2026-11-07; kept so stored page numbers keep meaning */
    PG_CAPS,          /* one app's Permissions sub-menu: its caps, togglable */
    PG_APPS,          /* the catalog: every installed app, its state, its doors (B3/B4) */
    PG_APPACT,        /* a card file's doors: install / remove from card */
    PG_ABOUT,         /* inert identity list — no press does anything  */
    PG_SHARE,         /* D38: the USB share window's own card, forced over any page */
    PG_UNITS,         /* weather-plan §2.1: temperature, speed, distance for every app */
    PG_GO,            /* plan §51: the app list a hold opens, then settings */
    PG_N
} page_t;

/** Where a row — or a page's "<- back" — navigates. A value >= PG_LANDING is a
 *  table index; the negatives are what no table index can say: this row (or
 *  page) does not navigate, and leave the menu. */
typedef enum { MENU_GO_NONE = -2, MENU_GO_EXIT = -1 } menu_go_t;

/** One static row of a page's sub-table — the top page's today, and the shape
 *  P5.3's APPS/ABOUT rows reuse. A press runs act() first and navigates to
 *  `to` second; a row with neither is a label, which no shipped row is. */
typedef struct {
    const char *label;
    int  to;                 /* PG_* id or a menu_go_t          */
    void (*act)(void);       /* runs before the navigation      */
} menu_row_t;

/* docs/knob-edit-plan.md §1.1: a row the knob edits. The page answers its range and
 * value; fmt renders the row as it will read with a value not yet saved; commit saves. */
typedef struct {
    knob_range_t r;
    int32_t      v;
    void (*fmt)(int row, int32_t v, char *out, size_t outsz);
    void (*commit)(int row, int32_t v);
} menu_edit_t;

typedef struct menu_page_s menu_page_t;
struct menu_page_s {
    int  (*rows)(void);                 /* rows on the glass, back row incl.  */
    void (*label)(int row, char *out, size_t outsz, bool *sel_hi);
    const char *(*title)(void);
    const char *(*hint)(void);          /* NULL = the generic open hint       */
    void (*act)(int row);               /* the press, once back and exit are ruled out */
    /** The dynamic kinds' entry: PG_APP's open resolves which app, arms it,
     *  validates its shape and opens its one group — a generic dispatcher cannot
     *  know an app page needs that, which is why entry is a hook and not a
     *  flag. enter() only places the cursor. */
    void (*open)(const orb_app_t *a);
    /** Where "<- back" lands, when the parent's row of interest moves: the
     *  a service page returns to the top page's dynamic run at its own row,
     *  and an app's page to its row of APPS (go_back). Evaluated
     *  before leave(), so the page can still read its own state. */
    int  (*ordinal)(void);
    void (*enter)(void);                /* cursor placement on entry          */
    void (*leave)(void);                /* side effects when the page is left */
    int  back_to;                       /* menu_go_t: NONE = the page has no back row */
    int  back_cur;                      /* cursor there, literal              */
    /** A row the knob edits: true and its range, value, row text and save. A press on
     *  such a row enters the edit mode instead of act(). NULL: a press-only page. */
    bool (*edit)(int row, menu_edit_t *e);
};

/* The page table ships below the functions it names (C has no forward into an
 * initializer list); it is declared here so s_pg can start on the landing row
 * and every function can reach its own row through P(). The sentinel lifts the
 * definition whole — the table is the page inventory, and the host suite is
 * the thing that must read it as one. */
extern const menu_page_t s_pages[PG_N];
static const menu_page_t *s_pg = &s_pages[PG_LANDING];
#define P(id) (&s_pages[id])
#define TOP_OS_N ((int)(sizeof(s_top) / sizeof(s_top[0])))
#define TOP_EXIT (TOP_OS_N + app_rows())      /* the top page's last row      */

/* Everything the table names, declared before it: the definitions follow in
 * whatever order their neighbours need, and the table reads as the inventory
 * it is. */
static int  rows_top(void);      static void label_top(int, char *, size_t, bool *);
static const char *title_top(void);
static int  rows_app(void);      static void label_app(int, char *, size_t, bool *);
static const char *title_app(void);  static const char *hint_app(void);
static void act_app(int);        static void open_app(const orb_app_t *);
static int  ord_app(void);       static void leave_app(void);
static int  rows_display(void);  static void label_display(int, char *, size_t, bool *);
static const char *title_display(void);
static void act_display(int);
static bool edit_display(int row, menu_edit_t *e);
static int  rows_units(void);    static void label_units(int, char *, size_t, bool *);
static const char *title_units(void);  static const char *hint_units(void);
static bool edit_units(int row, menu_edit_t *e);
static bool edit_app(int row, menu_edit_t *e);
static bool app_owned(const orb_app_t *a);
static int  own_rows(void);      static void label_own(int, char *, size_t, bool *);
static void act_own(int);        static void reopen_app(const orb_app_t *a, int what);
static int  rows_caps(void);     static void label_caps(int, char *, size_t, bool *);
static const char *title_caps(void);   static const char *hint_caps(void);
static void act_caps(int);
static int  rows_apps(void);     static void label_apps(int, char *, size_t, bool *);
static const char *title_apps(void);   static const char *hint_apps(void);
static void act_apps(int);       static void enter_apps(void);
static int  rows_appact(void);   static void label_appact(int, char *, size_t, bool *);
static const char *title_appact(void); static const char *hint_appact(void);
static void act_appact(int);     static int  ord_appact(void);
static int  rows_about(void);    static void label_about(int, char *, size_t, bool *);
static const char *title_about(void);
static int  rows_share(void);    static void label_share(int, char *, size_t, bool *);
static const char *title_share(void);  static const char *hint_share(void);
static int  top_os_rows(void);   static int app_row_index(const orb_app_t *);
static void go_to(int to, int cur);
static int  cap_pack(const orb_app_t *, int row);
static int  door_pack(const orb_app_t *, int act);
static void job_grant(void *arg);
static void job_clear(void *arg);    static void job_uninstall(void *arg);
static void job_scan(void *arg);     static void job_install_card(void *arg);
static void job_cardrm(void *arg);
static int  page_rows(void);
static void row_text(int, char *, size_t, bool *);
static const char *page_title(void);
static void title_line(char *, size_t);
static int  rows_on_glass(void);
static const char *page_hint(void);
/* The busy state the last paint drew: "working..." is painted while a job
 * runs, and a job that lands without changing a row count (a theme walk
 * publishing through its provider) would otherwise leave the hint up until
 * the next detent — read as a stuck page (Gold, 2026-11-07). */
static bool s_painted_busy;
static void edit_end(void);      /* the knob edit mode, docs/knob-edit-plan.md */
static void go_back(void);       static void activate(void);
static void repaint(void);       static void job_post(orb_job_fn fn, int idx);


/** How many dynamic rows the top page carries: the service tier (D36 — rows with
 *  settings and no face, the OS's own answers). */
static int app_rows(void)
{
    /* The service tier only (USB Drive, Sound): an app's own settings, themes and
     * permissions live on its APPS page now (2026-11-07). */
    return (int)orb_app_menu_count();
}

/** The i-th service row, or NULL. Resolved through the registry every time
 *  rather than cached, so this stays the only place the run is defined. */
static const orb_app_t *app_row(int i)
{
    if (i < 0) return NULL;
    const int nsvc = (int)orb_app_menu_count();
    return i < nsvc ? orb_app_menu_at((size_t)i) : NULL;
}

/* The dim timeout, edited with the knob in minutes (docs/knob-edit-plan.md §1.2: 1 to
 * 120, fast turns in fives). It replaced a list of seven presets, whose ruling ("a
 * wall-clock decision, not a tuning knob") the owner superseded on 2026-11-04. */
#define TO_MIN_MAX 120

/* Row label for the timeout actually in force: whole minutes, else the truth in
 * seconds ("45 s"), which a device flashed with another Kconfig default can hold. */
static void timeout_label(char *out, size_t outsz)
{
    const uint32_t cur = idle_dim_timeout_s();
    if (cur && cur % 60u == 0) snprintf(out, outsz, "%lu min", (unsigned long)(cur / 60u));
    else snprintf(out, outsz, "%lu s", (unsigned long)cur);
}

/* Which app PG_APP is showing. Set on entry from the registry and cleared on
 * leave, so a repaint can never render the previous app's rows under the new
 * one's header — the two-second card walk that lands after the owner has
 * already rotated away is exactly when that would show. */
static const orb_app_t *s_app;
static bool   s_armed;
static int    s_cur;              /* cursor within the current page */
static int    s_painted_rows;     /* page_rows() the last repaint drew, for the PG_APP reap */

/* ---- D19: the owner's revocation ladder (plan §4.2), rows on an app's own page
 * (2026-11-07; it was two pages below PERMISSIONS) ----------------------------
 *
 * Declare-at-install is the grant, so these rows approve nothing — they let
 * the owner take back what an app asked for at install, and show the
 * difference between what it declared and what still runs. The rows, the
 * state, and the one rule the pages exist for: the broker is the only authority
 * on what a toggle may do, so a row never decides locally whether its cap is
 * grantable, it posts the job and renders the answer. The caps listed are the
 * owner-granted ones (D39 added sound as the fifth): the free ones would put rows on this page that can
 * only ever say yes, and a page of forced yeses teaches the owner to stop
 * reading it.
 */
static int      s_perm_slot = -1;              /* the Permissions sub-menu's app */
/* §58: the Themes sub-menu is the provider row's page aimed at one app; this says
 * whose, so back returns to that app's page and not to the top. */
static const orb_app_t *s_theme_of;
/* An app page's own rows after its settings: these two sub-menu rows, else a door id. */
enum { OWN_THEMES = -2, OWN_PERMS = -3, OWN_ROT = -4 };
static const orb_cap_t OWNER_CAPS[] = { CAP_POWER, CAP_NET,
                                        CAP_SD_READ, CAP_THEMES, CAP_SOUND, CAP_LOCATION, CAP_RING };
#define N_OWNER_CAPS ((int)(sizeof OWNER_CAPS / sizeof OWNER_CAPS[0]))
_Static_assert(N_OWNER_CAPS == CAP_N - CAP_POWER,
               "the permissions page lists every owner cap");

/* The installed, non-chrome entries: APPS's list, every app that can ASK,
 * option hooks or none. */
static int perm_rows(void)
{
    int n = 0;
    for (size_t i = 0; i < orb_app_rows(); i++) {
        const orb_app_t *a = orb_app_row(i);
        if (a && a->kind == ORB_APP_KIND_APP && a->installed) n++;
    }
    return n;
}
static const orb_app_t *perm_row(int i)
{
    for (size_t r = 0; r < orb_app_rows(); r++) {
        const orb_app_t *a = orb_app_row(r);
        if (a && a->kind == ORB_APP_KIND_APP && a->installed && i-- == 0) return a;
    }
    return NULL;
}
static int perm_row_of(const orb_app_t *a)
{
    int n = 0;
    for (size_t r = 0; r < orb_app_rows(); r++) {
        const orb_app_t *x = orb_app_row(r);
        if (x && x->kind == ORB_APP_KIND_APP && x->installed) { if (x == a) return n; n++; }
    }
    return 0;
}
/* Any app that has been refused at least once this boot. The marker is not an
 * ask awaiting an answer (D19 deleted those): it says a call was closed, either
 * by the owner's own toggle or by an app reaching for a cap it never declared,
 * and only the second is the app's fault. The broker's one line per (app, cap)
 * says which, and the owner has to do nothing either way. RAM-only, like the
 * counts — a reboot is the reset, not a nag the owner has to dismiss. */
static bool perms_denied(void)
{
    for (size_t r = 0; r < orb_app_rows(); r++) {
        const orb_app_t *a = orb_app_row(r);
        if (a && a->kind == ORB_APP_KIND_APP && orb_svc_denied_mask(a)) return true;
    }
    return false;
}

/* ---- the one level of app-owned nesting (ruling D12) -----------------------
 *
 * The app says what each row IS; this file decides what to draw and where the
 * cursor may go. An OPT_ITEM belongs to the nearest preceding OPT_GROUP and is
 * drawn only while that group is open, so a page of sixty rows can carry the
 * twelve the owner is actually looking at. One group per page is the whole ABI —
 * items never live inside items — which is what keeps a state this small: the
 * open group's app row index, or -1.
 *
 * The state lives in the shell and not in the app on purpose: the app that owned
 * the open/closed bit would own the cursor's range too, and §13.4 spends its
 * existence on making sure the app never does that. */
static int  s_group_open = -1;

/* What the worker last brought back, copied out for the UI. */
typedef struct {
    char              status[72];                           /* line, or ""   */
} ui_t;

static ui_t   s_ui;               /* read by the repaint, LVGL task only  */
static ui_t   s_shared;           /* written by the worker, under s_res_mx */
static SemaphoreHandle_t s_res_mx;
static volatile bool     s_dirty; /* worker published, UI has not reaped it */

static void worker_publish(const ui_t *src)
{
    xSemaphoreTake(s_res_mx, portMAX_DELAY);
    s_shared = *src;
    xSemaphoreGive(s_res_mx);
    s_dirty = true;                 /* published last: the reader trusts the copy */
}

/* The jobs this screen still needs. Each takes the copied payload the
 * shell's worker hands it and writes only its own status line: nothing here
 * touches a widget, which is the half of the rule that keeps the dial moving
 * while NVS erases a sector. */
static void job_dim_en(void *arg)
{
    (void)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const bool want = !idle_dim_enabled();
    const esp_err_t er = idle_dim_set_enabled(want);
    if (er == ESP_ERR_NOT_SUPPORTED)
        snprintf(out.status, sizeof(out.status), "the dim is not in this build");
    else
        /* NVS failure still left the RAM setting in force, and the line says
         * which it cost: without the qualifier a "dim after idle: off" that
         * dies at the next reboot reads as a switch that un-toggles itself. */
        snprintf(out.status, sizeof(out.status), "dim after idle: %s%s",
                 want ? "on" : "off", (er == ESP_OK) ? "" : " (until reboot, nvs refused)");
    worker_publish(&out);
}

/* One unit, packed kind * 8 + value by commit_units. */
static void job_units(void *arg)
{
    const int kv = *(const int *)arg, kind = kv / 8, v = kv % 8;
    ui_t out; memset(&out, 0, sizeof out);
    orb_units_t u;
    orb_units_get(&u);
    if (kind == ORB_UNITS_TEMP) u.temp = (uint8_t)v;
    else if (kind == ORB_UNITS_SPEED) u.speed = (uint8_t)v;
    else if (kind == ORB_UNITS_TIME) u.time = (uint8_t)v;
    else if (kind == ORB_UNITS_PRESSURE) u.pressure = (uint8_t)v;
    else u.dist = (uint8_t)v;
    const char *why = orb_units_set(&u);
    const char *l = orb_units_label(kind, v);
    if (why) snprintf(out.status, sizeof(out.status), "units: %s", why);
    else snprintf(out.status, sizeof(out.status), "%s: %s", kind == ORB_UNITS_TEMP ? "temperature" :
                  kind == ORB_UNITS_SPEED ? "speed" : kind == ORB_UNITS_TIME ? "time" :
                  kind == ORB_UNITS_PRESSURE ? "pressure" : "distance",
                  l ? l : "?");
    worker_publish(&out);
}

/* §59: the rotation's interval choices, 0 = off. */
static const uint32_t ROT_S[] = { 0, 15, 30, 60, 120, 300, 600, 900, 1800, 3600 };
#define ROT_N ((int)(sizeof ROT_S / sizeof ROT_S[0]))
static void rot_text(int choice, char *out, size_t outsz)
{
    const uint32_t v = (choice > 0 && choice < ROT_N) ? ROT_S[choice] : 0;
    if (!v)            snprintf(out, outsz, "off");
    else if (v < 60)   snprintf(out, outsz, "every %lu s", (unsigned long)v);
    else               snprintf(out, outsz, "every %lu min", (unsigned long)(v / 60));
}
static int rot_choice(void)
{
    orb_rotation_t r;
    orb_rotation_get(&r);
    if (!r.on) return 0;
    int best = 1;
    for (int i = 1; i < ROT_N; i++)
        if ((ROT_S[i] > r.every_s ? ROT_S[i] - r.every_s : r.every_s - ROT_S[i]) <
            (ROT_S[best] > r.every_s ? ROT_S[best] - r.every_s : r.every_s - ROT_S[best])) best = i;
    return best;
}
static void job_rot_every(void *arg)
{
    const int c = *(const int *)arg;
    ui_t out; memset(&out, 0, sizeof out);
    orb_rotation_t r;
    orb_rotation_get(&r);
    const char *why = orb_rotation_set(c > 0, c > 0 && c < ROT_N ? ROT_S[c] : r.every_s, NULL);
    char t[24];
    rot_text(c, t, sizeof t);
    snprintf(out.status, sizeof out.status, why ? "%.60s" : "rotate apps: %s", why ? why : t);
    worker_publish(&out);
}
/* pack: (registry index << 1) | take part */
static void job_rot_member(void *arg)
{
    const int pack = *(const int *)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const orb_app_t *a = orb_app_row((size_t)(pack >> 1));
    if (!a || !a->slug) { snprintf(out.status, sizeof out.status, "that app is gone"); worker_publish(&out); return; }
    orb_rotation_t r;
    orb_rotation_get(&r);
    char csv[ORB_ROTATION_MAX * ORB_ROTATION_SLUG] = "";
    size_t at = 0;
    for (int i = 0; i < r.n; i++) {
        if (!strcmp(r.apps[i], a->slug)) continue;
        at += (size_t)snprintf(csv + at, sizeof csv - at, "%s%s", at ? "," : "", r.apps[i]);
    }
    if (pack & 1) snprintf(csv + at, sizeof csv - at, "%s%s", at ? "," : "", a->slug);
    const char *why = orb_rotation_set(r.on, r.every_s, csv);
    snprintf(out.status, sizeof out.status, "\"%.30s\": %s", a->title ? a->title : a->slug,
             why ? why : (pack & 1) ? "in the rotation" : "out of the rotation");
    worker_publish(&out);
}

static void job_dim_to(void *arg)
{
    const int min = *(const int *)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const esp_err_t er = idle_dim_set_timeout_s((uint32_t)min * 60u);
    if (er == ESP_ERR_NOT_SUPPORTED)
        snprintf(out.status, sizeof(out.status), "the dim is not in this build");
    else
        snprintf(out.status, sizeof(out.status), "timeout: %d min%s", min,
                 (er == ESP_OK) ? "" : " (until reboot, nvs refused)");
    worker_publish(&out);
}

/* ---- the view ---- */

static lv_obj_t *s_scr;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_row[ROW_MAX];
static lv_obj_t *s_row_lbl[ROW_MAX];

/* The preview plane, B2's one image answer. One lv_image over the five row
 * strips, built once at init and hidden until an app's opt_preview answers a
 * plane for the row under the cursor — the same build-once/repaint-from-
 * statics shape the row strips use, for the same reason: allocating under a
 * turning knob fragments the heap the framebuffer is competing with. The
 * dsc is the shell's own (the buffer it points at belongs to the app); the
 * stamp keys the reap repaint, so a plane that lands while the page stands
 * still still reaches the glass. The image is created after every other
 * child of s_scr, so it paints over the list, which is what the design's
 * "full-frame over the list" means: the rows stay painted under it, and the
 * plane hides them. */
static lv_obj_t         *s_pv_img;
static lv_obj_t         *s_note_box, *s_note_lbl;   /* §58: the message box */
static lv_image_dsc_t    s_pv_dsc;
static uint32_t          s_pv_stamp;      /* the stamp last painted; 0 = none */
static uint16_t          s_pv_w, s_pv_h;  /* its drawn extent                 */

static const lv_color_t COL_BG_SEL   = LV_COLOR_MAKE(0x2e, 0x5f, 0x8a);
static const lv_color_t COL_BG_ROW   = LV_COLOR_MAKE(0x18, 0x18, 0x18);
static const lv_color_t COL_TXT_HI   = LV_COLOR_MAKE(0xff, 0xff, 0xff);
static const lv_color_t COL_TXT_LO   = LV_COLOR_MAKE(0x9a, 0x9a, 0x9a);

/* Rows available on the current page, including the trailing "<- back" row on
 * the sub-pages. Kept in one place so the cursor clamp and the painter can
 * never disagree about how far the knob scrolls. */
/* ---- the shape of an app's page (ruling D12) -------------------------------
 *
 * Four small predicates, and one rule under them all: a row the owner cannot
 * reach is a bug, so the only rows ever hidden are the ITEMS of a group that is
 * shut, and an ITEM anywhere the shape does not allow is drawn as a plain row.
 * The malformed cases are a leading ITEM and an ITEM with no group above it —
 * both are the app's mistake, and silently swallowing the row would turn it into
 * "the knob does nothing". It is logged once, at entry, where the row is new. */

/** The group that owns idx, or -1 when nothing above it is a group. */
static int owner_group(int rows, int idx)
{
    for (int g = idx - 1; g >= 0; g--)
        if (orb_app_opt_kind(s_app, g) == OPT_GROUP) return g;
    return -1;
}

/** What to DRAW at idx: the app's own answer, except for an ITEM that belongs to
 *  no group, which is a row like any other. */
static orb_opt_kind_t drawn_kind(int rows, int idx)
{
    (void)rows;
    const orb_opt_kind_t k = orb_app_opt_kind(s_app, idx);
    if (k != OPT_ITEM) return k;
    return (owner_group(rows, idx) < 0) ? OPT_ROW : OPT_ITEM;
}

/** True when idx must not be painted: an ITEM whose group is the one that is closed. */
static bool row_hidden(int rows, int idx)
{
    if (drawn_kind(rows, idx) != OPT_ITEM) return false;
    return owner_group(rows, idx) != s_group_open;
}

/** How many of an app's rows the glass shows, given which group is open. */
static int visible_rows(int rows)
{
    int n = 0;
    for (int i = 0; i < rows; i++) if (!row_hidden(rows, i)) n++;
    return n;
}

/** The app row behind drawn position v, or -1. */
static int vis_to_row(int rows, int v)
{
    for (int i = 0; i < rows; i++) if (!row_hidden(rows, i)) { if (v-- == 0) return i; }
    return -1;
}

/** The drawn position of app row r, or -1 when it is hidden. */
static int row_to_vis(int rows, int r)
{
    if (r < 0 || r >= rows || row_hidden(rows, r)) return -1;
    int v = 0;
    for (int i = 0; i < r; i++) if (!row_hidden(rows, i)) v++;
    return v;
}

/** Keep the cursor inside the list. The rows can shrink under it — the card
 *  pulled, the app's walk landed, a group closed — and a highlight on a row that
 *  is not on the glass is the knob appearing not to work: the next detent moves
 *  from somewhere the owner cannot see. Called by every repaint and by the
 *  service() reap, which are the two moments the count is allowed to change. */
static void cursor_fit(int rows)
{
    if (s_cur >= rows) s_cur = rows ? rows - 1 : 0;
    if (s_cur < 0) s_cur = 0;
}

/** Say it once per visit, not once per repaint: a shape this wrong is the app's
 *  bug and the log is where the person who owns that app will look for it. */
static void shape_check(int rows)
{
    for (int i = 0; i < rows; i++) {
        if (orb_app_opt_kind(s_app, i) == OPT_ITEM && owner_group(rows, i) < 0) {
            ESP_LOGW(TAG, "\"%s\" row %d is a sub-menu item with no header above it — "
                          "drawn as a plain row", s_app->slug ? s_app->slug : "?", i);
            return;
        }
    }
}

/* The top page's sub-table, in glass order. The clock is NOT among them: B2
 * rules the clock app supplies the theme picker and the shell only renders it,
 * so it arrives through the dynamic app rows. APPS and ABOUT landed here with
 * P5.3 — a new row is one entry plus its page row, with no switch anywhere to
 * open.
 *
 * ABOUT leads (owner ruling, 2026-11-01). It took the AP row, a stub that
 * never worked (a push-input cannot type a passphrase): ABOUT opens on a QR
 * code for the orb's own page, which is where settings grow from here on, so
 * the first row of the menu is the way to a phone. */
static const menu_row_t s_top[] = {
    { "ABOUT",       PG_ABOUT,       NULL },
    /* TZ is gone from the dial (owner ruling, 2026-11-01): the home location
     * sets the zone, and it is set from the hub, where a map and a city search
     * are a good experience and a character-per-press editor is not. Every
     * back_cur literal into this run moved up one with it. */
    { "DISPLAY",     PG_DISPLAY,     NULL },
    /* UNITS (weather-plan §2.1): one OS setting every app reads, set here or from
     * the hub. */
    { "UNITS",       PG_UNITS,       NULL },
    /* APPS is every app's one place (owner, 2026-11-07: "3 different places now
     * where apps show up ... way too disjointed"): the list, and per app one flat
     * page of its settings, its permissions and its doors. PERMISSIONS and the
     * per-app rows that followed exit are gone into it. The '*' this row can carry
     * is the refusal signal, not an ask (D19): an app the gate closed this boot. */
    { "APPS",        PG_APPS,        NULL },
};

/* §58: a row that is a sentence rather than an action ("More on Orb Manager") shows
 * it in a box over the list. While it is up the knob answers nothing else: a press,
 * a turn or a hold closes it, and the page under it is exactly as it was. */
static char s_note[200];

/* ---- the page table, and what its rows answer ------------------------------
 *
 * One block per page, in table order: its count, then its labels. What a page
 * draws and how many rows it has were the switches' two worst disagreements by
 * construction — row_text answered from its own switch arm and page_rows sized
 * it from another. They sit together here now, and "<- back" is named once per
 * page where the count says it exists. The top page's labels are the sub-table
 * plus the dynamic app run plus exit; the app page's are the registry's opt_*
 * answers through the D12 shape predicates above, which stay untouched. */

static int rows_top(void)     { return TOP_OS_N + app_rows() + 1; }   /* +1: exit */
static int rows_display(void) { return 3; }                     /* dim, timeout, back */
static int rows_units(void)   { return ORB_UNITS_KINDS + 1; }   /* temp, speed, dist, back */

/* The app counts every row it has; the shell decides how many of them are on
 * the glass, which is the whole of ruling D12 and the reason the count below
 * is not the app's number handed straight back. The +1 is the back row, and it
 * is outside the group: leaving a page must never depend on whether a header
 * happens to be open. */
static int rows_app(void)
{
    if (!s_app) return 0;
    return visible_rows(orb_app_opt_count(s_app)) + own_rows() + 1;
}

static void label_top(int row, char *out, size_t outsz, bool *sel_hi)
{
    out[0] = '\0';
    if (sel_hi) *sel_hi = false;
    /* Rows after the OS's own are the service pages, labelled with their TITLE;
     * the row after those is exit. Two ways to get this wrong and both read as
     * the knob being ignored: reading the dynamic run without bounds and letting
     * the exit case answer "exit" on a row that is really a page, and testing
     * only `>= TOP_OS_N` and sending the exit row looking for a page that is not
     * there. */
    if (row >= TOP_OS_N && row < TOP_EXIT) {
        const orb_app_t *a = app_row(row - TOP_OS_N);
        if (!a) { snprintf(out, outsz, "?"); return; }   /* unreachable: rows_top sized it */
        /* A service row answers its own state, asked through the same option
         * hooks that fill its page (D36): row 0's value is the one line that
         * says what the feature is doing right now, and the title alone says it
         * when there is nothing to report. A face row keeps the bare title,
         * exactly as before — its state is the glass itself. */
        const char *v = orb_app_is_menu_only(a) ? orb_app_opt_value(a, 0) : NULL;
        if (v && v[0])
            snprintf(out, outsz, "%s  (%s)", a->title ? a->title : a->slug, v);
        else
            snprintf(out, outsz, "%s", a->title ? a->title : a->slug);
        return;
    }
    if (row >= TOP_OS_N) { snprintf(out, outsz, "exit"); return; }
    const menu_row_t *r = &s_top[row];
    /* The marker is the refusal signal, not an ask: any app the gate closed
     * this boot leaves one '*' on the APPS row, and the owner reads it here
     * rather than being interrupted on the dial (D19). Nothing waits on an
     * answer. The row is the one that opens the app list — keyed to
     * its target, not to an index, so inserting a row above it moves the
     * marker along with the row. */
    if (r->to == PG_APPS && perms_denied())
        snprintf(out, outsz, "%s  *", r->label);
    else
        snprintf(out, outsz, "%s", r->label);
}

/* Which page a "<- back" row returns to, and on which row: both come from the
 * table row, which is where the count, the parent and the landing cursor live
 * together. The dynamic answers are the ordinal functions — a service page
 * lands on the top page's row that opened it (wherever that sits now), a card
 * file's doors on its row of APPS — and an app's own page, which go_back routes
 * to its row of APPS itself. An ordinal runs
 * before the leaving page's leave(), so it can still read the page's state.
 * go_back stays a name because the picker arms and the sentinel both speak it. */
static void go_back(void)
{
    /* An app's own page came from APPS, not from the top page's service run, and
     * lands on the app's row there (perm_row_of answers 0 for an app since gone). */
    if (s_pg == P(PG_APP) && s_app == &orb_app_themes_row && s_theme_of) {
        const orb_app_t *a = s_theme_of;       /* a Themes sub-menu: back to its app */
        s_theme_of = NULL;
        reopen_app(a, OWN_THEMES);
        return;
    }
    if (s_pg == P(PG_CAPS)) {                  /* the Permissions sub-menu likewise */
        reopen_app(orb_app_row((size_t)s_perm_slot), OWN_PERMS);
        return;
    }
    if (s_pg == P(PG_APP) && s_app && !orb_app_is_menu_only(s_app)) {
        go_to(PG_APPS, perm_row_of(s_app));
        return;
    }
    go_to(s_pg->back_to, s_pg->ordinal ? s_pg->ordinal() : s_pg->back_cur);
}

/* The sub-table's size, asked rather than spelled: the dispatcher needs it to
 * tell the OS rows from the dynamic run, and TOP_OS_N above is the same
 * sizeof from the other side. The agreement is one definition deep. */
static int top_os_rows(void) { return TOP_OS_N; }

/* Which of the dynamic rows a's own row is, or 0 when it is not in the run
 * (an app can stop declaring options between the press and the back). */
static int app_row_index(const orb_app_t *a)
{
    for (int i = 0; app_row(i); i++)
        if (app_row(i) == a) return i;
    return 0;
}

static void label_app(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (!s_app) return;
    const int rows = orb_app_opt_count(s_app);
    if (row >= visible_rows(rows)) {
        label_own(row - visible_rows(rows), out, outsz, sel_hi);
        return;
    }
    /* The index the cursor holds is a POSITION ON THE GLASS, and the app answers
     * by its own row number. vis_to_row() is the only place that translation
     * happens, which is what keeps a closed group from being four different
     * functions' idea of a row to skip. */
    const int ar = vis_to_row(rows, row);
    if (ar < 0) { snprintf(out, outsz, "?"); return; }  /* unreachable: rows_app sized it */
    const orb_opt_kind_t k = drawn_kind(rows, ar);
    /* Label then value, in the shell's own words, off the app's answers: this is
     * the one line every options page shares, which is why the dial's theme page
     * and the browser's copy cannot drift. A group's value is the summary the
     * app gives for what is inside it, and nothing else: the shell does not
     * overwrite it with the open/closed state, because an always-open list
     * makes that fact echo the glass, and the owner asked the count instead
     * (2026-10-26). Items are indented, because a list that nests has to say
     * so on the glass and not only in the ABI. */
    const char *v = orb_app_opt_value(s_app, ar);
    if (v && v[0])
        snprintf(out, outsz, "%s%s  (%s)", k == OPT_ITEM ? "  " : "",
                 orb_app_opt_label(s_app, ar), v);
    else
        snprintf(out, outsz, "%s%s", k == OPT_ITEM ? "  " : "",
                 orb_app_opt_label(s_app, ar));
}
/* The units page: one row per quantity, each knob-edited through its choices. */
static const char *const UNIT_ROW[ORB_UNITS_KINDS] = { "temperature", "speed", "distance", "time", "pressure" };
static int unit_now(int kind)
{
    orb_units_t u;
    orb_units_get(&u);
    return kind == ORB_UNITS_TEMP ? u.temp : kind == ORB_UNITS_SPEED ? u.speed :
           kind == ORB_UNITS_TIME ? u.time : kind == ORB_UNITS_PRESSURE ? u.pressure : u.dist;
}
static void label_units(int row, char *out, size_t outsz, bool *sel_hi)
{
    (void)sel_hi;
    if (row < ORB_UNITS_KINDS)
        snprintf(out, outsz, "%s:  %s", UNIT_ROW[row], orb_units_label(row, unit_now(row)));
    else
        snprintf(out, outsz, "<- back");
}

static void label_display(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (row == 0) {
        snprintf(out, outsz, "dim after idle:  %s",
                 idle_dim_enabled() ? "on" : "off");
        if (sel_hi) *sel_hi = idle_dim_enabled();
    } else if (row == 1) {
        char lab[16];
        timeout_label(lab, sizeof(lab));
        snprintf(out, outsz, "timeout:  %s", lab);
    } else {
        snprintf(out, outsz, "<- back");
    }
}


/* The headers. The app page answers with the app's own title, and the caps
 * page with that title qualified — a page about one app must never be read as
 * the list it came from. Static buffer, one writer, the LVGL task: same
 * reasoning as every painted string in this file. */
static const char *title_top(void)  { return "SETTINGS"; }   /* §61: the carousel calls it Settings */
static const char *title_app(void)
{
    static char t[48];
    if (s_app == &orb_app_themes_row && s_theme_of) {     /* §58: a Themes sub-menu */
        snprintf(t, sizeof t, "%.30s THEMES",
                 s_theme_of->title ? s_theme_of->title : s_theme_of->slug);
        return t;
    }
    return (s_app && s_app->title) ? s_app->title : "OPTIONS";
}
static const char *title_display(void) { return "DISPLAY"; }
static const char *title_units(void)   { return "UNITS"; }

/* The hint says what the two gestures do HERE, because a sub-menu that does
 * not name the gesture that leaves it is a page the owner is stuck on until
 * they hold the knob by accident (C8's lesson, from the other side). NULL
 * means the plain "push: open" — the top page, the app list, the landing
 * card nobody presses. The app page's own hint lost its second string on
 * 2026-10-26: back is a row and the group header is inert, so the plain
 * sentence is true again. */
static const char *hint_app(void)
{
    /* The plain answer. Back is a row, so the exit needs no clause; the group
     * header is inert, so the hint must not promise it answers a press. An app's
     * own page carries the removals too, but their second press is said by the
     * first one's prompt: a two-line hint here sat over the page's last row
     * (Gold, 2026-11-07), which a page this long always scrolls onto. */
    return "rotate: choose   push: change";
}
static const char *hint_display(void) { return "rotate: choose   push: change"; }
static const char *hint_units(void)   { return "rotate: choose   push: change"; }

/* The presses that answer without a page of their own: each posts the one job
 * the row names and repaints — showing "working..." while the single worker
 * gets to it. Nothing that can block runs on the task that paints the dial:
 * the dim setter nvs_commits, and the grant writes the broker's word. */
static void act_display(int row)
{
    /* The timeout row never lands here: it is knob-edited (edit_display). */
    if (row == 0) { job_post(job_dim_en, 0); repaint(); }
}
/* ---- P5.3: APPS, its doors, and ABOUT (plan §7 items 2 and 4) --------------
 *
 * The catalog is the registry's installed tier (perm_rows/perm_row), because `orb_app_at()` hides the
 * shell's own entries and every faulted app, and a page whose job is the
 * owner's doors cannot afford to hide a faulted one — it is exactly the row
 * they come to retry.
 *
 * An app is installed or it is not (§43.3), so the row says one of three things:
 * `on the dial` (the entry the glass is showing), `installed` (on the carousel,
 * not showing), and `failed: <reason>` (C2's fault record, shown with its reason
 * verbatim — the orb_theme_scan rule that a row which cannot be used is still a
 * row that must be explained).
 *
 * `off` is the word now, and the word it replaced was a lie. This row read
 * `on card` since P5.3, naming the drop zone an app is copied FROM rather than
 * the state it is in: the bytes an installed row is drawn from live in the
 * `apps` partition (resolve_boot reads the slot headers), and the card copy is
 * only the transport — which is exactly why uninstall says "the card copy
 * stays". An owner who empties /sd/apps over USB and opens APPS then sees rows
 * claiming a location that has nothing in them (bench, orb-B, 2026-10-24, after
 * a host-side delete-all). `off` is the switch's own word, the same one the
 * door one row below uses and the same one the /apps page prints as
 * "turned off", so one state has one name across both surfaces.
 *
 * One press cannot carry the doors, so an app row opens PG_APPACT rather than
 * cycling anything. B5's cap shows here as a row of its own: an owner at 8 of 8
 * needs the page to say so before they look for something to uninstall.
 *
 * D38 grew the catalog past the registry. The dial's verbs are the hub's —
 * install, uninstall, remove from card — and install needs the card files that
 * are NOT on the dial, so the page now walks the card on the worker and lists
 * the snapshot's uninstalled rows under the installed ones. The snapshot is
 * the only card fact any page function reads: a two-second walk landing after
 * the owner rotated away can only ever enlarge a row set the paint already
 * owns, never repaint a page under the cursor. */
/* Forward: the page table and rows_apps() name these before this point's
 * definitions, exactly as the top of the file forwards the other page hooks.
 * The ACT_* enum and the helpers are defined below, beside their siblings. */
static int  card_rows_n(void);
static int  appact_id(int row);
static int  appact_n(void);
static const orbapp_scan_entry_t *card_subject(void);
static bool slug_on_dial(const char *slug);

static int rows_apps(void)   { return perm_rows() + card_rows_n() + 3; } /* +cap, +rotate, +back */
static int rows_appact(void) { return appact_n() + 1; }                  /* this subject's doors + back */
static const char *const ABOUT_CREDITS[] = {
    "maps  OpenStreetMap contributors",
    "openstreetmap.org/copyright",
    "tiles  OpenMapTiles, OpenFreeMap",
    "radar  LibreWXR (CC BY 4.0)",
    "weather  Open-Meteo (CC BY 4.0)",
};
#define ABOUT_CREDITS_N ((int)(sizeof ABOUT_CREDITS / sizeof ABOUT_CREDITS[0]))
static int rows_about(void)  { return 5 + ABOUT_CREDITS_N + 1; }   /* facts, credits, back */

/* ABOUT's first row is the address, and with the cursor on it (the page opens
 * there) the glass shows a QR code for the orb's own page instead of the list:
 * a phone on the same Wi-Fi scans it and lands where the settings live. An
 * orb with no address has nothing to point at, so the row stays a row. */
enum { ABOUT_ROW_ADDR = 0 };
static bool about_qr_on(void)
{
    return s_armed && s_pg == P(PG_ABOUT) && s_cur == ABOUT_ROW_ADDR && orb_net_ip_str()[0];
}

/* The subject PG_APPACT is showing, by registry index when it is on the dial
 * (an installed app's own page, PG_APP, sets it too, so its doors are these), and by snapshot index when it is a card file
 * the dial does not run. The two two-press arms are held as the subject's
 * token: a subject that can no longer be pointed at cannot fire. */
static int s_door_slot  = -1;
static int s_doormode   = 0;                      /* 0 registry row, 1 card file */
static int s_card_sub   = -1;
static int s_uninst_arm = -1;
static int s_cardrm_arm = -1;

/* The card walk's snapshot — the menu's private copy of h_apps_list's card
 * tier, sized at the listing's own cap. s_card_n reads -1 until the first walk
 * lands, and until then the catalog is the registry alone. */
EXT_RAM_BSS_ATTR static orbapp_scan_entry_t s_card_ent[ORB_APP_SCAN_MAX];   /* PSRAM: internal RAM is the DMA ring's (2026-11-09) */
static int s_card_n = -1;

/* The pending press's subject, stashed at press time for the worker: jobs
 * carry an int and names do not fit in one. The queue runs one job at a time
 * and job_post drops a press while one is outstanding, so at most one set is
 * ever pending. */
static char s_pend_stem[ORB_FILES_MAX_NAME + 1];
static char s_pend_slug[ORBAPP_SLUG_MAX];
static char s_pend_title[ORBAPP_TITLE_MAX];

/* A card row is a catalog row when the slug it claims holds no dial row, and
 * a refused file — header unreadable — is listed too, so the owner can see and
 * erase the file the page will not install. */
static bool slug_on_dial(const char *slug)
{
    const orb_app_t *a = (slug && slug[0]) ? orb_app_find_slug(slug) : NULL;
    return a && a->installed;
}
static int card_rows_n(void)
{
    if (s_card_n < 0) return 0;
    int n = 0;
    for (int i = 0; i < s_card_n; i++)
        if (!slug_on_dial(s_card_ent[i].slug)) n++;
    return n;
}
static const orbapp_scan_entry_t *card_row_at(int i)
{
    if (s_card_n < 0) return NULL;
    for (int k = 0; k < s_card_n; k++) {
        if (slug_on_dial(s_card_ent[k].slug)) continue;
        if (i-- == 0) return &s_card_ent[k];
    }
    return NULL;
}
static int card_row_pos(const orbapp_scan_entry_t *e)
{
    if (!e || s_card_n < 0) return 0;
    int i = 0;
    for (int k = 0; k < s_card_n; k++) {
        if (slug_on_dial(s_card_ent[k].slug)) continue;
        if (&s_card_ent[k] == e) return i;
        i++;
    }
    return 0;
}

/* The APPACT subject, re-resolved on every read: a file the hub installed or
 * erased between two repaints retires its door page the same way an
 * uninstalled app's registry row retired it before D38. */
static const orbapp_scan_entry_t *card_subject(void)
{
    if (!s_doormode || s_card_n < 0 || s_card_sub < 0 || s_card_sub >= s_card_n)
        return NULL;
    const orbapp_scan_entry_t *e = &s_card_ent[s_card_sub];
    return slug_on_dial(e->slug) ? NULL : e;
}
static int door_subject(void) { return s_doormode ? 1000 + s_card_sub : s_door_slot; }

/* The doors this subject answers, in the order an owner wants them. A registry
 * row always carries both removals, led by retry when the OS stopped it after two
 * faults (C2). The old "show on the dial" door went 2026-11-07 (owner): the app list
 * and the carousel open apps, and the door swapped the glass behind the router's
 * back. The removals do not depend on the state, because an app the owner cannot
 * use is still an app they may want gone. A card
 * file gets install plus remove-from-card, which the worker answers honestly when
 * the card holds nothing. Install and uninstall are mutually exclusive by
 * construction: the dial holds a runnable slot or it does not. */
enum { ACT_INSTALL = 0, ACT_UNINSTALL, ACT_CARDRM };

static int appact_id(int row)
{
    int ids[4], n = 0;
    if (s_doormode) {
        const orbapp_scan_entry_t *e = card_subject();
        if (!e) return -1;
        if (e->err == ESP_OK) ids[n++] = ACT_INSTALL;
        ids[n++] = ACT_CARDRM;
    } else {
        const orb_app_t *a = s_door_slot >= 0 ? orb_app_row((size_t)s_door_slot) : NULL;
        if (!a) return -1;
        /* The two removals every registry row owns, led by "retry" — C2's fault
         * clear — only when the OS stopped the app after two faults. There is no
         * off state (§43.3).
         * The removals stand whether or not the row runs — an owner who cannot use
         * an app is an owner who may want it gone. The card door rides the worker's
         * honesty, not a pre-check here: `g_scan_n == 0` at press time means the
         * walk had not landed on this page yet, and the door answers "the card held
         * no copy" rather than the page guessing (bench, orb-B 2026-10-24). */
        if (a->failed) ids[n++] = ACT_INSTALL;
        ids[n++] = ACT_UNINSTALL;
        ids[n++] = ACT_CARDRM;
    }
    return (row >= 0 && row < n) ? ids[row] : -1;
}
static int appact_n(void)
{
    int n = 0;
    while (appact_id(n) >= 0) n++;
    return n;
}

/* Cancel the armed removals, and take their prompts off the status row with
 * them. The arm and the yellow prompt are one fact — this row awaits its
 * second press — so every cancel clears both, or the glass keeps offering a
 * press the arm no longer honors. Bench, orb-B 2026-10-24: the owner pressed
 * uninstall, rotated away without confirming, and the row went back to plain
 * `uninstall` under a line still reading "press uninstall again to confirm".
 *
 * The line goes ONLY while it is still a removal arm's own words. Anything the
 * worker published since the first press overwrote it, and that answer belongs
 * to the owner: rotating the knob is not a reason to lose a refusal they have
 * not read yet. So the test is on the text, not on a flag that would have to
 * be kept honest at every other write. */
static void unarm_arms(void)
{
    s_uninst_arm = -1;
    s_cardrm_arm = -1;
    if (strstr(s_ui.status, "press uninstall again to confirm") ||
        strstr(s_ui.status, "press again to erase the card copy"))
        s_ui.status[0] = '\0';
}

static int ord_appact(void)
{
    /* Which APPS row this subject is on, so back lands where the owner came
     * from even if the row moved underneath them. A card file's ordinal sits
     * after the registry run, in the catalog's own card tier. */
    if (s_doormode) return perm_rows() + card_row_pos(card_subject());
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    return a ? perm_row_of(a) : 0;
}

/* Which entry the glass is showing, by the registry's own answer plus the row's
 * own active bit: one rule, asked in one place, so the catalog's "on the dial"
 * and the door page's "showing on the dial" cannot start disagreeing about who
 * is on the glass. */
static bool app_on_glass(const orb_app_t *a)
{
    return a == orb_app_current() || a->active;
}

static void label_apps(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const int n = perm_rows(), cn = card_rows_n();
    if (row >= n + cn) {
        if (row == n + cn) {   /* B5's cap, on the glass rather than in a manual */
            snprintf(out, outsz, "%d of %d slots used", n, ORB_APP_CAP);
            return;
        }
        if (row == n + cn + 1) {   /* §59: the rotation, knob-edited (edit_apps) */
            char t[24];
            rot_text(rot_choice(), t, sizeof t);
            snprintf(out, outsz, "Rotate apps  (%s)", t);
            return;
        }
        snprintf(out, outsz, "<- back");
        return;
    }
    if (row >= n) {                              /* D38: the card tier */
        const orbapp_scan_entry_t *c = card_row_at(row - n);
        if (!c) { snprintf(out, outsz, "?"); return; }
        snprintf(out, outsz, c->err == ESP_OK ? "%.42s  on the card" : "%.42s  cannot install",
                 c->title[0] ? c->title : c->stem);
        return;
    }
    const orb_app_t *a = perm_row(row);
    if (!a) { snprintf(out, outsz, "?"); return; }    /* rows_apps sized it */
    if (a->failed) {
        /* Verbatim from the broker's fault record — the same sentence the
         * ledger and the /apps page print. */
        snprintf(out, outsz, "%s  failed: %s", a->title ? a->title : a->slug,
                 orb_svc_fault_reason(a));
        return;
    }
    const bool on_glass = app_on_glass(a);
    snprintf(out, outsz, "%s%s  %s", orb_svc_denied_mask(a) ? "* " : "",
             a->title ? a->title : a->slug, on_glass ? "on the dial" : "installed");
}

static void act_apps(int row)
{
    const int n = perm_rows(), cn = card_rows_n();
    if (row >= n + cn) { go_back(); repaint(); return; }       /* the cap/back rows */
    if (row >= n) {
        /* A card file's door page, armed the same way the registry rows arm
         * theirs — the subject is this page's business, set here and read by
         * every appact function through card_subject(). */
        const orbapp_scan_entry_t *c = card_row_at(row - n);
        if (!c) { go_back(); repaint(); return; }
        s_doormode = 1;
        s_card_sub = (int)(c - s_card_ent);
        s_door_slot = -1;
        go_to(PG_APPACT, 0);
        repaint();
        return;
    }
    const orb_app_t *a = perm_row(row);
    if (!a) { go_back(); repaint(); return; }
    /* An installed app's one page (2026-11-07): its settings, permissions and
     * doors, armed through PG_APP's own open hook. */
    s_pages[PG_APP].open(a);
    repaint();
}

static const char *title_apps(void) { return "APPS"; }

/* §59: the Rotate apps row edits by knob through its interval choices, off first. */
static void fmt_apps(int row, int32_t v, char *out, size_t outsz)
{
    (void)row;
    char t[24];
    rot_text((int)v, t, sizeof t);
    snprintf(out, outsz, "Rotate apps  (%s)", t);
}
static void commit_apps(int row, int32_t v) { (void)row; job_post(job_rot_every, (int)v); }
static bool edit_apps(int row, menu_edit_t *e)
{
    if (row != perm_rows() + card_rows_n() + 1) return false;
    e->r = (knob_range_t){ 0, ROT_N - 1, false, 1 };
    e->v = rot_choice();
    e->fmt = fmt_apps;
    e->commit = commit_apps;
    return true;
}
static const char *hint_apps(void)  { return "rotate: choose   push: open"; }

/* Leaving the list ends the door page's claim on a subject and both arms with
 * it: an arm that outlived its page would remove on the next press anywhere.
 * The entry asks the worker for a card walk, so the install tier is as fresh
 * as the last time the owner stood on this page; the walk lands by repaint,
 * never under the cursor (see the snapshot rule above). */
static void enter_apps(void)
{
    s_theme_of   = NULL;
    s_door_slot  = -1;
    s_doormode   = 0;
    s_card_sub   = -1;
    unarm_arms();
    job_post(job_scan, 0);
}

/* The doors this subject answers, in the order appact_id() lays them out. D38
 * cut the enable switch and §43.3 the state with it: an app is installed or
 * not, a faulted one gets a retry row (C2's fault clear), and every subject
 * gets uninstall and
 * remove-from-card. The row count and label are one function, so a page cannot
 * draw a door its press would refuse. */
static void label_door(int id, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const bool armed_u = (s_uninst_arm == door_subject());
    const bool armed_c = (s_cardrm_arm == door_subject());
    switch (id) {
    case ACT_INSTALL: {
        /* The label follows the press: a card file installs; a faulted row retries
         * (C2's fault clear). An app is installed or not — there is no third word. */
        const orb_app_t *a = s_doormode ? NULL : orb_app_row((size_t)s_door_slot);
        snprintf(out, outsz, (a && a->failed) ? "retry" : "install");
        return;
    }
    case ACT_UNINSTALL:
        /* The arm is stated on the row, not only in the status line: the owner
         * who walks back to this row after a detent sees the press is armed. */
        snprintf(out, outsz, "uninstall%s", armed_u ? " - press again" : "");
        return;
    case ACT_CARDRM:
        snprintf(out, outsz, "remove from card%s", armed_c ? " - press again" : "");
        return;
    default: snprintf(out, outsz, "<- back"); return;
    }
}

static const char *title_appact(void)
{
    static char act_title[48];
    if (s_doormode) {
        const orbapp_scan_entry_t *e = card_subject();
        snprintf(act_title, sizeof act_title, "%.42s APP",
                 e ? (e->title[0] ? e->title : e->stem) : "?");
        return act_title;
    }
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    snprintf(act_title, sizeof act_title, "%s APP",
             a && a->title ? a->title : a ? a->slug : "?");
    return act_title;
}
/* Two rows remove something for good — uninstall and remove-from-card — so the
 * hint names the second press rather than letting a single push stand for
 * consent on either. */
static const char *hint_appact(void) { return "rotate: choose   push: run   removals need two presses"; }

/* slot in the high bits, a one-bit action low. Same shape as cap_pack
 * for the same reason: this is the whole contract between a press and a write,
 * and the host suite builds packs against it directly. */
static int door_pack(const orb_app_t *a, int on)
{
    return (orb_app_index_of(a) << 2) | (on ? 1 : 0);
}

/* Stash the subject's identity for the worker: the arm prompt, the outcome
 * line, and the removal itself all need the name, and jobs cross the task
 * boundary carrying an int. Call on the LVGL task, at press time, before the
 * door's own gate can move the cursor. */
static void appact_stash(void)
{
    s_pend_stem[0] = s_pend_slug[0] = s_pend_title[0] = '\0';
    if (s_doormode) {
        const orbapp_scan_entry_t *e = card_subject();
        if (!e) return;
        strlcpy(s_pend_stem,  e->stem,  sizeof s_pend_stem);
        strlcpy(s_pend_slug,  e->slug,  sizeof s_pend_slug);
        strlcpy(s_pend_title, e->title, sizeof s_pend_title);
        return;
    }
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    if (!a) return;
    strlcpy(s_pend_slug, a->slug, sizeof s_pend_slug);
    /* The card file an installed app came from is filed under its slug — that
     * is h_app_upload's own naming rule — so stem and slug agree here, and the
     * remove-from-card job re-walks to catch a file filed some other way. */
    strlcpy(s_pend_stem, a->slug, sizeof s_pend_stem);
    if (a->title) strlcpy(s_pend_title, a->title, sizeof s_pend_title);
}
static const char *appact_name(void)
{
    return s_pend_title[0] ? s_pend_title
         : s_pend_slug[0]  ? s_pend_slug
         : s_pend_stem[0]  ? s_pend_stem : "?";
}

static void label_appact(int row, char *out, size_t outsz, bool *sel_hi)
{
    label_door(appact_id(row), out, outsz, sel_hi);
}

static void act_door(int id);
static void act_appact(int row)
{
    const int id = appact_id(row);
    if (id < 0) { go_to(PG_APPS, 0); repaint(); return; }   /* the back row, or subject gone */
    act_door(id);
}

/* ---- one app's page: settings, then Themes and Permissions, then doors -------------
 * 2026-11-07, twice. The owner found an app in three places — PERMISSIONS, APPS and
 * its own settings row at the top — and asked for one: an installed app's PG_APP page
 * is the whole of it. A flat page with every theme and cap on it then read as too
 * much, so the two lists are sub-menus: the page is the settings its manifest shows
 * on the dial (the app's own opt_* answers, above), a Themes row and a Permissions
 * row, then the doors APPACT has always drawn, by the same ids. A card file nobody
 * installed keeps APPACT itself. The service pages (USB Drive, Sound) and the Themes
 * sub-menu are PG_APP too, and own none of this. */
static bool app_owned(const orb_app_t *a)
{
    return a && a->kind == ORB_APP_KIND_APP && a->installed && !orb_app_is_menu_only(a);
}

/* The owner caps the Permissions sub-menu lists for a: declared ones, and any it
 * was refused — the undeclared reach is the one refusal that says something about
 * the app. */
static int own_cap(const orb_app_t *a, int i)
{
    const uint32_t m = orb_svc_declared(a) | orb_svc_denied_mask(a);
    for (int c = 0; c < N_OWNER_CAPS; c++)
        if ((m & (1u << OWNER_CAPS[c])) && i-- == 0) return c;
    return -1;
}
static int own_caps_n(const orb_app_t *a)
{
    int n = 0;
    while (own_cap(a, n) >= 0) n++;
    return n;
}

/* The page's own rows after the settings, in order: Themes when the card holds any
 * of the app's, Permissions when it declared or reached for a cap, then the doors. */
static bool own_has_themes(const orb_app_t *a)
{
    /* or none on the card and a reason to say so (theme-cache-plan: why it is missing) */
    return themes_page_count_for(a->slug) > 0 || orb_theme_absent(a->slug, NULL, NULL, 0);
}
/* §59: the In rotation row stands only while the rotation is on. */
static bool own_rotating(void) { orb_rotation_t r; orb_rotation_get(&r); return r.on; }
static int own_at(int i)                     /* OWN_* or a door id, -1 past the end */
{
    if (!app_owned(s_app)) return -1;
    if (own_has_themes(s_app) && i-- == 0) return OWN_THEMES;
    if (own_caps_n(s_app) > 0 && i-- == 0) return OWN_PERMS;
    if (own_rotating() && i-- == 0) return OWN_ROT;
    return appact_id(i);
}
static int own_rows(void)
{
    int n = 0;
    while (own_at(n) != -1) n++;
    return n;
}
/** The page row of an own row, so a sub-menu's back lands on the row that opened it. */
static int own_row_of(int what)
{
    for (int i = 0; own_at(i) != -1; i++)
        if (own_at(i) == what) return visible_rows(orb_app_opt_count(s_app)) + i;
    return 0;
}

static void label_own(int i, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const int what = own_at(i);
    if (what == OWN_THEMES) {
        const char *on = themes_page_active_for(s_app->slug);
        const int n = themes_page_count_for(s_app->slug);
        const char *why = NULL;
        if (orb_theme_absent(s_app->slug, &why, NULL, 0) && why)
            snprintf(out, outsz, "Themes  (%s)", why);       /* what is missing, not a count */
        else if (on) snprintf(out, outsz, "Themes  (%.30s)", on);
        else    snprintf(out, outsz, "Themes  (%d)", n);
        return;
    }
    if (what == OWN_PERMS) {
        int nd = 0, ng = 0;
        for (int k = 0; own_cap(s_app, k) >= 0; k++) {
            const uint32_t bit = 1u << OWNER_CAPS[own_cap(s_app, k)];
            if (orb_svc_declared(s_app) & bit) nd++;
            if ((orb_svc_declared(s_app) & bit) && (orb_svc_grants(s_app) & bit)) ng++;
        }
        snprintf(out, outsz, "Permissions  (%d of %d)%s", ng, nd,
                 orb_svc_denied_mask(s_app) ? "  *" : "");
        return;
    }
    if (what == OWN_ROT) {
        const bool in = orb_rotation_takes_part(s_app->slug);
        if (sel_hi) *sel_hi = in;
        snprintf(out, outsz, "In rotation  (%s)", in ? "yes" : "no");
        return;
    }
    if (what >= 0) { label_door(what, out, outsz, sel_hi); return; }
    snprintf(out, outsz, "<- back");
}

static void act_own(int i)
{
    if (!app_owned(s_app)) { go_back(); repaint(); return; }
    const int what = own_at(i);
    if (what == OWN_THEMES) {
        const orb_app_t *a = s_app;
        if (themes_page_count_for(a->slug) == 0) { repaint(); return; }   /* only a reason */
        /* §61: the themes as a carousel over the page, which stays as it is under the
         * held glass and is where a Back or a hold returns. The text list stays the
         * answer when the carousel cannot run. */
        if (theme_strip_open(a)) return;
        go_to(PG_APP, 0);                       /* leaves this page: disarms it */
        themes_page_for(a->slug);
        s_theme_of = a;
        s_pages[PG_APP].open(&orb_app_themes_row);
        repaint();
        return;
    }
    if (what == OWN_PERMS) {
        s_perm_slot = orb_app_index_of(s_app);
        go_to(PG_CAPS, 0);
        repaint();
        return;
    }
    if (what == OWN_ROT) {
        job_post(job_rot_member, (orb_app_index_of(s_app) << 1) |
                                 (orb_rotation_takes_part(s_app->slug) ? 0 : 1));
        repaint();                                      /* shows "working..." */
        return;
    }
    if (what >= 0) act_door(what);
}

/* ---- the Permissions sub-menu: one row per cap, a press toggles it (D19) ---------- */
static int rows_caps(void)
{
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    return (a ? own_caps_n(a) : 0) + 1;
}
static void label_caps(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    const int c = a ? own_cap(a, row) : -1;
    if (c < 0) { snprintf(out, outsz, "<- back"); return; }
    const uint32_t bit = 1u << OWNER_CAPS[c];
    const bool dcl = (orb_svc_declared(a) & bit) != 0;
    const bool g   = (orb_svc_grants(a) & bit) != 0;
    const bool ref = (orb_svc_denied_mask(a) & bit) != 0;
    if (sel_hi) *sel_hi = g;           /* the highlight answers "what is the state" */
    snprintf(out, outsz, "%s:  %s%s", orb_svc_cap_name(OWNER_CAPS[c]),
             !dcl ? "not declared" : g ? "allowed" : "revoked", ref ? " *" : "");
}
static const char *title_caps(void)
{
    static char t[48];
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    snprintf(t, sizeof t, "%.30s PERMISSIONS", a && a->title ? a->title : a ? a->slug : "?");
    return t;
}
static const char *hint_caps(void) { return "rotate: choose   push: toggle"; }
static void act_caps(int row)
{
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    if (!a) { go_to(PG_APPS, 0); repaint(); return; }
    /* One press, one toggle: the broker is the single authority on what a toggle
     * may do, so an undeclared cap posts too and the line reports the refusal. */
    job_post(job_grant, cap_pack(a, own_cap(a, row)));
    repaint();                                          /* shows "working..." */
}

/* Back out of a sub-menu onto its app's page, cursor on the row that opened it; an
 * app uninstalled meanwhile lands on the list instead. */
static void reopen_app(const orb_app_t *a, int what)
{
    if (!a || !app_owned(a)) { go_to(PG_APPS, 0); return; }
    go_to(PG_APP, 0);
    s_pages[PG_APP].open(a);
    s_cur = own_row_of(what);
}

static void act_door(int id)
{
    appact_stash();
    switch (id) {
    case ACT_INSTALL:
        /* Faulted registry row: the press is C2's fault clear. Uninstalled card
         * file: the press installs it from the card, the hub's one-press shape. */
        if (s_doormode) {
            /* The copy is the worker's; the line is said now, so the press shows at
             * once rather than after a big app's card read and flash write. */
            snprintf(s_ui.status, sizeof(s_ui.status), "\"%.40s\": installing...",
                     appact_name());
            job_post(job_install_card, 0);
        } else {
            const orb_app_t *a = orb_app_row((size_t)s_door_slot);
            if (!a) { go_to(PG_APPS, 0); repaint(); return; }
            job_post(job_clear, door_pack(a, 1));
        }
        repaint();
        return;
    case ACT_UNINSTALL:
        /* Two presses, same row. Moving the cursor clears the arm (see
         * menu_view_apply_input), so a subject the owner rotated away from can
         * never fire on the next press. */
        if (s_uninst_arm != door_subject()) {
            s_uninst_arm = door_subject();
            snprintf(s_ui.status, sizeof(s_ui.status),
                     "\"%.33s\": press uninstall again to confirm", appact_name());
            repaint();
            return;
        }
        unarm_arms();
        /* Card-mode subjects carry no uninstall row (appact_id), so the row
         * that armed here always names a registry row. */
        job_post(job_uninstall, door_pack(orb_app_row((size_t)s_door_slot), 0));
        repaint();
        return;
    case ACT_CARDRM: {
        if (s_cardrm_arm != door_subject()) {
            s_cardrm_arm = door_subject();
            snprintf(s_ui.status, sizeof(s_ui.status),
                     "\"%.27s\": press again to erase the card copy", appact_name());
            repaint();
            return;
        }
        unarm_arms();
        job_post(job_cardrm, door_subject());
        repaint();
        return;
    }
    default:
        return;
    }
}

static void job_clear(void *arg)
{
    const int pack = *(const int *)arg;
    const int slot = pack >> 2;
    ui_t out; memset(&out, 0, sizeof out);
    const orb_app_t *a = orb_app_row((size_t)slot);
    /* The row can go between the press and the worker (an uninstall from the
     * browser, a reboot that resolved the slot away); say so rather than guess. */
    if (!a) { snprintf(out.status, sizeof out.status, "that press found no app"); worker_publish(&out); return; }
    /* The status line is 72 bytes and a whole title has to fit beside the
     * sentence, so the door is asked for a line that fits. */
    char why[48] = "";
    const esp_err_t e = orb_app_clear_faults(a->slug, why, sizeof why);
    snprintf(out.status, sizeof out.status, "\"%s\": %s",
             a->title ? a->title : a->slug,
             e == ESP_OK ? "fault cleared - it runs after the reboot" : why);
    worker_publish(&out);
}

/* D38: the card walk, on the worker. The snapshot is the menu's whole knowledge
 * of files the dial does not run; the walk's own refusals ride with it, so the
 * catalog can list a damaged file rather than silently omit it (the scan's own
 * rule, mirrored). A refused walk leaves the count at zero rather than stale:
 * a page that lists files the card would not name is worse than a bare tier. */
static void job_scan(void *arg)
{
    (void)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const int n = orb_app_scan(s_card_ent, ORB_APP_SCAN_MAX);
    s_card_n = n < 0 ? 0 : n;
    themes_page_walk();      /* §58: each app's Themes row is ready when its page opens */
    worker_publish(&out);
}

/* Install a card file the dial does not run — one press, the hub's shape. The
 * worker runs the ladder synchronously, as every other dial door does; an
 * installed app is on the carousel (a code app after the install's own reboot). */
static void job_install_card(void *arg)
{
    (void)arg;
    ui_t out; memset(&out, 0, sizeof out);
    char why[48] = "";
    const esp_err_t e = s_pend_stem[0]
        ? orb_app_install(s_pend_stem, why, sizeof why) : ESP_ERR_NOT_FOUND;
    if (e != ESP_OK) {
        snprintf(out.status, sizeof out.status, "\"%.30s\": %.33s",
                 appact_name(), why[0] ? why : "the install was refused");
        worker_publish(&out);
        return;
    }
    snprintf(out.status, sizeof out.status, "\"%s\": installed", appact_name());
    const int n = orb_app_scan(s_card_ent, ORB_APP_SCAN_MAX);   /* the tier moved */
    s_card_n = n < 0 ? 0 : n;
    worker_publish(&out);
}

static void job_uninstall(void *arg)
{
    const int slot = *(const int *)arg >> 2;
    ui_t out; memset(&out, 0, sizeof out);
    const orb_app_t *a = orb_app_row((size_t)slot);
    if (!a) { snprintf(out.status, sizeof out.status, "that switch found no app"); worker_publish(&out); return; }
    char why[48] = "";                       /* fits the line, as above */
    /* The name is copied first: a successful uninstall frees the row, and `a`
     * then points at a zeroed entry whose slug is NULL — formatting it took the
     * dial down on every uninstall from the menu (Gold, 2026-11-07). */
    char slug[ORBAPP_SLUG_MAX], name[ORBAPP_TITLE_MAX];
    strlcpy(slug, a->slug, sizeof slug);
    strlcpy(name, a->title ? a->title : a->slug, sizeof name);
    const esp_err_t e = orb_app_uninstall(slug, why, sizeof why);
    /* The card file survives on purpose (B4), and the line says so: an owner
     * who reads a bare "uninstalled" looks for the app on the card next time
     * and finds it, which reads as if the first refusal had been a lie. */
    snprintf(out.status, sizeof out.status, "\"%.30s\": %.33s", name,
             e == ESP_OK ? "uninstalled, the card copy stays" : why);
    if (e == ESP_OK) {                        /* the tier moved: the file is installable */
        const int n = orb_app_scan(s_card_ent, ORB_APP_SCAN_MAX);
        s_card_n = n < 0 ? 0 : n;
    }
    worker_publish(&out);
}

/* D38: remove from card. The verb's one promise is the hub's: the app leaves
 * the dial AND the card, in that order — uninstall first, so a power cut
 * between the two leaves a file the owner can reinstall from, never a slot
 * wearing a name the card no longer holds. The worker re-walks to find the
 * file rather than trusting the press-time snapshot: a hub upload may have
 * filed it under a different stem, and the card is the truth. */
static void job_cardrm(void *arg)
{
    (void)arg;
    ui_t out; memset(&out, 0, sizeof out);
    char why[48] = "";
    bool uninstalled = false;
    if (s_pend_slug[0] && slug_on_dial(s_pend_slug)) {
        const esp_err_t e = orb_app_uninstall(s_pend_slug, why, sizeof why);
        if (e != ESP_OK) {
            snprintf(out.status, sizeof out.status, "\"%.30s\": %.33s",
                     appact_name(), why[0] ? why : "the uninstall was refused");
            worker_publish(&out);
            return;
        }
        uninstalled = true;
    }
    const int n = orb_app_scan(s_card_ent, ORB_APP_SCAN_MAX);
    s_card_n = n < 0 ? 0 : n;
    int hit = -1;
    for (int i = 0; n > 0 && i < n; i++)
        if ((s_pend_slug[0] && s_card_ent[i].slug[0] &&
             strcmp(s_card_ent[i].slug, s_pend_slug) == 0) ||
            (s_pend_stem[0] && strcmp(s_card_ent[i].stem, s_pend_stem) == 0)) {
            hit = i;
            break;
        }
    if (hit < 0) {
        snprintf(out.status, sizeof out.status,
                 uninstalled ? "\"%.30s\": uninstalled; the card held no copy"
                             : "\"%.30s\": the card held no copy", appact_name());
        worker_publish(&out);
        return;
    }
    char rel[24 + ORB_FILES_MAX_NAME];
    snprintf(rel, sizeof rel, "apps/%s.orbapp", s_card_ent[hit].stem);
    int rem = 0;
    const esp_err_t e = orb_sd_fm_rm(rel, false, &rem);
    if (e != ESP_OK)
        snprintf(out.status, sizeof out.status, "\"%.30s\": the card refused the delete",
                 appact_name());
    else
        snprintf(out.status, sizeof out.status,
                 uninstalled ? "\"%.30s\": gone from the dial and the card"
                             : "\"%.30s\": erased from the card", appact_name());
    worker_publish(&out);
}

/* ABOUT is a read-only list and says so in its hint: five facts, then the data the
 * OS draws from and whose it is, no press that changes anything. Every getter here
 * is a RAM read — no NVS, no card, no queue — so the page paints instantly and
 * cannot answer "working...".
 *
 * The credits (radar-plan §2.4, weather-plan): OpenStreetMap's licence asks for
 * its address "in an easily locatable part of the application" beside the credit
 * the glass shows when a map first appears (orb_map_credit.c), and Open-Meteo's
 * and LibreWXR's data are CC BY 4.0. */
static void label_about(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    switch (row) {
    case ABOUT_ROW_ADDR:
        /* The row the page opens on, and the one that shows the QR: with the
         * cursor here the list gives way to the code (about_qr_on). */
        snprintf(out, outsz, "address  %s", orb_net_ip_str()[0] ? orb_net_ip_str() : "none (not on Wi-Fi)");
        return;
    case 1:
        snprintf(out, outsz, "firmware  %s", esp_app_get_description()->version);
        return;
    case 2:
        snprintf(out, outsz, "card  %s",
                 orb_sd_mounted() ? (orb_sd_card_id()[0] ? orb_sd_card_id() : "mounted")
                                  : "none");
        return;
    case 3:
        snprintf(out, outsz, "installed  %d of %d", perm_rows(), ORB_APP_CAP);
        return;
    case 4:
        {
            char zl[ORB_HOME_ZONE_MAX];
            orb_home_zone_label(zl, sizeof zl);
            snprintf(out, outsz, "zone  %s", zl);
        }
        return;
    default:
        if (row >= 5 && row < 5 + ABOUT_CREDITS_N) { snprintf(out, outsz, "%s", ABOUT_CREDITS[row - 5]); return; }
        snprintf(out, outsz, "<- back");
        return;
    }
}

static const char *title_about(void) { return "ABOUT"; }
static const char *hint_about(void)  { return about_qr_on() ? "scan with a phone on this Wi-Fi" : "rotate: read   push: nothing here"; }


/* ---- D38: the share card ---------------------------------------------------
 *
 * A share window parks the display's tile ring on purpose — that memory is
 * what funds the MSC storage object (orb_usb_msc.c) — so from the handover to
 * the reclaim reboot no frame reaches the glass. Before this page existed the
 * window froze on whatever the USB Drive row list happened to be: a dead menu
 * with a spinner's status line, which owners read as a lock-up. The card is
 * what the freeze preserves instead: what the orb gave, where it went, and the
 * one gesture that takes it back. It carries no rows because during a window
 * there is nothing to choose and one thing to press.
 *
 * The router calls menu_view_share_gate() on every poll while the window is
 * open (orb_app_share_gate, input_router.c): the card paints on the way in, the
 * closing words once on the way out, and any repaint that lands a normal menu
 * page in between gets the card back on the next poll. Every press or hold
 * anywhere on the dial becomes the stop. PG_SHARE is reachable from no row —
 * only the window itself puts it on the glass — and it survives disarm and
 * re-entry, because during a window it is the only page that is true. */
/* What the LAST repaint put on the glass: -1 means a normal menu page, 0 the
 * open card, 1 the closing card. The gate repaints on the edge rather than
 * trusting that a card which froze the ring is still the frame the panel holds,
 * because the home switch's own show hook and a worker result landing in
 * menu_view_service() both repaint this screen during a window — and frozen menu
 * rows are the dead door this card exists to replace. */
static int s_share_painted = -1;
/* What the glass holds while the reclaim reboot is in flight. Saying "saved" and
 * "wait for the clock" here is the whole point of the state existing: the owner's
 * last write to the card happened on the host, and the next thing they see is a
 * dial that is about to go dark on its own. */
static const char *const s_share_closing =
    "Stopping the share. Anything the computer wrote is on the card now. "
    "Wait for the clock.";

static int rows_share(void) { return 0; }
static void label_share(int row, char *out, size_t outsz, bool *sel_hi)
{
    (void)row;
    if (sel_hi) *sel_hi = false;
    snprintf(out, outsz, "%s", "press the knob to stop sharing");
}
static const char *title_share(void) { return "USB SHARED"; }
static const char *hint_share(void)
{
    /* One block, because the card paints this as its only body of words: the
     * fact (what the host has, write access named while it is true), what waits
     * inside it (so a frozen dial does not read as lost files), then the door and
     * the reboot stated with it — the stop IS the reboot, the only exit the mode
     * has, and saying so before the press is what keeps the press from reading as
     * the second half of a lock-up. Pure ASCII, so the fold pass cannot grow it in
     * the card's own 160-byte scratch. */
    return "The orb's card is the computer's disk now. Your files, themes and apps "
           "are inside it, and the computer can change them. Press the knob to stop "
           "sharing. The orb restarts and takes the card back.";
}

/** The share card's claim on the glass, one poll at a time. True means the
 *  window spent this poll's knob, so the router stops there. */
bool menu_view_share_gate(bool press)
{
#if CONFIG_ORB_USB_MSC
    if (!orb_usb_msc_window_open()) return false;
    const bool closing = orb_usb_msc_stopping();
    const int  want    = closing ? 1 : 0;
    /* Claim the page before anything can repaint this screen: the home switch
     * below runs the menu's show hook, and a menu that paints its rows or its
     * landing card over a frozen ring is the dead door this page replaced. */
    s_pg  = P(PG_SHARE);
    s_cur = 0;
    /* A boot-claim window can open under the clock, and a store that resumes
     * onto its own page can move the dial again afterwards. So the ask repeats
     * on every poll that finds it off home, not once per window. set_screen()
     * re-activates a target it already holds, so the != guard must stay. */
    if (input_router_current() != orb_app_home()) input_router_home();
    /* Paint on the edge only: the card goes up once per window, the closing
     * words once, and any repaint that landed a normal page in between — a
     * worker result, an activate's show hook — puts the card back. */
    if (s_share_painted != want) {
        repaint();
        if (!closing) ESP_LOGI(TAG, "share card up — the knob is the door");
    }
    if (press) (void)orb_usb_msc_request_stop();
    return true;                      /* the window spent this poll's knob */
#else
    (void)press;
    return false;
#endif
}



/* The pack the worker decodes: slot in the high bits, cap, wanted state last.
 * Its own function because this is the whole contract between a press on a cap
 * row and the write that follows — a wrong shift here writes another app's word
 * or another cap, silently, so the host suite builds packs against it directly
 * rather than reaching them only through the dispatcher's act_caps arm.
 *
 * The wanted state is the INVERSE of what the row shows, so the same press
 * allows a revoked cap and revokes an allowed one. */
static int cap_pack(const orb_app_t *a, int row)
{
    const orb_cap_t cap = OWNER_CAPS[row];
    const bool on = !(orb_svc_grants(a) & (1u << cap));
    return (orb_app_index_of(a) << 9) | ((int)cap << 1) | (on ? 1 : 0);
}

static void job_grant(void *arg)
{
    const int pack = *(const int *)arg;
    const int slot = pack >> 9, cap = (pack >> 1) & 0xFF, on = pack & 1;
    ui_t out; memset(&out, 0, sizeof out);
    const orb_app_t *a = orb_app_row((size_t)slot);
    if (!a) { snprintf(out.status, sizeof out.status, "that app is gone"); worker_publish(&out); return; }
    const esp_err_t er = orb_svc_set_grant(a, (orb_cap_t)cap, on);
    /* No ask to clear (D19): the row's meaning is the owner taking a capability
     * back, and the broker is the one that decides whether the toggle is even
     * legal — a cap the app never declared comes back refused, and the status
     * line says so. A refused NVS write leaves the RAM answer matching the
     * flash and the line says "not saved", the same durability/correctness
     * split the dim toggle's line makes. */
    if (er != ESP_OK)
        snprintf(out.status, sizeof out.status, "\"%s\": %s not saved (%s)",
                 a->slug, orb_svc_cap_name((orb_cap_t)cap), esp_err_to_name(er));
    else
        snprintf(out.status, sizeof out.status, "\"%s\": %s %s",
                 a->slug, orb_svc_cap_name((orb_cap_t)cap),
                 on ? "ALLOWED" : "revoked");
    worker_publish(&out);
}

/* Ask the worker for a job. Drops the request while one is outstanding — a
 * double-tap on a row must not queue two multi-second card walks, and the first
 * result repaints the list anyway. */
static void job_post(orb_job_fn fn, int idx)
{
    /* The gate is the worker's, not a local flag: with one task serving every
     * app, the menu asking "am I busy" and the theme page asking it have to get
     * the same answer, or two card walks and two NVS commits queue up behind
     * each other on a screen that shows "working..." for one of them. */
    const esp_err_t e = orb_jobs_post(fn, &idx, sizeof idx);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "job refused (%s)", esp_err_to_name(e));
}

/* ---- the glyph floor: what the shipped face can actually draw -------------
 * The dial paints with lv_font_montserrat_14 and _48, both generated with
 * `-r 0x20-0x7F,0xB0,0x2022` plus a list of FontAwesome symbol codepoints
 * (lv_font_montserrat_14.c:4). No fallback face is configured, so a codepoint
 * outside that list is not substituted — LVGL draws the missing-glyph box, and
 * mid-row it reads as broken firmware rather than as punctuation (bench, orb-B,
 * 2026-10-24: the armed uninstall row showed a box where the dash belongs).
 *
 * The fold belongs HERE, at the last point that is still the dial's own
 * decision, and not in the strings. Two reasons, both load-bearing:
 *   - The switch/uninstall doors print orb_app_install()'s refusal sentence
 *     verbatim by rule (see job_clear), and those sentences carry em
 *     dashes like every other string in this repo, because the writing style
 *     wants them. The same words are pinned against tools/orbapp.py and against
 *     tools/orb-manager's fake device, and they also print to the browser,
 *     where the glyph exists and renders correctly.
 *   - App titles arrive off the card, from a manifest an owner wrote. Whatever
 *     is in one of those is never this build's to reject at the last moment.
 *
 * So: ASCII passes untouched, the two non-ASCII codepoints the face carries
 * (U+00B0, U+2022) pass untouched, a mapped codepoint takes its ASCII twin, and
 * anything else — including torn UTF-8 — takes '?'. The fold never lengthens
 * the string in bytes (its longest expansion, U+2026 to three dots, is the same
 * three bytes), so a caller's bound on the original is still a bound here.
 *
 * Pure: no widget, no allocation, no I/O. repaint() is a no-op in the host
 * harness, so a fold that lived inside it would be unfalsifiable there — the
 * same trap repaint_lint exists for. It is lifted and pinned instead. */
typedef struct { unsigned cp; const char *sub; } glyph_fold_t;
static const glyph_fold_t s_glyph_fold[] = {
    { 0x2010, "-"  }, { 0x2011, "-"  }, { 0x2012, "-"  }, { 0x2013, "-"  },
    { 0x2014, "-"  }, { 0x2015, "-"  }, { 0x2212, "-"  },
    { 0x2018, "'"  }, { 0x2019, "'"  }, { 0x201A, "'"  }, { 0x201B, "'"  },
    { 0x201C, "\"" }, { 0x201D, "\"" }, { 0x201E, "\"" }, { 0x201F, "\"" },
    { 0x2026, "..." },
    { 0x00A0, " "  }, { 0x2007, " "  }, { 0x2009, " "  }, { 0x200A, " "  },
    { 0x202F, " "  }, { 0x3000, " "  },
};

static void dial_text(char *dst, size_t dstsz, const char *src)
{
    if (!dst || !dstsz) return;
    if (!src) { dst[0] = '\0'; return; }

    size_t o = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && o + 1 < dstsz) {
        const unsigned b = *s;
        if (b < 0x80) { dst[o++] = (char)b; s++; continue; }

        /* Long enough to decode, valid enough to trust: a lead byte with a
         * non-continuation behind it is a torn byte, and one byte of a torn
         * sequence is all this consumes before falling back to '?'. */
        unsigned cp;
        int need;
        if      ((b & 0xE0) == 0xC0) { cp = b & 0x1Fu;      need = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0Fu;      need = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07u;      need = 3; }
        else                         { dst[o++] = '?'; s++; continue; }

        unsigned v = 0;
        int i;
        for (i = 0; i < need; i++) {
            const unsigned c = s[1 + i];
            if ((c & 0xC0) != 0x80) break;
            v = (v << 6) | (c & 0x3Fu);
        }
        if (i != need) { dst[o++] = '?'; s++; continue; }
        cp = (cp << (6 * need)) | v;
        const int bytes = need + 1;

        if (cp == 0x00B0u || cp == 0x2022u) {     /* generated into the face */
            if (o + (size_t)bytes >= dstsz) break;
            for (int k = 0; k < bytes; k++) dst[o++] = (char)s[k];
            s += bytes;
            continue;
        }

        const char *sub = "?";
        for (unsigned k = 0; k < sizeof s_glyph_fold / sizeof s_glyph_fold[0]; k++)
            if (s_glyph_fold[k].cp == cp) { sub = s_glyph_fold[k].sub; break; }
        const size_t sl = strlen(sub);
        if (o + sl >= dstsz) break;               /* never cut a substitution in half */
        memcpy(dst + o, sub, sl);
        o += sl;
        s += bytes;
    }
    dst[o] = '\0';
}

/* The app's plane answer for the row under the cursor, or NULL. Pure: no
 * widget, so the host suite compiles it and the service() recheck below reads
 * the same truth the painter draws. The answer comes from the app's reaped
 * state only (opt_preview's own rule); asking is not starting — the focus
 * event that may start a read is orb_app_opt_focus, and service() never
 * focuses. */
static const void *app_preview(uint16_t *w, uint16_t *h, uint32_t *stamp)
{
    if (!s_armed || s_pg != P(PG_APP) || !s_app) return NULL;
    const int rows = orb_app_opt_count(s_app);
    const int ar = vis_to_row(rows, s_cur);
    if (ar < 0 || drawn_kind(rows, ar) != OPT_ITEM) return NULL;
    return orb_app_opt_preview(s_app, ar, w, h, stamp);
}

/* The preview plane's paint. Runs on the LVGL task from repaint() — the one
 * place that knows which row the cursor is on. The ask goes out first (the
 * focus event is the app's licence to start one worker read — opt_sel's
 * licence, spelled for the cursor); the answer drawn is app_preview's, which
 * is reaped state only: the callback that paints the page never touches the
 * card, the same rule opt_label and opt_value live under. */
/* ---- ABOUT's QR: the orb's own page, for a phone ---------------------------
 * Built with LVGL's qrcodegen into a PSRAM plane of one pixel per module and
 * drawn through the preview image, scaled up nearest. No lv_qrcode widget: it
 * would take its draw buffer from the LVGL pool, which has ~4 KB of headroom
 * (flights_view.c's note), and the menu already owns an image object that
 * scales a plane exactly this way. Black on white with the spec's four-module
 * quiet zone, because the glass around it is black and a scanner needs the
 * light border to find the code. "http://255.255.255.255/" is 23 bytes:
 * version 2 at ECC medium, so version 4 is headroom, not a limit in use. */
#define QR_VER_MAX 4
#define QR_QUIET   4
#define QR_SIDE_MAX (QR_VER_MAX * 4 + 17 + 2 * QR_QUIET)
#define QR_GLASS_PX 232                       /* the code's side on the glass, at most */
EXT_RAM_BSS_ATTR static uint16_t s_qr_px[QR_SIDE_MAX * QR_SIDE_MAX];
static lv_image_dsc_t s_qr_dsc;
static char s_qr_url[32];                     /* what s_qr_px encodes, "" = nothing */
static int  s_qr_side;                        /* modules per side, quiet zone in    */

static void qr_url(char *out, size_t outsz)
{
    snprintf(out, outsz, "http://%s/", orb_net_ip_str());
}

/* Encode once per address: the paint asks every repaint, the work happens on
 * a change. False when the URL does not fit (it always does). */
static bool qr_build(void)
{
    char url[sizeof s_qr_url];
    qr_url(url, sizeof url);
    if (s_qr_side && !strcmp(url, s_qr_url)) return true;
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VER_MAX)];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_VER_MAX)];
    s_qr_side = 0;
    if (!qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, QR_VER_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGW(TAG, "about: \"%s\" does not fit a version-%d code", url, QR_VER_MAX);
        return false;
    }
    const int size = qrcodegen_getSize(qr), n = size + 2 * QR_QUIET;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            s_qr_px[y * n + x] = qrcodegen_getModule(qr, x - QR_QUIET, y - QR_QUIET) ? 0x0000 : 0xFFFF;
    memset(&s_qr_dsc, 0, sizeof s_qr_dsc);
    s_qr_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_qr_dsc.header.w      = (uint32_t)n;
    s_qr_dsc.header.h      = (uint32_t)n;
    s_qr_dsc.header.stride = (uint32_t)n * 2u;
    s_qr_dsc.data_size     = (uint32_t)(n * n * 2);
    s_qr_dsc.data          = (const uint8_t *)s_qr_px;
    snprintf(s_qr_url, sizeof s_qr_url, "%s", url);
    s_qr_side = n;
    return true;
}

/* The plane while ABOUT's cursor is on the address. It borrows the preview
 * image, so the preview's own memory is cleared: the next app plane re-blits
 * rather than trusting a stamp that last described a QR code. */
static void paint_qr(void)
{
    s_pv_stamp = 0; s_pv_w = 0; s_pv_h = 0;
    if (!qr_build()) { lv_obj_add_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN); return; }
    const int n = s_qr_side, mod = QR_GLASS_PX / n;   /* whole pixels per module */
    lv_image_set_src(s_pv_img, &s_qr_dsc);
    lv_obj_set_size(s_pv_img, n, n);
    lv_image_set_pivot(s_pv_img, n / 2, n / 2);
    lv_image_set_scale(s_pv_img, (uint32_t)(mod * 256));
    lv_obj_center(s_pv_img);
    lv_obj_remove_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN);
}

static void paint_preview(void)
{
    if (about_qr_on()) { paint_qr(); return; }
    if (s_pg == P(PG_APP) && s_app) {
        const int rows = orb_app_opt_count(s_app);
        const int ar = vis_to_row(rows, s_cur);
        if (ar >= 0) orb_app_opt_focus(s_app, ar);
    }

    uint16_t w = 0, h = 0;
    uint32_t stamp = 0;
    const void *px = app_preview(&w, &h, &stamp);

    /* The stamp is the answer's own "this is a new plane" word; a plane whose
     * extents change without a new stamp is the same repaint trigger. A NULL
     * answer means hide: the text rows this repaint just drew stand. */
    const bool same = px && stamp == s_pv_stamp && w == s_pv_w && h == s_pv_h;
    if (!px) {
        s_pv_stamp = 0;
        if (s_pv_img) lv_obj_add_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (same) return;                    /* unchanged plane; no re-blit */

    memset(&s_pv_dsc, 0, sizeof s_pv_dsc);
    s_pv_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_pv_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;   /* the class's shape   */
    s_pv_dsc.header.w      = w;
    s_pv_dsc.header.h      = h;
    s_pv_dsc.header.stride = (uint32_t)w * 2u;         /* 2 B/px, opaque      */
    s_pv_dsc.data_size     = (uint32_t)w * (uint32_t)h * 2u;
    s_pv_dsc.data          = (const uint8_t *)px;
    s_pv_w = w; s_pv_h = h; s_pv_stamp = stamp;

    lv_image_set_src(s_pv_img, &s_pv_dsc);
    lv_obj_set_size(s_pv_img, w, h);
    lv_image_set_pivot(s_pv_img, w / 2, h / 2);
    /* The half-res square the design reserves, upscaled to the glass — nearest
     * (antialias off above): a bake doubled, not a picture resampled. */
    const int scale = (w && SCREEN_WIDTH >= w) ? (SCREEN_WIDTH * 256 / w) : 256;
    lv_image_set_scale(s_pv_img, (uint32_t)scale);
    lv_obj_center(s_pv_img);
    lv_obj_remove_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN);
}

/* Repaint from the model. Called only from the LVGL task (via apply_input or
 * service), never from the worker — which is the entire point of the queue. */
static void repaint(void)
{
    if (!s_scr) return;

#if CONFIG_ORB_USB_MSC
    /* The share card owns the glass for the whole window, through disarm and
     * re-entry alike: every paint path that lands here during the window paints
     * the card and stops. A window can start while the menu is unarmed (the boot
     * claim opens under the clock), and an unarmed branch that painted "MENU /
     * push to open" over it would put a dead door back on the frozen frame.
     * Every string here is ASCII, so the fold pass cannot grow one. */
    if (s_pg == P(PG_SHARE)) {
        char gl[160];
        for (int i = 0; i < ROW_MAX; i++) {
            lv_label_set_text(s_row_lbl[i], "");
            lv_obj_set_style_bg_color(s_row[i], lv_color_black(), 0);
        }
        /* The window's card is the whole glass: any plane the page had up goes
         * with it. The share window can open over an armed options page, and a
         * preview left on the mirror under a frozen panel is a picture of a
         * theme painting over the one fact the card exists to carry. */
        s_pv_stamp = 0;
        lv_obj_add_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN);
        const bool closing = orb_usb_msc_stopping();
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_48, 0);
        lv_label_set_text(s_title, "USB SHARED");
        /* One block of words, in the colour of the state it reports, and the
         * grey hint blank. Both bottom labels are BOTTOM_ALIGNED and wrap
         * UPWARD from their own anchor, 32 px apart, so the two sentences this
         * card used to split between them grew into each other and drew on top
         * of one another (bench, orb-B, 2026-10-25: amber over grey, neither
         * readable). The card has one body now, kept to three lines at the
         * status label's 300 px, and the door is its last sentence. */
        dial_text(gl, sizeof gl, closing ? s_share_closing : hint_share());
        lv_label_set_text(s_status, gl);
        lv_label_set_text(s_hint, "");
        s_share_painted = closing ? 1 : 0;
        lv_obj_invalidate(s_scr);
        return;
    }
#endif

    const int rows = page_rows();
    s_painted_rows = rows;
    s_share_painted = -1;            /* a normal page is on the glass now        */
    cursor_fit(rows);

    /* Window the list around the cursor so a 62-row zone table scrolls inside
     * five physical rows; clamped at both ends so the last row stays reachable
     * without the window drifting past the end of the list. */
    const int vis = rows_on_glass();
    int win = 0;
    if (rows > vis) {
        win = s_cur - vis / 2;
        if (win < 0) win = 0;
        if (win > rows - vis) win = rows - vis;
    }

    char buf[64];
    char gl[96];                                   /* fold scratch: status is 72 B */
    const bool qr = about_qr_on();                 /* the code takes the list's place */
    for (int i = 0; i < ROW_MAX; i++) {
        const int r = win + i;
        if (rows == 0 || r >= rows || qr || i >= vis || s_note[0]) {   /* the box stands alone */
            lv_label_set_text(s_row_lbl[i], "");
            lv_obj_set_style_bg_color(s_row[i], lv_color_black(), 0);
            continue;
        }
        bool mark = false;
        row_text(r, buf, sizeof(buf), &mark);
        dial_text(gl, sizeof gl, buf);
        lv_label_set_text(s_row_lbl[i], gl);
        const bool on_cursor = (r == s_cur);
        lv_obj_set_style_bg_color(s_row[i],
            on_cursor ? COL_BG_SEL : (mark ? COL_BG_ROW : lv_color_black()), 0);
        lv_obj_set_style_text_color(s_row_lbl[i], on_cursor ? COL_TXT_HI
                                 : (mark ? COL_TXT_HI : COL_TXT_LO), 0);
    }

    /* The landing is a title card: one large word and the gesture that leaves
     * it. Keyed off s_armed rather than the page, because the page is only authoritative
     * while armed — menu_view_service() can land a staged result at any moment,
     * including after the user has already rotated away, and repainting that as
     * a list would put an actionable menu on a title card. */
    if (!s_armed) {
        /* Rows are already blank: page_rows() returns 0 unarmed, so the loop
         * above never had a row index to paint. */
        lv_label_set_text(s_title, "SETTINGS");
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_48, 0);
        lv_label_set_text(s_hint, "push to open");
        s_painted_busy = orb_jobs_busy();   /* this hint never says "working..." */
        lv_label_set_text(s_status, "");
    } else {
        char head[64];
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
        title_line(head, sizeof head);
        dial_text(gl, sizeof gl, head);
        lv_label_set_text(s_title, gl);
        dial_text(gl, sizeof gl, page_hint());
        lv_label_set_text(s_hint, gl);
        char url[sizeof s_qr_url];
        if (qr) qr_url(url, sizeof url);
        dial_text(gl, sizeof gl, qr ? url : s_ui.status);
        lv_label_set_text(s_status, gl);
        /* The URL sits under the code, centred like it; status lines elsewhere
         * keep the left edge they wrap from. */
        lv_obj_set_style_text_align(s_status, qr ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    }

    /* The plane over the list, asked and drawn in the same pass that decided
     * the list: one repaint, one source of truth about which row is under the
     * cursor. Hidden on every page that is not an app's open group — a plane
     * surviving a page change is the last theme's picture over a page that
     * never mentioned it. */
    paint_preview();

    /* §58: the message box, last, so it sits over the plane too. */
    if (s_note_box) {
        if (s_armed && s_note[0]) {
            char gn[sizeof s_note + 32];       /* the fold can widen a glyph */
            dial_text(gn, sizeof gn, s_note);
            lv_label_set_text(s_note_lbl, gn);
            lv_obj_remove_flag(s_note_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_note_box);
        } else {
            lv_obj_add_flag(s_note_box, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_invalidate(s_scr);
}

/* ---- Public API ---- */

esp_err_t menu_view_init(void)
{
    /* No task of its own: the shell's single orb_jobs worker runs this
     * screen's NVS writes alongside every app's blocking work. */
    s_res_mx = xSemaphoreCreateMutex();
    if (!s_res_mx) return ESP_ERR_NO_MEM;

    /* Every object below comes from the internal heap, and this init runs at
     * boot as the shell — first built since D35, before the card tier spends
     * anything, but still behind the theme's primary bank and the display
     * stack. When that heap is
     * fragmented the largest free block can drop below what an LVGL object
     * needs — the fleet saw heap_internal_largest ~9216 here — and lv_obj_create
     * returns NULL. Pre-hardening, the next line dereferenced that NULL: a boot-
     * time panic, billed by C2 as a menu fault, twice into permanent quarantine,
     * with no UI to clear it (that UI lives in this app). Check every allocation
     * and return ESP_ERR_NO_MEM instead; orb_app_init_all already drops an app
     * whose init fails, so the menu skips the carousel this boot, draws nothing,
     * costs no fault strike, and is retried clean on the next boot. */
    s_scr = lv_obj_create(NULL);
    if (!s_scr) return ESP_ERR_NO_MEM;
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    s_title = lv_label_create(s_scr);
    if (!s_title) goto nomem;
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_title, "SETTINGS");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 60);

    /* The five row strips, centred as a column. Built once and re-labelled:
     * allocating during a scroll would fragment a heap this device has no
     * reason to touch while somebody is turning a knob. */
    const int row_h = 46, row_w = 320, row_gap = 6, row_top = 150;
    for (int i = 0; i < ROW_MAX; i++) {
        s_row[i] = lv_obj_create(s_scr);
        if (!s_row[i]) goto nomem;
        lv_obj_set_size(s_row[i], row_w, row_h);
        lv_obj_set_style_radius(s_row[i], 10, 0);
        lv_obj_set_style_pad_all(s_row[i], 0, 0);
        lv_obj_set_style_border_width(s_row[i], 0, 0);
        lv_obj_set_scrollable(s_row[i], false);
        lv_obj_set_pos(s_row[i], (SCREEN_WIDTH - row_w) / 2, row_top + i * (row_h + row_gap));

        s_row_lbl[i] = lv_label_create(s_row[i]);
        if (!s_row_lbl[i]) goto nomem;
        lv_obj_set_style_text_font(s_row_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_row_lbl[i], COL_TXT_LO, 0);
        lv_label_set_text(s_row_lbl[i], "");
        lv_obj_center(s_row_lbl[i]);
    }

    /* Result line: why a scan failed, or what was just set. It is the only place
     * the dial can tell the owner why the card did not answer, which orb_sd's
     * silent ESP_OK/ESP_FAIL otherwise leaves them to guess at.
     *
     * Width-bounded and wrapping, because the panel is round: the chord at this
     * label's bottom edge (y=400) is ~325 px, and a refusal sentence with the
     * title prefix runs ~60 characters — over 400 px on one line, its ends
     * outside the visible circle (bench, orb-B: the switch-refusal message was
     * unreadable). 300 px leaves margin inside the chord; WRAP takes the second
     * line upward, toward the wider part of the circle, not off the bottom. */
    s_status = lv_label_create(s_scr);
    if (!s_status) goto nomem;
    lv_obj_set_style_text_color(s_status, lv_color_make(0xe0, 0xb0, 0x40), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_status, 300);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -66);

    /* Same circular bound, tighter here: the chord at this hint's bottom edge
     * (y=432) is ~244 px, and the app-doors hint runs 55 characters. 230 px
     * keeps both wrap lines inside the circle; the hint growing to two lines is
     * cheaper than the word "uninstall" losing its tail. */
    s_hint = lv_label_create(s_scr);
    if (!s_hint) goto nomem;
    lv_obj_set_style_text_color(s_hint, COL_TXT_LO, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_hint, 230);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_hint, "push to open");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -34);

    /* The preview plane, created last so it paints over every other child. It
     * carries no style of its own: an RGB565 plane is opaque, and the design
     * says full-frame. The size and the source are set when a plane arrives
     * (the extent comes from the app with the answer); until then it is
     * hidden, and the pivot is its centre so a square plane scaled 2x stays
     * centred on the 466 glass. */
    s_pv_img = lv_image_create(s_scr);
    if (!s_pv_img) goto nomem;
    lv_obj_add_flag(s_pv_img, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_antialias(s_pv_img, false);      /* 2x nearest, not a resample */
    lv_obj_remove_flag(s_pv_img, LV_OBJ_FLAG_CLICKABLE);

    /* §58: the message box, over the list and under nothing. Sized to the round
     * glass: 330 px wide sits inside the chord from y=130 to y=330. */
    s_note_box = lv_obj_create(s_scr);
    if (!s_note_box) goto nomem;
    lv_obj_set_size(s_note_box, 330, 200);
    lv_obj_align(s_note_box, LV_ALIGN_CENTER, 0, -2);
    lv_obj_set_style_bg_color(s_note_box, lv_color_make(0x1c, 0x24, 0x2e), 0);
    lv_obj_set_style_border_color(s_note_box, COL_BG_SEL, 0);
    lv_obj_set_style_border_width(s_note_box, 2, 0);
    lv_obj_set_style_radius(s_note_box, 18, 0);
    lv_obj_set_style_pad_all(s_note_box, 18, 0);
    lv_obj_set_scrollable(s_note_box, false);
    lv_obj_add_flag(s_note_box, LV_OBJ_FLAG_HIDDEN);
    s_note_lbl = lv_label_create(s_note_box);
    if (!s_note_lbl) goto nomem;
    lv_obj_set_width(s_note_lbl, 290);
    lv_label_set_long_mode(s_note_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_note_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_note_lbl, COL_TXT_HI, 0);
    lv_obj_set_style_text_align(s_note_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_note_lbl, "");
    lv_obj_center(s_note_lbl);

    repaint();
    ESP_LOGI(TAG, "menu ready (worker up, %s)", orb_sd_mounted() ? "card mounted" : "no card");
    return ESP_OK;

nomem:
    /* Tear the whole half-built subtree down (deleting s_scr frees its children
     * with it), null every cached pointer, and release the mutex, so a dropped
     * init leaves no dangling screen for menu_view_screen()/repaint() to touch
     * and nothing held across the failure. The next boot starts from empty. */
    lv_obj_del(s_scr);
    s_scr = NULL; s_title = NULL; s_status = NULL; s_hint = NULL; s_pv_img = NULL;
    s_note_box = NULL; s_note_lbl = NULL;
    for (int i = 0; i < ROW_MAX; i++) { s_row[i] = NULL; s_row_lbl[i] = NULL; }
    vSemaphoreDelete(s_res_mx);
    s_res_mx = NULL;
    return ESP_ERR_NO_MEM;
}

lv_obj_t *menu_view_screen(void) { return s_scr; }

bool menu_view_armed(void) { return s_armed; }

void menu_view_show(void) { repaint(); }

/* ---- the menu's own "where was I", across entry and exit -------------------
 *
 * The dial used to open every visit on the top page at row 0. An owner three
 * pages down in the 62-row zone list, who dimmed the screen and came back, paid
 * for that scroll again every time. The router already keeps this exact record
 * for the SCREEN the glass sits on (input_router.c's page_write/page_remember,
 * one NVS string plus a RAM cache); this is the same idea one level in, for the
 * PAGE inside the menu. Same discipline: a RAM mirror so walking over the same
 * page ten times a day costs zero commits, an idempotent-by-comparison write,
 * and a read that carries no authority until it resolves.
 *
 * Only pages that re-enter from their own id are remembered: TOP, DISPLAY,
 * ABOUT, UNITS. The rest cannot be, and the reason each is left out is a
 * correctness rule, not an oversight — PG_APP/PG_APPACT name an app that may
 * have uninstalled since (their open() resolves the pointer), the retired
 * PERMS/CAPS pages open the top page instead, PG_SHARE is the window's
 * and not the owner's to return to, and PG_LANDING is unarmed. A remembered page
 * whose enter() hook re-derives the cursor (the zone list on the zone in force,
 * the timeout picker on the preset in force) takes the hook's answer, not the
 * stored row: the cursor there is a fact about the current setting, not a place
 * the owner parked it. */
#define MENU_POS_NS  "orb_menu"
/* "pg2" since the zone page left the enum (2026-11-01) and every later id moved
 * down one: a record under the old key names a different page now, so it is
 * ignored and the first open after the update is at the top. */
#define MENU_POS_KEY_PG  "pg2"
#define MENU_POS_KEY_CUR "cur"

static int  s_pos_pg = PG_TOP;    /* the page the last exit left, RAM mirror */
static int  s_pos_cur;            /* the cursor there */
static bool s_pos_loaded;         /* the record has been read once this boot */

/* Which pages a stored id may be restored into. A symbolic bitmask, not a table
 * designator or a switch arm — the suite pins the page inventory as the only
 * line-start [PG_*] set and the switch model dead, and whether a page re-enters
 * from its id alone is a fact about this file's navigation rather than the
 * table's. A page absent here is left out on purpose: APP/APPACT name an app that
 * may have uninstalled since, CAPS needs a selected slot, SHARE is the window's,
 * and LANDING is unarmed. */
static bool pos_restorable(int pg)
{
    const unsigned ok = (1u << PG_TOP) | (1u << PG_DISPLAY)
                      | (1u << PG_ABOUT) | (1u << PG_UNITS);
    return pg >= 0 && pg < PG_N && ((ok >> pg) & 1u) != 0;
}

/* Read the record once. A first boot, an erased partition, or a stored page the
 * menu no longer ships all leave s_pos_pg at TOP, so a restore that cannot
 * resolve is exactly the open-at-the-top behaviour this replaced. */
static void pos_load(void)
{
    if (s_pos_loaded) return;
    s_pos_loaded = true;
    nvs_handle_t h;
    if (nvs_open(MENU_POS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int32_t pg = PG_TOP, cur = 0;
    if (nvs_get_i32(h, MENU_POS_KEY_PG, &pg) != ESP_OK) pg = PG_TOP;
    (void)nvs_get_i32(h, MENU_POS_KEY_CUR, &cur);
    nvs_close(h);
    if (!pos_restorable((int)pg) || pg < 0 || pg >= PG_N) return;   /* keep TOP */
    s_pos_pg  = (int)pg;
    s_pos_cur = (cur < 0) ? 0 : (int)cur;
}

/* Store the edge the owner left on. Idempotent-by-comparison against the RAM
 * mirror, so the steady state costs zero NVS commits — the same reason the
 * router's page_write refuses a write it already holds. */
static void pos_save(int pg, int cur)
{
    if (cur < 0) cur = 0;
    if (pg == s_pos_pg && cur == s_pos_cur) return;
    s_pos_pg  = pg;
    s_pos_cur = cur;
    nvs_handle_t h;
    if (nvs_open(MENU_POS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i32(h, MENU_POS_KEY_PG,  (int32_t)pg);
    (void)nvs_set_i32(h, MENU_POS_KEY_CUR, (int32_t)cur);
    (void)nvs_commit(h);
    nvs_close(h);
}

void menu_view_set_armed(bool armed)
{
#if CONFIG_ORB_USB_MSC
    /* During a share window the card outranks arming: the router may disarm on
     * the way, and PG_LANDING under a parked ring is the dead frozen menu this
     * card replaced. Keep the page where it is — repaint paints the card from
     * any page state. */
    if (orb_usb_msc_window_open()) { repaint(); return; }
#endif
    /* The save is the disarm edge, mirroring the picker's D35 commit: leaving is
     * the last moment this task knows where the owner stopped, and a detent is a
     * gesture that would otherwise commit a write a second. Only a restorable
     * page is recorded, so leaving from an app page cannot store an id that would
     * later restore onto a vanished app. */
    if (!armed && s_armed && pos_restorable((int)(s_pg - s_pages)))
        pos_save((int)(s_pg - s_pages), s_cur);

    if (!armed) edit_end();
    s_armed = armed;
    if (armed) {
        /* Re-enter on the remembered page, letting the page's own enter() hook
         * place the cursor where it knows the value in force lives. A page with
         * no hook keeps the stored row; repaint's cursor_fit clamps it into a row
         * count that may have shrunk since (an app uninstalled, a card walk not
         * yet landed), so a stale row can never strand the highlight off-glass. */
        pos_load();
        s_pg  = P(s_pos_pg);
        s_cur = 0;
        if (s_pg->enter) s_pg->enter();
        else s_cur = s_pos_cur;
        /* Clamp here, not only in the painter: a row the owner left can point past
         * a list that has since shrunk (an app uninstalled, a card walk not yet
         * landed), and the whole rule of the memory is that the dial opens on a
         * row the owner can actually see. cursor_fit is the same one repaint runs,
         * so the two cannot disagree about where the last legal row is. */
        cursor_fit(page_rows());
    } else {
        s_pg  = P(PG_LANDING);
        s_cur = 0;
    }
    /* Drop last attempt's line, so re-entering shows the menu and not a stale
     * complaint about a card that may since have been swapped. */
    s_ui.status[0] = '\0';
    repaint();
    ESP_LOGI(TAG, "%s", armed ? "menu open" : "menu idle");
}

/* The app page's entry, and its two dynamic answers. One visit, one shape:
 * validated on entry rather than per repaint, because the log line is the
 * point and a page that logged it fifty times a second would bury the cursor
 * moves it is kept beside. The one group an app carries opens with the page
 * (owner, 2026-10-26, correcting the closed entry of the day before): an
 * operator who opened Clock to pick a theme found two rows and a word to
 * press, and a header that answers presses only to hide the list again is
 * the folder tree D12 exists not to be. The cursor still lands on row 0 —
 * the header — so a fresh entry rests on a label and not on a theme, whose
 * cursor move would already load its preview onto the plane. The app is
 * armed so its rows can take the knob — undone by leave_app() on the way
 * out, because an app left armed off-glass would take the next detent from
 * the view actually showing. */
static void open_app(const orb_app_t *a)
{
    s_app        = a;
    s_cur        = 0;
    s_group_open = -1;
    for (int i = 0; i < orb_app_opt_count(a); i++)
        if (orb_app_opt_kind(a, i) == OPT_GROUP) { s_group_open = i; break; }
    shape_check(orb_app_opt_count(a));
    orb_app_opt_arm(a, true);
    /* An installed app's page carries its doors, and they speak APPACT's subject. */
    s_door_slot  = app_owned(a) ? orb_app_index_of(a) : -1;
    s_doormode   = 0;
    s_card_sub   = -1;
    s_pg = P(PG_APP);
}

static int ord_app(void) { return top_os_rows() + app_row_index(s_app); }

static void leave_app(void)
{
    orb_app_opt_arm(s_app, false);
    s_app = NULL;
    s_group_open = -1;
    unarm_arms();                        /* a removal arm never outlives its page */
}

/* The press of an app row. The press belongs to the app, at the app's OWN
 * row number, not the glass position the cursor holds: handing an app a
 * visible index would make a closed group every app's problem to renumber.
 * A group header is NOT intercepted here — the shell stopped owning that
 * press on 2026-10-26, because the list opens with the page and the only
 * other answer the shell had was to hide it again: a trap behind an inert
 * header. The press goes to the app at the header's number, and the app
 * that owns a group (the clock, for now) answers it by doing nothing.
 * opt_sel() may start a job, and it may not paint: repaint() below is what
 * puts the app's new answer on the glass. */
static void act_app(int row)
{
    const int rows = orb_app_opt_count(s_app);
    if (row >= visible_rows(rows)) { act_own(row - visible_rows(rows)); return; }
    const int ar = vis_to_row(rows, row);
    if (ar < 0) { go_back(); repaint(); return; }   /* unreachable: rows_app sized it */
    const char *note = orb_app_opt_note(s_app, ar);
    if (note) { strlcpy(s_note, note, sizeof s_note); repaint(); return; }
    orb_app_opt_sel(s_app, ar);
    repaint();
}

/* A press on the top page: the sub-table's row first — a row that acts (AP)
 * says so, a row that opens a page (DISPLAY, UNITS, APPS, ABOUT) names the
 * page — and below the sub-table, the service run.
 * Reaching past those means the exit row, but rows_top() put exactly one row
 * there and the dispatcher already routed it through MENU_GO_EXIT, so this
 * function never sees it. */
static void act_top(int row)
{
    if (row < top_os_rows()) {
        const menu_row_t *r = &s_top[row];
        if (r->act) r->act();
        if (r->to >= PG_LANDING) go_to(r->to, 0);   /* the page's enter() places the cursor */
        repaint();
        return;
    }
    const orb_app_t *a = app_row(row - top_os_rows());
    if (!a) { s_ui.status[0] = '\0'; repaint(); return; }  /* uninstalled since the paint */
    s_pages[PG_APP].open(a);   /* the table names the entry, not this row */
    repaint();
}

/* Move the page cursor: the page being left sheds its state first (it can
 * still read it — that is the point of running leave() before the switch),
 * the page being entered places its own cursor second, over the one the
 * caller named. The exit answer has no page to enter: it disarms through the
 * public door — which is what stops an app being left armed off-glass after
 * the owner leaves — and hands the knob to the router. */
static void go_to(int to, int cur)
{
    /* The exit answer has no page to enter. Out of the shell goes back to the
     * entry the owner stood on before the menu, not to the landing card (owner
     * ruling, 2026-10-25); input_router_menu_exit disarms on the way, and that
     * disarm is the item-3 edge that stores where in the list they stopped. */
    s_note[0] = '\0';                     /* nor does a message box */
    if (to == MENU_GO_EXIT) { input_router_menu_exit(); return; }
    edit_end();                          /* an edit never outlives its page */
    if (s_pg->leave) s_pg->leave();
    s_pg  = P(to);
    s_cur = cur;
    if (s_pg->enter) s_pg->enter();
}

/* ---- plan §51: the app list ------------------------------------------------------
 * A hold anywhere, or a turn on a face with no dial of its own, lands here: every app
 * the knob can show, in the walk's order, then "settings" for the pages this menu has
 * always held. The shell is not a row of its own list. Every row navigates, so the
 * page has no back row; a hold leaves the way it leaves every page, back to the app it
 * was opened from. */
static const orb_app_t *go_app(int row)
{
    for (size_t i = 0; i < orb_app_count(); i++) {
        const orb_app_t *a = orb_app_at(i);
        if (!a || a->kind == ORB_APP_KIND_SHELL) continue;
        if (row-- == 0) return a;
    }
    return NULL;
}

static int rows_go(void)
{
    int n = 0;
    while (go_app(n)) n++;
    return n + 1;                                    /* +1: settings */
}

static void label_go(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const orb_app_t *a = go_app(row);
    if (a) snprintf(out, outsz, "%s", a->title ? a->title : a->slug);
    else snprintf(out, outsz, "settings");
}

static const char *title_go(void) { return "OPEN"; }

static void act_go(int row)
{
    const orb_app_t *a = go_app(row);
    if (a) { input_router_show(a); return; }
    go_to(PG_TOP, 0);
    repaint();
}

void menu_view_switcher(const orb_app_t *from, int step)
{
    edit_end();
    s_armed = true;
    s_pg = P(PG_GO);
    s_cur = 0;
    for (int r = 0; go_app(r); r++)
        if (go_app(r) == from) { s_cur = r; break; }
    s_cur += step;
    cursor_fit(page_rows());
    s_ui.status[0] = '\0';
    repaint();
    ESP_LOGI(TAG, "app list open on row %d", s_cur);
}

void menu_view_open_settings(void)
{
    edit_end();
    s_armed = true;
    s_pg = P(PG_TOP);
    s_cur = 0;
    if (s_pg->enter) s_pg->enter();
    s_ui.status[0] = '\0';
    repaint();
    ESP_LOGI(TAG, "settings open from the carousel");
}

/* The inventory (declared above; the definitions it names ship in this file,
 * in this order). back_to is where "<- back" goes — the page's parent and the
 * row it lands on, in the one place the page itself is written — and
 * MENU_GO_EXIT on the top page turns its last row into "exit", so no page
 * spells a row's navigation twice. */
const menu_page_t s_pages[PG_N] = {
    [PG_LANDING] = { .title = title_top, .back_to = MENU_GO_NONE },
    [PG_TOP]     = { .rows = rows_top, .label = label_top, .title = title_top,
                     .act = act_top, .back_to = MENU_GO_EXIT },
    [PG_APP]     = { .rows = rows_app, .label = label_app, .title = title_app, .hint = hint_app,
                     .act = act_app, .open = open_app, .ordinal = ord_app,
                     .leave = leave_app, .back_to = PG_TOP, .back_cur = 0, .edit = edit_app },
    [PG_DISPLAY] = { .rows = rows_display, .label = label_display, .title = title_display,
                     .hint = hint_display, .act = act_display,
                     .back_to = PG_TOP, .back_cur = 1, .edit = edit_display },
    /* §58: back is go_back's, onto the app's page at its Permissions row. */
    [PG_CAPS]    = { .rows = rows_caps, .label = label_caps, .title = title_caps,
                     .hint = hint_caps, .act = act_caps, .back_to = PG_APPS, .back_cur = 0 },
    /* APPS is the catalog and the two doors' only path on the glass (B4);
     * ABOUT is inert. Both sit in the OS's own run of the top page, so the
     * dynamic app rows and exit shift down two without any arithmetic here
     * changing — rows_top() reads TOP_OS_N by sizeof. */
    [PG_APPS]    = { .rows = rows_apps, .label = label_apps, .title = title_apps,
                     .hint = hint_apps, .act = act_apps, .enter = enter_apps,
                     .back_to = PG_TOP, .back_cur = 3, .edit = edit_apps },
    [PG_APPACT]  = { .rows = rows_appact, .label = label_appact, .title = title_appact,
                     .hint = hint_appact, .act = act_appact,
                     .ordinal = ord_appact,
                     .back_to = PG_APPS, .back_cur = 0 },
    [PG_UNITS]   = { .rows = rows_units, .label = label_units, .title = title_units,
                     .hint = hint_units, .back_to = PG_TOP, .back_cur = 2, .edit = edit_units },
    [PG_ABOUT]   = { .rows = rows_about, .label = label_about, .title = title_about,
                     .hint = hint_about, .back_to = PG_TOP, .back_cur = 0 },
    /* The share card: reachable from no row, carrying no rows of its own, and
     * answering no press with navigation — the whole page is the door. The
     * table holds it so the paint path stays the table's; the window installs
     * it (menu_view_share_gate), and only the window can take it off. */
    [PG_SHARE]   = { .rows = rows_share, .label = label_share, .title = title_share,
                     .hint = hint_share, .back_to = MENU_GO_NONE, .back_cur = 0 },
    [PG_GO]      = { .rows = rows_go, .label = label_go, .title = title_go,
                     .act = act_go, .back_to = MENU_GO_NONE, .back_cur = 0 },
};

/* ---- docs/knob-edit-plan.md §1.1: the edit mode --------------------------------------
 * A press on a row the page says the knob edits enters it: the row reads the value it
 * will save, each detent moves it by a step its turn's speed picks (knob_edit.c), a
 * press saves through the row's commit, and a hold cancels. Leaving the page or the menu
 * cancels too, so no edit outlives the glass it was made on. */
static struct {
    bool         on;
    int          row;
    const menu_page_t *pg;
    menu_edit_t  e;
    knob_speed_t sp;
} s_edit;

static void edit_end(void) { s_edit.on = false; }

static bool edit_begin(int row)
{
    menu_edit_t e;
    memset(&e, 0, sizeof e);
    if (!s_pg->edit || !s_pg->edit(row, &e)) return false;
    if (e.v < e.r.min) e.v = e.r.min;
    if (e.v > e.r.max) e.v = e.r.max;
    s_edit.on = true;
    s_edit.row = row;
    s_edit.pg = s_pg;
    s_edit.e = e;
    knob_speed_reset(&s_edit.sp);
    repaint();
    return true;
}

static void edit_input(int32_t delta, bool pressed)
{
    if (s_edit.pg != s_pg) { edit_end(); return; }          /* the page moved under it */
    char row[48];
    if (pressed) {
        s_edit.e.commit(s_edit.row, s_edit.e.v);
        s_edit.e.fmt(s_edit.row, s_edit.e.v, row, sizeof row);
        ESP_LOGI(TAG, "edit %s row %d saved: %s", page_title(), s_edit.row, row);
        edit_end();
        repaint();
        return;
    }
    if (!delta) return;
    const int32_t n = delta < 0 ? -delta : delta;
    const int32_t step = knob_speed_step(&s_edit.sp, n, esp_timer_get_time(), s_edit.e.r.step_max);
    /* Up the value is up the list (knob.h): col_step is the sense for "more". */
    s_edit.e.v = knob_edit_move(s_edit.e.v, knob_col_step(delta) * n, step, &s_edit.e.r);
    repaint();
}

/* The app page's rows: the app says which it edits, and the row reads as label_app
 * draws it, with the value not yet saved. */
static void fmt_app(int row, int32_t v, char *out, size_t outsz)
{
    const int ar = vis_to_row(orb_app_opt_count(s_app), row);
    const orb_opt_kind_t k = drawn_kind(orb_app_opt_count(s_app), ar);
    snprintf(out, outsz, "%s%s  (%s)", k == OPT_ITEM ? "  " : "",
             orb_app_opt_label(s_app, ar), orb_app_opt_fmt(s_app, ar, v));
}
static void commit_app(int row, int32_t v)
{
    orb_app_opt_set(s_app, vis_to_row(orb_app_opt_count(s_app), row), v);
}
static bool edit_app(int row, menu_edit_t *e)
{
    if (!s_app) return false;
    const int ar = vis_to_row(orb_app_opt_count(s_app), row);
    orb_opt_edit_t oe;
    if (ar < 0 || !orb_app_opt_edit(s_app, ar, &oe)) return false;
    e->r = (knob_range_t){ oe.min, oe.max, oe.wrap, oe.step_max };
    e->v = oe.value;
    e->fmt = fmt_app;
    e->commit = commit_app;
    return true;
}

/* The display page's timeout, in minutes. */
static void fmt_display(int row, int32_t v, char *out, size_t outsz)
{
    (void)row;
    snprintf(out, outsz, "timeout:  %ld min", (long)v);
}
static void commit_display(int row, int32_t v)
{
    (void)row;
    job_post(job_dim_to, (int)v);
}
static void fmt_units(int row, int32_t v, char *out, size_t outsz)
{
    const char *l = orb_units_label(row, (int)v);
    snprintf(out, outsz, "%s:  %s", UNIT_ROW[row], l ? l : "?");
}
static void commit_units(int row, int32_t v)
{
    job_post(job_units, row * 8 + (int)v);
}
static bool edit_units(int row, menu_edit_t *e)
{
    if (row < 0 || row >= ORB_UNITS_KINDS) return false;
    e->r = (knob_range_t){ 0, orb_units_count(row) - 1, true, 1 };
    e->v = unit_now(row);
    e->fmt = fmt_units;
    e->commit = commit_units;
    return true;
}

static bool edit_display(int row, menu_edit_t *e)
{
    if (row != 1) return false;
    const uint32_t s = idle_dim_timeout_s();
    e->r = (knob_range_t){ 1, TO_MIN_MAX, false, 5 };
    e->v = (int32_t)((s + 30u) / 60u);
    e->fmt = fmt_display;
    e->commit = commit_display;
    return true;
}

/* The press, table-driven. The two answers every page shares are decided
 * here: the last row is the page's exit — "<- back" where there is a parent,
 * "exit" where the table says MENU_GO_EXIT — and every press routes through
 * the busy and armed gates in menu_view_apply_input before this runs. What
 * remains is the page's own act(), and this function finally has no LVGL and
 * no card in it: the host suite tests it directly, press paths included. */
static void activate(void)
{
    const int rows = s_pg->rows ? s_pg->rows() : 0;
    if (rows <= 0 || s_cur >= rows) return;
    if (s_cur == rows - 1 && s_pg->back_to != MENU_GO_NONE) { go_back(); repaint(); return; }
    if (edit_begin(s_cur)) return;      /* a knob-edited row: the press enters the edit */
    if (s_pg->act) s_pg->act(s_cur);
}

/* The four accessors the view calls, each one line of table now. They keep
 * their old names because the view and the sentinel speak them, and
 * page_rows() keeps the armed check as its own: unarmed means no list at all,
 * decided in one place so the painter and the cursor clamp cannot drift into
 * disagreeing about whether rows exist. */
static int page_rows(void) { return s_armed && s_pg->rows ? s_pg->rows() : 0; }

static void row_text(int row, char *out, size_t outsz, bool *sel_hi)
{
    out[0] = '\0';
    if (sel_hi) *sel_hi = false;
    if (s_edit.on && s_edit.pg == s_pg && row == s_edit.row) {   /* the value being edited */
        s_edit.e.fmt(row, s_edit.e.v, out, outsz);
        if (sel_hi) *sel_hi = true;
        return;
    }
    if (s_pg->label) s_pg->label(row, out, outsz, sel_hi);
}

static const char *page_title(void) { return s_pg->title ? s_pg->title() : "SETTINGS"; }

/* The header as painted (§7 item 6). A page longer than the glass scrolls, and a
 * scrolled list with no position readout cannot answer "how far am I from the
 * end" — the zone picker is 63 rows and the highlight alone does not say whether
 * you are at 4 or 60. The gate is the window's own (rows > ROW_MAX), not a flag
 * a page can forget to set: a page that draws every row needs no readout and
 * gets none, and a page that scrolls cannot be reached without one. The
 * denominator is page_rows() rather than the plan's example 62 because this page
 * has 63 rows and the cursor fits on the back row; a header that could read
 * 63/62 would be a worse lie than one the plan miscounted. The cursor log line
 * keeps page_title(): its row N/M is the same number in the walk driver's own
 * 0-based shape, so the line stays the glass's witness without doubling the
 * digits into the log. */
/* The rows the list may use: all five, or four while a status line is up. The
 * line sits where a fifth row lands, so on a page that fills the glass the two
 * were drawn over each other ("HSB: installed" beside "uninstall", Gold,
 * 2026-11-07); every page had that, an app's one page just reaches five rows
 * every time. ABOUT's QR takes the list's place and the line's, so it keeps five. */
static int rows_on_glass(void)
{
    return (s_ui.status[0] && !about_qr_on()) ? ROW_MAX - 1 : ROW_MAX;
}

static void title_line(char *out, size_t outsz)
{
    const int rows = page_rows();
    if (rows > rows_on_glass())
        snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);
    else
        snprintf(out, outsz, "%s", page_title());
}

static const char *page_hint(void)
{
    s_painted_busy = orb_jobs_busy();
    if (s_note[0]) return "push: OK";
    if (s_painted_busy) return "working...";
    if (s_edit.on) return "rotate: adjust   push: save   hold: cancel";
    return s_pg->hint ? s_pg->hint() : "rotate: choose   push: open";
}


void menu_view_apply_input(int32_t delta, bool pressed)
{
    if (!s_armed) return;                    /* the router owns the knob here */

    /* A job is running: the row it is working on is already chosen, so moving
     * the cursor now would leave the highlight somewhere other than the thing
     * being applied and the status line would arrive about the old row. The
     * worker cannot be cancelled, so the honest UI is to freeze and show
     * "working..." (page_hint) until the result lands. */
    if (orb_jobs_busy()) return;

    if (s_note[0]) {                         /* the box takes the gesture that closes it */
        if (pressed || delta) { s_note[0] = '\0'; repaint(); }
        return;
    }
    if (s_edit.on) { edit_input(delta, pressed); return; }
    if (pressed) { activate(); return; }
    if (delta == 0) return;

    const int rows = page_rows();
    if (rows <= 0) return;

    /* The sign comes from the knob layer, not from this file: one constant decides
     * the sense for every screen, and this list follows it. See knob.h. */
    int next = s_cur + knob_row_step(delta);
    if (next == s_cur) return;
    if (next < 0) next = 0;
    /* Clamp rather than wrap: a 62-row list that rings lets one fast turn land
     * anywhere but where the hand was heading, and on a dial with no way to
     * see the whole list at once that reads as the knob being ignored. */
    if (next >= rows) next = rows - 1;

    /* Inside an open group the clamp is the group's, not the page's (D12): while
     * items are on the glass the walk cannot fly above their header, because a
     * detent that carried the cursor past the row that shuts the group would
     * strand the owner who opened it somewhere down the page looking for it.
     * The floor of the walk is the header itself and the ceiling is the page's
     * last row (owner, 2026-10-26): the first item and the back row are both on
     * the glass, and a cursor that could not rotate onto either made an open
     * group a locked room. Pressing the header does nothing; pressing the back
     * row leaves. */
    if (s_pg == P(PG_APP) && s_app && s_group_open >= 0) {
        const int arows = orb_app_opt_count(s_app);
        const int cur = vis_to_row(arows, s_cur);
        if (cur >= 0 && drawn_kind(arows, cur) == OPT_ITEM) {
            const int lo = row_to_vis(arows, s_group_open);
            const int hi = rows - 1;               /* the back row, always last */
            if (lo >= 0) {
                if (next < lo) next = lo;
                if (next > hi) next = hi;
            }
        }
    }
    if (next == s_cur) return;
    /* Any move of the cursor ends the uninstall arm (P5.3): the confirmation is
     * a second press on THE SAME ROW, and an arm that survived a detent would
     * let the press that follows a rotation erase an app the owner had already
     * walked away from. The helper takes the confirmation's own line with it —
     * the arm and the prompt are one fact, and the bench caught a detent that
     * cleared the first and left the second on the glass. */
    unarm_arms();
    s_cur = next;
    /* Where the cursor landed, in the log. The menu never said: it repaints the
     * highlight and that was the only answer, so driving it blind over
     * /api/v1/screen?turn=N could not tell a clamped turn from a registered one.
     * Three scripted walks pressed the AP stub believing they had chosen DISPLAY,
     * and fired the zone write believing they were committing the dim timeout -- the
     * symptoms all read as a swallowed knob, which is the exact report this
     * feature already carries. The row text is this screen's own truth, so the
     * line cannot drift from what the glass shows. */
    char row[40];
    bool hi;
    row_text(s_cur, row, sizeof row, &hi);
    ESP_LOGI(TAG, "cursor %s row %d/%d: %s", page_title(), s_cur, rows - 1, row);
    repaint();
}

void menu_view_status(const char *msg)
{
    strlcpy(s_ui.status, msg ? msg : "", sizeof s_ui.status);
    repaint();
}

bool menu_view_opt_long(void)
{
    if (!s_armed) return false;                    /* the router owns the knob */

    /* A hold with the message box up closes the box, and only the box. */
    if (s_note[0]) { s_note[0] = '\0'; repaint(); return true; }

    /* A hold during an edit cancels it, and only it: the value stays as saved. */
    if (s_edit.on) { edit_end(); repaint(); return true; }

    /* A page's own long hook has first refusal (D12). Below it the app page
     * answers exactly like every other page: the hold exits the menu. It used
     * to collapse the open group first; the header is inert now and collapsing
     * under it would hide the list behind a row that cannot bring it back —
     * only an exit and a re-entry could, so the collapse was a trap with an
     * extra step (owner, 2026-10-26). The clock's own hook still parks on
     * Home and takes the hold before this rule. */
    if (s_pg == P(PG_APP) && s_app) {
        if (orb_app_opt_long(s_app)) return true;
    }

    /* Nothing was open to collapse. Owner ruling, 2026-10-25: the hold is the
     * way out of the menu, and out of the menu means back to the entry the
     * glass showed before the shell took it — the same door the exit row
     * takes, so a hold from the top list and a hold from the depths of the
     * tree both land where the owner stood on their way in. The landing card
     * is not the answer to leaving; it stays what always was, the face of an
     * unarmed shell (a bench request, or a share window parked over the dial).
     *
     * The share page says no on its own row: while the window is open the
     * gate above this function swallows the hold and answers with the release,
     * and this hook must not grow a second, quieter door out of the window. */
    if (s_pg == P(PG_SHARE)) return false;
    input_router_menu_exit();
    return true;
}

void menu_view_service(void)
{
#if CONFIG_ORB_USB_MSC
    /* A loan that fell through (no card, a refused alloc) returns the mode to
     * OFF without a reboot while the card may still be the painted page: the
     * glass is live again, so put the page back that was there before the
     * press. The reboot exit never reaches this line — the glass is parked
     * and the boot that follows rebuilds everything. */
    if (s_pg == P(PG_SHARE) && !orb_usb_msc_window_open()) {
        s_share_painted = -1;
        s_pg = s_app ? P(PG_APP) : P(PG_TOP);
        s_cur  = 0;
        ESP_LOGI(TAG, "share refused — the menu is live again");
        repaint();
    }
#endif
    /* Three independent reasons to repaint, any of which can fire without the
     * others: this screen's own worker landing a result (s_dirty), and the app
     * PG_APP is showing gaining or losing rows out from under a paint. The
     * second is the one that makes a theme page reachable at all — entering it
     * posts the clock's card walk, that walk reaps in orb_app_service_all()
     * BEFORE menu_service is ever reached, so by the time this runs the app
     * already has four rows the glass has never drawn. Without the count
     * re-check the page opens on an empty list and only fills in once the owner
     * turns the knob, which reads as the card being ignored. The third is the
     * same reap for the plane: the preview read lands on the app's worker and
     * changes no row count, so without the stamp re-check a plane that arrives
     * while the cursor rests paints nothing until the next detent — the
     * "previews never show" bug written into the layout. */
    const bool app_grew = (s_pg == P(PG_APP) && s_app &&
                           page_rows() != s_painted_rows);
    uint16_t pw = 0, ph = 0;
    uint32_t pst = 0;
    const void *px = app_preview(&pw, &ph, &pst);
    const bool pv_landed = (px != NULL) != (s_pv_stamp != 0) ||
                           (px && (pst != s_pv_stamp || pw != s_pv_w || ph != s_pv_h));
    const bool own_result = s_dirty;
    const bool busy_flip = s_armed && orb_jobs_busy() != s_painted_busy;
    if (!own_result && !app_grew && !pv_landed && !busy_flip) return;
    s_dirty = false;

    /* §58: a Themes sub-menu's group arrives with the card walk, which can land
     * after the page opened with no group to open. The group opens as it would have at
     * entry, so the themes are on the glass rather than behind an inert header. */
    if (app_grew && s_group_open < 0) {
        for (int i = 0; i < orb_app_opt_count(s_app); i++)
            if (orb_app_opt_kind(s_app, i) == OPT_GROUP) { s_group_open = i; break; }
    }

    if (own_result) {
        xSemaphoreTake(s_res_mx, portMAX_DELAY);
        s_ui = s_shared;
        xSemaphoreGive(s_res_mx);
        /* A card file's door page whose file just installed has no subject left —
         * the file is on the dial now, not "on the card" — and drew "? APP" over a
         * lone back row, which read as a hung dial (Gold, 2026-11-07). Follow the
         * app to its own registry doors instead. */
        if (s_pg == P(PG_APPACT) && s_doormode && !card_subject() && s_pend_slug[0]) {
            const orb_app_t *a = orb_app_find_slug(s_pend_slug);
            if (a && a->installed) s_pages[PG_APP].open(a);   /* its own page, cursor on top */
        }
        /* And the other way: an uninstalled app's row is gone (s_app now names a
         * zeroed entry), so its page follows the app to its card file, whose first
         * door is install again — or back to the list when the card holds no copy. */
        if (s_pg == P(PG_APP) && s_app && !orb_app_is_menu_only(s_app) &&
            !s_app->installed && s_pend_slug[0]) {
            int k = 0;
            while (k < s_card_n && strcmp(s_card_ent[k].slug, s_pend_slug) != 0) k++;
            s_app = NULL;                    /* nothing left to disarm */
            s_group_open = -1;
            if (k < s_card_n) {
                s_pg        = P(PG_APPACT);
                s_doormode  = 1;
                s_card_sub  = k;
                s_door_slot = -1;
                s_cur       = 0;
            } else {
                go_to(PG_APPS, 0);
            }
        }
    }

    /* The theme page's own results are the clock's to reap, through its
     * service() hook: a card walk that finishes while the owner has rotated to
     * the dial must still land, and the app owns what it means. What is left
     * here is this screen's own status line. */
    repaint();
}


