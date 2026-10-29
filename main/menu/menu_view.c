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
 * What still blocks on this screen is NVS: the zone write (net_tz_select) and the
 * two DISPLAY setters, each of which can hit a sector erase. A theme walk used to
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

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "orb_jobs.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_app_desc.h"   /* ABOUT: the running image's own stamp */

#include "orb_net.h"
#include "orb_sd.h"
#include "orb_app.h"
#include "orb_app_install.h"   /* APPS: the enable/uninstall doors (P5.3) */
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
 * rows_top() adds the dynamic ones — one per app that declares options, then
 * exit. APPS and ABOUT landed as s_top entries in P5.3 without this shape
 * changing at all, which is the point of the table. */

typedef enum {
    PG_LANDING = 0,   /* inert title; the knob belongs to the router   */
    PG_TOP,           /* s_top, then one row per app with options, exit */
    PG_TZ,            /* the zone table                               */
    PG_APP,           /* one app's own options (its opt_* answers, B1) */
    PG_DISPLAY,       /* the two idle-dim settings                    */
    PG_TO,            /* the timeout presets                          */
    PG_PERMS,         /* one row per installed app: what it may do    */
    PG_CAPS,          /* the selected app's four owner caps, togglable */
    PG_APPS,          /* the catalog: every installed app, its state, its doors (B3/B4) */
    PG_APPACT,        /* one app's own doors: activate / enable / uninstall */
    PG_ABOUT,         /* inert identity list — no press does anything  */
    PG_SHARE,         /* D38: the USB share window's own card, forced over any page */
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
     *  app page returns to the top page's dynamic run at this app's own row,
     *  and the caps page to its app's row of the permission list. Evaluated
     *  before leave(), so the page can still read its own state. */
    int  (*ordinal)(void);
    void (*enter)(void);                /* cursor placement on entry          */
    void (*leave)(void);                /* side effects when the page is left */
    int  back_to;                       /* menu_go_t: NONE = the page has no back row */
    int  back_cur;                      /* cursor there, literal              */
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
static int  rows_tz(void);       static void label_tz(int, char *, size_t, bool *);
static const char *title_tz(void);   static const char *hint_tz(void);
static void act_tz(int);         static void enter_tz(void);
static int  rows_app(void);      static void label_app(int, char *, size_t, bool *);
static const char *title_app(void);  static const char *hint_app(void);
static void act_app(int);        static void open_app(const orb_app_t *);
static int  ord_app(void);       static void leave_app(void);
static int  rows_display(void);  static void label_display(int, char *, size_t, bool *);
static const char *title_display(void);
static void act_display(int);
static int  rows_to(void);       static void label_to(int, char *, size_t, bool *);
static const char *title_to(void);
static void act_to(int);         static void enter_to(void);
static int  rows_perms(void);    static void label_perms(int, char *, size_t, bool *);
static const char *title_perms(void);
static void act_perms(int);      static int  ord_caps(void);
static int  rows_caps(void);     static void label_caps(int, char *, size_t, bool *);
static const char *title_caps(void);   static const char *hint_caps(void);
static void act_caps(int);
static int  rows_apps(void);     static void label_apps(int, char *, size_t, bool *);
static const char *title_apps(void);   static const char *hint_apps(void);
static void act_apps(int);       static void enter_apps(void);
static int  rows_appact(void);   static void label_appact(int, char *, size_t, bool *);
static const char *title_appact(void); static const char *hint_appact(void);
static void act_appact(int);     static int  ord_appact(void);
static void open_appact(const orb_app_t *);
static int  rows_about(void);    static void label_about(int, char *, size_t, bool *);
static const char *title_about(void);
static int  rows_share(void);    static void label_share(int, char *, size_t, bool *);
static const char *title_share(void);  static const char *hint_share(void);
static int  top_os_rows(void);   static int app_row_index(const orb_app_t *);
static void act_ap(void);        static void go_to(int to, int cur);
static int  cap_pack(const orb_app_t *, int row);
static int  door_pack(const orb_app_t *, int act);
static void job_grant(void *arg);
static void job_enable(void *arg);   static void job_uninstall(void *arg);
static void job_scan(void *arg);     static void job_install_card(void *arg);
static void job_cardrm(void *arg);
static int  page_rows(void);
static void row_text(int, char *, size_t, bool *);
static const char *page_title(void);
static void title_line(char *, size_t);
static const char *page_hint(void);
static void go_back(void);       static void activate(void);
static void repaint(void);       static void job_post(orb_job_fn fn, int idx);


/** How many app rows the top page carries: the service tier first (D36 — those
 *  are rows with settings and no face, and they lead because they are the OS's
 *  own answers), then one per entry with a face that declares options, in table
 *  order, which is the carousel's order — so the menu's list and the knob's walk
 *  cannot disagree about what exists. */
static int app_rows(void)
{
    int n = (int)orb_app_menu_count();
    for (size_t i = 0; i < orb_app_count(); i++) {
        const orb_app_t *a = orb_app_at(i);
        if (a && orb_app_has_options(a)) n++;
    }
    return n;
}

/** The i-th app that declares options, or NULL. Resolved through the registry
 *  every time rather than cached: P4 enables and disables apps at runtime, and
 *  a cached entry is a screen the owner can reach and find nothing on. */
static const orb_app_t *app_row(int i)
{
    if (i < 0) return NULL;
    /* D36: the service tier leads the run, then the faces, in the same order
     * app_rows() counted. Asking the registry for both keeps this the only place
     * the menu's dynamic run is defined. */
    const int nsvc = (int)orb_app_menu_count();
    if (i < nsvc) return orb_app_menu_at((size_t)i);
    i -= nsvc;
    for (size_t k = 0; k < orb_app_count(); k++) {
        const orb_app_t *a = orb_app_at(k);
        if (!a || !orb_app_has_options(a)) continue;
        if (i-- == 0) return a;
    }
    return NULL;
}

/* The timeout choices on the DISPLAY sub-menu, seconds. Presets rather than a
 * spinner: the setting is a wall-clock decision ("how long before a clock on
 * the wall goes dark"), not a tuning knob, and six detent-turns to a number
 * invite the kind of value — 7 m 23 s — that nobody can justify. 1 min exists
 * for the bench (the Kconfig help's measurement procedure wants a short timer),
 * 10 min is the owner's number, and 1 h covers a reading chair. */
typedef struct { uint32_t s; const char *lbl; } to_preset_t;
static const to_preset_t s_to_preset[] = {
    {    60, "1 min"  },
    {   120, "2 min"  },
    {   300, "5 min"  },
    {   600, "10 min" },
    {   900, "15 min" },
    {  1800, "30 min" },
    {  3600, "1 hour" },
};
#define TO_PRESET_N ((int)(sizeof(s_to_preset) / sizeof(s_to_preset[0])))

/* Row label for the timeout actually in force. A device flashed with a
 * different Kconfig default can hold a value no preset covers, and the row
 * must then read the truth ("45 s") rather than the nearest preset, which
 * would be claiming a setting the dial does not have. */
static void timeout_label(char *out, size_t outsz)
{
    const uint32_t cur = idle_dim_timeout_s();
    for (int i = 0; i < TO_PRESET_N; i++)
        if (s_to_preset[i].s == cur) { snprintf(out, outsz, "%s", s_to_preset[i].lbl); return; }
    snprintf(out, outsz, "%lu s", (unsigned long)cur);
}

/* Which app PG_APP is showing. Set on entry from the registry and cleared on
 * leave, so a repaint can never render the previous app's rows under the new
 * one's header — the two-second card walk that lands after the owner has
 * already rotated away is exactly when that would show. */
static const orb_app_t *s_app;
static bool   s_armed;
static int    s_cur;              /* cursor within the current page */
static int    s_painted_rows;     /* page_rows() the last repaint drew, for the PG_APP reap */

/* ---- D19: the owner's revocation ladder (plan §4.2), two pages below
 * PERMISSIONS ---------------------------------------------------------------
 *
 * Declare-at-install is the grant, so these pages approve nothing — they let
 * the owner take back what an app asked for at install, and show the
 * difference between what it declared and what still runs. The rows, the
 * state, and the one rule the pages exist for: the broker is the only authority
 * on what a toggle may do, so a row never decides locally whether its cap is
 * grantable, it posts the job and renders the answer. The four caps listed are
 * the owner-granted four: the free ones would put rows on this page that can
 * only ever say yes, and a page of forced yeses teaches the owner to stop
 * reading it.
 */
static int      s_perm_slot = -1;              /* the app under inspection  */
static const orb_cap_t OWNER_CAPS[4] = { CAP_POWER, CAP_NET,
                                         CAP_SD_READ, CAP_THEMES };

/* The installed, non-chrome entries — which is NOT the same list as the top
 * page's app rows: those are the apps that declare options, while
 * PERMISSIONS must show every app that can ASK, option hooks or none. */
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

/* The three jobs this screen still needs. Each takes the copied payload the
 * shell's worker hands it and writes only its own status line: nothing here
 * touches a widget, which is the half of the rule that keeps the dial moving
 * while NVS erases a sector. */
static void job_tz(void *arg)
{
    const int idx = *(const int *)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const esp_err_t er = orb_net_tz_select(idx);
    if (er != ESP_OK)
        snprintf(out.status, sizeof(out.status), "\"%s\" not set (%s)",
                 orb_net_tz_label(idx), esp_err_to_name(er));
    else
        snprintf(out.status, sizeof(out.status), "zone set: %s", orb_net_tz_label(idx));
    worker_publish(&out);
}

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

static void job_dim_to(void *arg)
{
    const int idx = *(const int *)arg;
    ui_t out; memset(&out, 0, sizeof out);
    const to_preset_t *p = &s_to_preset[idx];
    const esp_err_t er = idle_dim_set_timeout_s(p->s);
    if (er == ESP_ERR_NOT_SUPPORTED)
        snprintf(out.status, sizeof(out.status), "the dim is not in this build");
    else
        snprintf(out.status, sizeof(out.status), "timeout: %s%s", p->lbl,
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
 * open. The AP row stays a stub on
 * purpose: joining needs a passphrase, and a single push-input cannot type
 * one; the browser page is the way in until a keypad screen exists. */
static const menu_row_t s_top[] = {
    { "AP",          MENU_GO_NONE,   act_ap },
    { "TZ",          PG_TZ,          NULL },
    { "DISPLAY",     PG_DISPLAY,     NULL },
    /* The marker this row can carry is the refusal signal, not an ask: any
     * app the gate closed this boot leaves one '*' on it, and the owner reads
     * it on the top page rather than being interrupted on the dial (D19).
     * Nothing waits on an answer. */
    { "PERMISSIONS", PG_PERMS,       NULL },
    /* APPS and ABOUT join the OS's own run rather than the dynamic one: they
     * describe the machine, not one app, and the plan's §7.2 top level names
     * both. Appending them after PERMISSIONS rather than slotting them before
     * it keeps every back_cur literal in the table below valid — the row each
     * sub-page returns to is the row it was written for. */
    { "APPS",        PG_APPS,        NULL },
    { "ABOUT",       PG_ABOUT,       NULL },
};

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
static int rows_tz(void)      { return orb_net_tz_count() + 1; }      /* last row is back */
static int rows_display(void) { return 3; }                     /* dim, timeout, back */
static int rows_to(void)      { return TO_PRESET_N + 1; }       /* last row is back */
static int rows_perms(void)   { return perm_rows() + 1; }
static int rows_caps(void)    { return 4 + 1; }

/* The app counts every row it has; the shell decides how many of them are on
 * the glass, which is the whole of ruling D12 and the reason the count below
 * is not the app's number handed straight back. The +1 is the back row, and it
 * is outside the group: leaving a page must never depend on whether a header
 * happens to be open. */
static int rows_app(void)
{
    if (!s_app) return 0;
    return visible_rows(orb_app_opt_count(s_app)) + 1;
}

static void label_top(int row, char *out, size_t outsz, bool *sel_hi)
{
    out[0] = '\0';
    if (sel_hi) *sel_hi = false;
    /* Rows after the OS's own are one per app that declares options, labelled
     * with the app's TITLE; the row after those is exit. Two ways to get this
     * wrong and both read as the knob being ignored: reading the dynamic run
     * without bounds and letting the exit case answer "exit" on a row that is
     * really an app, and testing only `>= TOP_OS_N` and sending the exit row
     * looking for an app that is not there. */
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
     * this boot leaves one '*' on the PERMISSIONS row, and the owner reads it
     * here rather than being interrupted on the dial (D19). Nothing waits on
     * an answer. The row is the one that opens the permission list — keyed to
     * its target, not to an index, so inserting a row above it moves the
     * marker along with the row. */
    if (r->to == PG_PERMS && perms_denied())
        snprintf(out, outsz, "%s  *", r->label);
    else
        snprintf(out, outsz, "%s", r->label);
}

/* Which page a "<- back" row returns to, and on which row: both come from the
 * table row, which is where the count, the parent and the landing cursor live
 * together. The two dynamic answers are the ordinal functions — the app page
 * lands on the top page's row that opened it (wherever that sits now), and the
 * caps page lands on its app's row of the permission list. An ordinal runs
 * before the leaving page's leave(), so it can still read the page's state.
 * go_back stays a name because the picker arms and the sentinel both speak it. */
static void go_back(void)
{
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

static void label_tz(int row, char *out, size_t outsz, bool *sel_hi)
{
    const int n = orb_net_tz_count();
    if (row >= n) { snprintf(out, outsz, "<- back"); return; }
    /* The row the dial is actually on is marked, so re-entering the picker
     * answers "where am I?" without the owner having to remember what they
     * set. */
    const bool cur = (row == orb_net_tz_current_index());
    if (sel_hi) *sel_hi = cur;
    snprintf(out, outsz, "%s%s", orb_net_tz_label(row), cur ? "  *" : "");
}

static void label_app(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (!s_app) return;
    const int rows = orb_app_opt_count(s_app);
    if (row >= visible_rows(rows)) { snprintf(out, outsz, "<- back"); return; }
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
static void label_perms(int row, char *out, size_t outsz, bool *sel_hi)
{
    const int n = perm_rows();
    if (row >= n) { snprintf(out, outsz, "<- back"); return; }
    const orb_app_t *a = perm_row(row);
    if (!a) { snprintf(out, outsz, "?"); return; }   /* rows_perms sized it */
    /* Both counts, because under D19 they are different facts: declared
     * is what the app asked for at install, allowed is what still runs
     * after the owner's toggles. "2 of 2" and "0 of 2" read the same
     * under a single count, and they are the two answers an owner
     * actually asks — what did I agree to, and what is live now. */
    int nd = 0, ng = 0;
    const uint32_t dm = orb_svc_declared(a), gm = orb_svc_grants(a);
    for (unsigned c = 0; c < 4; c++) {
        if (dm & (1u << OWNER_CAPS[c])) nd++;
        if (gm & (1u << OWNER_CAPS[c])) ng++;
    }
    /* The dial is 466 px wide and this row carries the app's own title,
     * so the marker stays one character. A denied call is rare enough —
     * it means an app reached past its declaration — that a lone '*'
     * earns a look, and the cap page names which cap when they come. */
    if (nd == 0)
        snprintf(out, outsz, "%s%s  (nothing declared)",
                 orb_svc_denied_mask(a) ? "* " : "",
                 a->title ? a->title : a->slug);
    else
        snprintf(out, outsz, "%s%s  (%d of %d)",
                 orb_svc_denied_mask(a) ? "* " : "",
                 a->title ? a->title : a->slug, ng, nd);
}

static void label_caps(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (row >= 4) { snprintf(out, outsz, "<- back"); return; }
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    if (!a) { snprintf(out, outsz, "?"); return; }
    const uint32_t bit = 1u << OWNER_CAPS[row];
    const bool g   = (orb_svc_grants(a)   & bit) != 0;
    const bool dcl = (orb_svc_declared(a) & bit) != 0;
    if (sel_hi) *sel_hi = g;          /* the highlight answers "what is
                                       * the state" the same way the
                                       * zone row's star does */
    /* Three states, and the third is the one the old page could not
     * show: an app that never asked for the network must not read as
     * something the owner could turn on. The row is inert, and the
     * broker refuses the toggle anyway — this is the honest label, not
     * a second gate. The mark rides all three, because the case worth
     * seeing is an app calling a cap it never declared: that row IS
     * undeclared, and dropping the mark here would hide the one
     * refusal that says something about the app rather than the
     * owner. */
    const bool ref = (orb_svc_denied_mask(a) & bit) != 0;
    if (!dcl) {
        snprintf(out, outsz, "%s: not declared%s",
                 orb_svc_cap_name(OWNER_CAPS[row]), ref ? " *" : "");
        return;
    }
    snprintf(out, outsz, "%s: %s%s", orb_svc_cap_name(OWNER_CAPS[row]),
             g ? "allowed" : "revoked", ref ? " *" : "");
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

static void label_to(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (row >= TO_PRESET_N) { snprintf(out, outsz, "<- back"); return; }
    /* Same current-value mark as the zone list: the picker must answer
     * "which one am I on?" before anybody starts scrolling. */
    const bool cur = (s_to_preset[row].s == idle_dim_timeout_s());
    if (sel_hi) *sel_hi = cur;
    snprintf(out, outsz, "%s%s", s_to_preset[row].lbl, cur ? "  *" : "");
}

/* The headers. The app page answers with the app's own title, and the caps
 * page with that title qualified — a page about one app must never be read as
 * the list it came from. Static buffer, one writer, the LVGL task: same
 * reasoning as every painted string in this file. */
static const char *title_top(void)  { return "MENU"; }
static const char *title_tz(void)   { return "TIME ZONE"; }
static const char *title_app(void)  { return (s_app && s_app->title) ? s_app->title : "OPTIONS"; }
static const char *title_display(void) { return "DISPLAY"; }
static const char *title_to(void)   { return "DIM TIMEOUT"; }
static const char *title_perms(void){ return "PERMISSIONS"; }
static const char *title_caps(void)
{
    static char caps_title[48];
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    snprintf(caps_title, sizeof caps_title, "%s PERMISSIONS",
             a && a->title ? a->title : a ? a->slug : "?");
    return caps_title;
}

/* The hint says what the two gestures do HERE, because a sub-menu that does
 * not name the gesture that leaves it is a page the owner is stuck on until
 * they hold the knob by accident (C8's lesson, from the other side). NULL
 * means the plain "push: open" — the top page, the app list, the landing
 * card nobody presses. The app page's own hint lost its second string on
 * 2026-10-26: back is a row and the group header is inert, so the plain
 * sentence is true again. */
static const char *hint_tz(void) { return "rotate: choose   push: set"; }
static const char *hint_to(void) { return "rotate: choose   push: set"; }
static const char *hint_caps(void) { return "rotate: choose   push: toggle"; }
static const char *hint_app(void)
{
    /* The plain answer. Back is a row, so the exit needs no clause; the group
     * header is inert, so the hint must not promise it answers a press. */
    return "rotate: choose   push: change";
}
static const char *hint_display(void) { return "rotate: choose   push: change"; }

/* The presses that answer without a page of their own. AP is stubbed on
 * purpose (joining needs a passphrase a push-input cannot type; the browser
 * page stays the way in until a keypad screen exists). The others post the one
 * job the row names and repaint — showing "working..." while the single worker
 * gets to it. Nothing that can block runs on the task that paints the dial:
 * net_tz_set() writes NVS then tzset()s, the dim setter nvs_commits, and the
 * grant writes the broker's word. */
static void act_ap(void)
{
    snprintf(s_ui.status, sizeof(s_ui.status),
             "AP needs a passphrase: use the web page");
    ESP_LOGI(TAG, "AP selected (stub)");
    repaint();
}
static void act_tz(int row) { job_post(job_tz, row); repaint(); }
static void act_display(int row)
{
    if (row == 0) { job_post(job_dim_en, 0); repaint(); return; }
    /* Timeout row: open the picker. enter_to() starts it on the row that
     * matches the value in force -- the same "where am I?" courtesy PG_TZ
     * gives -- so this arm says nothing about the cursor. */
    go_to(PG_TO, 0);
    repaint();
}
static void act_to(int row)
{
    job_post(job_dim_to, row);
    /* Choosing IS leaving: back to DISPLAY, parked on the timeout row, rather
     * than leaving the picker up showing "working...". The row repaints from
     * the getter when the worker's write lands, so the value the user just
     * picked is already right when they arrive. */
    go_back();
    repaint();
}
static void act_perms(int row)
{
    /* The D19 door: what each installed app declared at install, what still
     * runs after the owner's toggles, and a '*' where an app has been refused
     * for calling past its declaration. app -> cap -> toggle from here, with
     * no ceremony in between — the install already carried the consent. */
    const orb_app_t *a = perm_row(row);
    if (!a) { go_back(); repaint(); return; }           /* rows_perms sized it */
    s_perm_slot = orb_app_index_of(a);
    go_to(PG_CAPS, 0);
    repaint();
}
static void act_caps(int row)
{
    const orb_app_t *a = orb_app_row((size_t)s_perm_slot);
    if (!a) { go_to(PG_PERMS, 0); repaint(); return; }
    /* One press, one toggle — the ceremony is gone because there is nothing
     * left to confirm (D19): the owner already agreed to this cap when the app
     * declared it at install, and this row only takes it back or gives it
     * again. An undeclared cap is NOT refused here: the broker is the single
     * authority on what a toggle may do, so the press posts the job and the
     * status line reports the refusal, which keeps one rule in one file
     * instead of two that can disagree. */
    job_post(job_grant, cap_pack(a, row));
    repaint();                                          /* shows "working..." */
}
static void enter_tz(void)
{
    /* Start on the zone in effect rather than at row 0: the common reason to
     * open this is to nudge a neighbouring city, and UTC-first would make
     * every visit a scroll. */
    const int c = orb_net_tz_current_index();
    s_cur = (c < 0) ? 0 : c;
}
static void enter_to(void)
{
    const uint32_t cur = idle_dim_timeout_s();
    for (int i = 0; i < TO_PRESET_N; i++)
        if (s_to_preset[i].s == cur) { s_cur = i; return; }
}

/* ---- P5.3: APPS, its doors, and ABOUT (plan §7 items 2 and 4) --------------
 *
 * The catalog is the registry's installed tier, the same set PERMISSIONS walks
 * (perm_rows/perm_row) and for the same reason: `orb_app_at()` hides the
 * shell's own entries and every installed-but-disabled app, and a page whose
 * job is the owner's doors cannot afford to hide the disabled ones — "turned
 * off" is exactly the row they come to turn back on.
 *
 * The four states, restated 2026-10-24: the row says one of them rather than a
 * boolean: `on the dial` (the entry the glass is showing), `enabled`
 * (on the carousel, not showing), `off` (installed and walkable but switched
 * off), and `failed: <reason>` (C2's fault record, shown with its
 * reason verbatim — the orb_theme_scan rule that a row which cannot be used is
 * still a row that must be explained). The reason wins over the enable state
 * because a failed app that reads merely "off" tells the owner to go looking
 * for a switch.
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

static int rows_apps(void)   { return perm_rows() + card_rows_n() + 2; } /* +1 cap, +1 back */
static int rows_appact(void) { return appact_n() + 1; }                  /* this subject's doors + back */
static int rows_about(void)  { return 5 + 1; }             /* 5 facts + back      */

/* The subject PG_APPACT is showing, by registry index when it is on the dial
 * (the same convention s_perm_slot uses for PG_CAPS, so the two "which app"
 * states are one kind of state), and by snapshot index when it is a card file
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
static orbapp_scan_entry_t s_card_ent[ORB_APP_SCAN_MAX];
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

/* The doors this subject answers, in the order an owner wants them. A dial row
 * that runs gets show; one that is off or faulted gets install (the hub's own
 * answer for that state — an install press carries the enable word and the
 * fault clear); both get uninstall, and both get remove from card, which the
 * worker answers honestly when the card holds nothing. */
enum { ACT_SHOW = 0, ACT_INSTALL, ACT_UNINSTALL, ACT_CARDRM };

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
        if (a->enabled && !a->failed) ids[n++] = ACT_SHOW;
        else                          ids[n++] = ACT_INSTALL;
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

static void open_appact(const orb_app_t *a)
{
    s_door_slot = orb_app_index_of(a);
    s_doormode  = 0;                    /* a registry row owns the page */
    s_card_sub  = -1;
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
    snprintf(out, outsz, "%s  %s", a->title ? a->title : a->slug,
             on_glass ? "on the dial" : (a->enabled ? "enabled" : "off"));
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
    /* The door page is armed through its own open hook, exactly as the top
     * page arms PG_APP: which app a page is about is the page's business, and
     * a second place that assigns it is a second place to get it wrong. */
    s_pages[PG_APPACT].open(a);
    go_to(PG_APPACT, 0);
    repaint();
}

static const char *title_apps(void) { return "APPS"; }
static const char *hint_apps(void)  { return "rotate: choose   push: open"; }

/* Leaving the list ends the door page's claim on a subject and both arms with
 * it: an arm that outlived its page would remove on the next press anywhere.
 * The entry asks the worker for a card walk, so the install tier is as fresh
 * as the last time the owner stood on this page; the walk lands by repaint,
 * never under the cursor (see the snapshot rule above). */
static void enter_apps(void)
{
    s_door_slot  = -1;
    s_doormode   = 0;
    s_card_sub   = -1;
    unarm_arms();
    job_post(job_scan, 0);
}

/* The doors this subject answers, in the order appact_id() lays them out. D38
 * cut the enable switch: the hub has no toggle and the dial now matches it —
 * an app that is off or faulted gets an install row (the press that carries
 * the enable word and the fault clear), and every subject gets uninstall and
 * remove-from-card. The row count and label are one function, so a page cannot
 * draw a door its press would refuse. */
static void label_appact(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const int id = appact_id(row);
    const bool armed_u = (s_uninst_arm == door_subject());
    const bool armed_c = (s_cardrm_arm == door_subject());
    switch (id) {
    case ACT_SHOW: {
        const orb_app_t *a = orb_app_row((size_t)s_door_slot);
        const bool on_glass = a && app_on_glass(a);
        if (sel_hi) *sel_hi = on_glass;
        snprintf(out, outsz, "%s", on_glass ? "showing on the dial"
                                            : "show on the dial");
        return;
    }
    case ACT_INSTALL:
        snprintf(out, outsz, "install");
        return;
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

/* slot in the high bits, the wanted enable state low. Same shape as cap_pack
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

static void act_appact(int row)
{
    const int id = appact_id(row);
    if (id < 0) { go_to(PG_APPS, 0); repaint(); return; }   /* the back row, or subject gone */
    appact_stash();
    switch (id) {
    case ACT_SHOW: {
        /* The carousel's own door, on the LVGL task where lv_screen_load()
         * must run. It refuses an app that cannot walk, and the line says so
         * rather than leaving the owner staring at a screen that did not move.
         * NOT_FOUND is its own sentence: it is not the app failing, it is the
         * platform not yet drawing that kind — a code app's layers have no
         * compositor commit until P8 lands, and the owner reading "will not
         * go on the dial" after a clean install would look in the app, not
         * the phase list. */
        const orb_app_t *a = orb_app_row((size_t)s_door_slot);
        if (!a) { go_to(PG_APPS, 0); repaint(); return; }
        const esp_err_t e = orb_app_activate(a);
        snprintf(s_ui.status, sizeof(s_ui.status),
                 e == ESP_OK ? "\"%s\": on the dial"
                             : e == ESP_ERR_NOT_FOUND
                 ? "\"%s\": code apps draw layers the dial cannot paint yet"
                             : "\"%s\": %s will not go on the dial",
                 a->title ? a->title : a->slug, a->slug);
        ESP_LOGI(TAG, "apps: activate \"%s\" -> %s", a->slug, esp_err_to_name(e));
        repaint();
        return;
    }
    case ACT_INSTALL:
        /* Off-or-faulted registry row: the press is the enable door's ON, which
         * is C2's fault clear too. Uninstalled card file: the press installs
         * from the card and turns the row on, the hub's one-press shape. */
        if (s_doormode) job_post(job_install_card, 0);
        else {
            const orb_app_t *a = orb_app_row((size_t)s_door_slot);
            if (!a) { go_to(PG_APPS, 0); repaint(); return; }
            job_post(job_enable, door_pack(a, 1));
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

static void job_enable(void *arg)
{
    const int pack = *(const int *)arg;
    const int slot = pack >> 2, on = pack & 1;
    ui_t out; memset(&out, 0, sizeof out);
    const orb_app_t *a = orb_app_row((size_t)slot);
    /* The row can go between the press and the worker: an uninstall from the
     * browser, or a reboot that resolved the slot away, both leave the pack
     * naming an index the registry no longer holds. The sentence names which
     * door found nothing, so an owner reading the dial can tell a switch that
     * never happened from a cap toggle that did. */
    if (!a) { snprintf(out.status, sizeof out.status, "that switch found no app"); worker_publish(&out); return; }
    /* The status line is 72 bytes and a whole title has to fit beside the
     * sentence, so the door is asked for a line that fits rather than for
     * everything it knows: .48 leaves room for the slug and the quotes, and
     * every refusal it returns is complete well inside that. */
    char why[48] = "";
    const esp_err_t e = orb_app_enable(a->slug, on != 0, why, sizeof why);
    /* The refusal sentence is the door's own, verbatim. D38: this job now answers
     * only the install row's press — turning an off-or-faulted row back on, which
     * doubles as C2's fault clear — so the OK word says the hub's word, and the
     * dial has no press that turns a row off any more. */
    snprintf(out.status, sizeof out.status, "\"%s\": %s",
             a->title ? a->title : a->slug,
             e == ESP_OK ? (on ? "installed" : "off") : why);
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
    worker_publish(&out);
}

/* Install a card file the dial does not run, then put the row on the carousel —
 * one press, the hub's shape: h_apps_install's own queue would split the copy
 * and the enable word across two witnesses the dial never reads, so the worker
 * runs the ladder synchronously, as every other dial door does. */
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
    char why2[48] = "";
    if (s_pend_slug[0] &&
        orb_app_enable(s_pend_slug, true, why2, sizeof why2) != ESP_OK)
        snprintf(out.status, sizeof out.status, "\"%.30s\": installed, but %.21s",
                 appact_name(), why2);
    else
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
    const esp_err_t e = orb_app_uninstall(a->slug, why, sizeof why);
    /* The card file survives on purpose (B4), and the line says so: an owner
     * who reads a bare "uninstalled" looks for the app on the card next time
     * and finds it, which reads as if the first refusal had been a lie. */
    snprintf(out.status, sizeof out.status, "\"%s\": %s",
             a->title ? a->title : a->slug,
             e == ESP_OK ? "uninstalled, the card copy stays" : why);
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

/* ABOUT is a read-only list and says so in its hint: five facts, no press that
 * changes anything. Every getter here is a RAM read — no NVS, no card, no
 * queue — so the page paints instantly and cannot answer "working...". */
static void label_about(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    switch (row) {
    case 0:
        snprintf(out, outsz, "firmware  %s", esp_app_get_description()->version);
        return;
    case 1:
        snprintf(out, outsz, "address  %s", orb_net_ip_str());
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
        snprintf(out, outsz, "zone  %s", orb_net_tz_current());
        return;
    default: snprintf(out, outsz, "<- back"); return;
    }
}

static const char *title_about(void) { return "ABOUT"; }
static const char *hint_about(void)  { return "rotate: read   push: nothing here"; }


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
 *     verbatim by rule (see job_enable's note), and those sentences carry em
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
static void paint_preview(void)
{
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
    int win = 0;
    if (rows > ROW_MAX) {
        win = s_cur - ROW_MAX / 2;
        if (win < 0) win = 0;
        if (win > rows - ROW_MAX) win = rows - ROW_MAX;
    }

    char buf[64];
    char gl[96];                                   /* fold scratch: status is 72 B */
    for (int i = 0; i < ROW_MAX; i++) {
        const int r = win + i;
        if (rows == 0 || r >= rows) {
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
        lv_label_set_text(s_title, "MENU");
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_48, 0);
        lv_label_set_text(s_hint, "push to open");
        lv_label_set_text(s_status, "");
    } else {
        char head[64];
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
        title_line(head, sizeof head);
        dial_text(gl, sizeof gl, head);
        lv_label_set_text(s_title, gl);
        dial_text(gl, sizeof gl, page_hint());
        lv_label_set_text(s_hint, gl);
        dial_text(gl, sizeof gl, s_ui.status);
        lv_label_set_text(s_status, gl);
    }

    /* The plane over the list, asked and drawn in the same pass that decided
     * the list: one repaint, one source of truth about which row is under the
     * cursor. Hidden on every page that is not an app's open group — a plane
     * surviving a page change is the last theme's picture over a page that
     * never mentioned it. */
    paint_preview();

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
    lv_label_set_text(s_title, "MENU");
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

    repaint();
    ESP_LOGI(TAG, "menu ready (worker up, %d zones, %s)", orb_net_tz_count(),
             orb_sd_mounted() ? "card mounted" : "no card");
    return ESP_OK;

nomem:
    /* Tear the whole half-built subtree down (deleting s_scr frees its children
     * with it), null every cached pointer, and release the mutex, so a dropped
     * init leaves no dangling screen for menu_view_screen()/repaint() to touch
     * and nothing held across the failure. The next boot starts from empty. */
    lv_obj_del(s_scr);
    s_scr = NULL; s_title = NULL; s_status = NULL; s_hint = NULL; s_pv_img = NULL;
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
 * Only pages that re-enter from their own id are remembered: TOP, TZ, DISPLAY,
 * TO, PERMS, ABOUT. The rest cannot be, and the reason each is left out is a
 * correctness rule, not an oversight — PG_APP/PG_APPACT name an app that may
 * have uninstalled since (their open() resolves the pointer), PG_CAPS carries a
 * selected slot that a stored page id cannot re-derive, PG_SHARE is the window's
 * and not the owner's to return to, and PG_LANDING is unarmed. A remembered page
 * whose enter() hook re-derives the cursor (the zone list on the zone in force,
 * the timeout picker on the preset in force) takes the hook's answer, not the
 * stored row: the cursor there is a fact about the current setting, not a place
 * the owner parked it. */
#define MENU_POS_NS  "orb_menu"
#define MENU_POS_KEY_PG  "pg"
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
    const unsigned ok = (1u << PG_TOP) | (1u << PG_TZ) | (1u << PG_DISPLAY)
                      | (1u << PG_TO)  | (1u << PG_PERMS) | (1u << PG_ABOUT);
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
    s_pg = P(PG_APP);
}

static int ord_app(void) { return top_os_rows() + app_row_index(s_app); }
static int ord_caps(void) { return perm_row_of(orb_app_row((size_t)s_perm_slot)); }

static void leave_app(void)
{
    orb_app_opt_arm(s_app, false);
    s_app = NULL;
    s_group_open = -1;
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
    const int ar = vis_to_row(rows, row);
    if (ar < 0) { go_back(); repaint(); return; }   /* unreachable: rows_app sized it */
    orb_app_opt_sel(s_app, ar);
    repaint();
}

/* A press on the top page: the sub-table's row first — a row that acts (AP)
 * says so, a row that opens a page (TZ, DISPLAY, PERMISSIONS, and P5.3's
 * APPS/ABOUT) names the page — and below the sub-table, the dynamic app run.
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
    if (to == MENU_GO_EXIT) { input_router_menu_exit(); return; }
    if (s_pg->leave) s_pg->leave();
    s_pg  = P(to);
    s_cur = cur;
    if (s_pg->enter) s_pg->enter();
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
    [PG_TZ]      = { .rows = rows_tz, .label = label_tz, .title = title_tz, .hint = hint_tz,
                     .act = act_tz, .enter = enter_tz, .back_to = PG_TOP, .back_cur = 1 },
    [PG_APP]     = { .rows = rows_app, .label = label_app, .title = title_app, .hint = hint_app,
                     .act = act_app, .open = open_app, .ordinal = ord_app,
                     .leave = leave_app, .back_to = PG_TOP, .back_cur = 0 },
    [PG_DISPLAY] = { .rows = rows_display, .label = label_display, .title = title_display,
                     .hint = hint_display, .act = act_display,
                     .back_to = PG_TOP, .back_cur = 2 },
    [PG_TO]      = { .rows = rows_to, .label = label_to, .title = title_to, .hint = hint_to,
                     .act = act_to, .enter = enter_to, .back_to = PG_DISPLAY, .back_cur = 1 },
    [PG_PERMS]   = { .rows = rows_perms, .label = label_perms, .title = title_perms,
                     .act = act_perms, .back_to = PG_TOP, .back_cur = 3 },
    [PG_CAPS]    = { .rows = rows_caps, .label = label_caps, .title = title_caps,
                     .hint = hint_caps, .act = act_caps, .ordinal = ord_caps,
                     .back_to = PG_PERMS, .back_cur = 0 },
    /* APPS is the catalog and the two doors' only path on the glass (B4);
     * ABOUT is inert. Both sit in the OS's own run of the top page, so the
     * dynamic app rows and exit shift down two without any arithmetic here
     * changing — rows_top() reads TOP_OS_N by sizeof. */
    [PG_APPS]    = { .rows = rows_apps, .label = label_apps, .title = title_apps,
                     .hint = hint_apps, .act = act_apps, .enter = enter_apps,
                     .back_to = PG_TOP, .back_cur = 4 },
    [PG_APPACT]  = { .rows = rows_appact, .label = label_appact, .title = title_appact,
                     .hint = hint_appact, .act = act_appact, .open = open_appact,
                     .ordinal = ord_appact,
                     .back_to = PG_APPS, .back_cur = 0 },
    [PG_ABOUT]   = { .rows = rows_about, .label = label_about, .title = title_about,
                     .hint = hint_about, .back_to = PG_TOP, .back_cur = 5 },
    /* The share card: reachable from no row, carrying no rows of its own, and
     * answering no press with navigation — the whole page is the door. The
     * table holds it so the paint path stays the table's; the window installs
     * it (menu_view_share_gate), and only the window can take it off. */
    [PG_SHARE]   = { .rows = rows_share, .label = label_share, .title = title_share,
                     .hint = hint_share, .back_to = MENU_GO_NONE, .back_cur = 0 },
};

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
    if (s_pg->label) s_pg->label(row, out, outsz, sel_hi);
}

static const char *page_title(void) { return s_pg->title ? s_pg->title() : "MENU"; }

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
static void title_line(char *out, size_t outsz)
{
    const int rows = page_rows();
    if (rows > ROW_MAX)
        snprintf(out, outsz, "%s  %d/%d", page_title(), s_cur + 1, rows);
    else
        snprintf(out, outsz, "%s", page_title());
}

static const char *page_hint(void)
{
    if (orb_jobs_busy()) return "working...";
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

bool menu_view_opt_long(void)
{
    if (!s_armed) return false;                    /* the router owns the knob */

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
    if (!own_result && !app_grew && !pv_landed) return;
    s_dirty = false;

    if (own_result) {
        xSemaphoreTake(s_res_mx, portMAX_DELAY);
        s_ui = s_shared;
        xSemaphoreGive(s_res_mx);
    }

    /* The theme page's own results are the clock's to reap, through its
     * service() hook: a card walk that finishes while the owner has rotated to
     * the dial must still land, and the app owns what it means. What is left
     * here is this screen's own status line. */
    repaint();
}


