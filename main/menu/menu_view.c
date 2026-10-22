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
#include "idle_dim.h"          /* DISPLAY submenu: the two runtime settings */
#include "input_router.h"     /* exit -> input_router_home() */

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
     *  validates its shape and closes its group — a generic dispatcher cannot
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
static int  top_os_rows(void);   static int app_row_index(const orb_app_t *);
static void act_ap(void);        static void go_to(int to, int cur);
static int  cap_pack(const orb_app_t *, int row);
static int  door_pack(const orb_app_t *, int act);
static void job_grant(void *arg);
static void job_enable(void *arg);   static void job_uninstall(void *arg);
static int  page_rows(void);
static void row_text(int, char *, size_t, bool *);
static const char *page_title(void);
static void title_line(char *, size_t);
static const char *page_hint(void);
static void go_back(void);       static void activate(void);
static void repaint(void);       static void job_post(orb_job_fn fn, int idx);


/** How many app rows the top page carries: one per walkable entry that declares
 *  options, in table order, which is the carousel's order — so the menu's list
 *  and the knob's walk cannot disagree about what exists. */
static int app_rows(void)
{
    int n = 0;
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

/** The first (or last) ITEM the open group owns, as an app row, or -1 if none. The
 *  cursor's clamp needs these two, and scanning from the group out is the only way
 *  to find them that cannot disagree with owner_group(). */
static int group_edge(int rows, bool last)
{
    if (s_group_open < 0) return -1;
    int found = -1;
    for (int i = s_group_open + 1; i < rows; i++) {
        if (orb_app_opt_kind(s_app, i) == OPT_GROUP) break;    /* the next header ends it */
        if (drawn_kind(rows, i) == OPT_ITEM) {
            found = i;
            if (!last) break;
        }
    }
    return found;
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
static int rows_app(void) { return s_app ? visible_rows(orb_app_opt_count(s_app)) + 1 : 0; }

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
     * app gives for what is inside it, and the shell overwrites that with the
     * one fact the app is not allowed to know: that it is open (D12). Items are
     * indented, because a list that nests has to say so on the glass and not
     * only in the ABI. */
    const char *v = (k == OPT_GROUP && s_group_open == ar) ? "open"
              : orb_app_opt_value(s_app, ar);
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
 * card nobody presses. */
static const char *hint_tz(void) { return "rotate: choose   push: set"; }
static const char *hint_to(void) { return "rotate: choose   push: set"; }
static const char *hint_caps(void) { return "rotate: choose   push: toggle"; }
static const char *hint_app(void)
{
    return s_group_open >= 0
         ? "rotate: choose   push: change   hold: close"
         : "rotate: choose   push: change";
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
 * The four states are the plan's words, and the row says one of them rather
 * than a boolean: `on the dial` (the entry the glass is showing), `enabled`
 * (on the carousel, not showing), `on card` (installed and walkable but
 * switched off), and `failed: <reason>` (C2's fault record, shown with its
 * reason verbatim — the orb_theme_scan rule that a row which cannot be used is
 * still a row that must be explained). The reason wins over the enable state
 * because a failed app that reads merely "off" tells the owner to go looking
 * for a switch.
 *
 * One press cannot carry three doors, so an app row opens PG_APPACT rather than
 * cycling anything. B5's cap shows here as a row of its own: an owner at 8 of 8
 * needs the page to say so before they look for something to uninstall. */
static int rows_apps(void)   { return perm_rows() + 2; }   /* +1 cap row, +1 back */
static int rows_appact(void) { return 3 + 1; }             /* 3 doors + back      */
static int rows_about(void)  { return 5 + 1; }             /* 5 facts + back      */

/* The app PG_APPACT is showing, by registry index — the same convention
 * s_perm_slot uses for PG_CAPS, so the two "which app" states are one kind of
 * state rather than two ways to lose track of an app. s_uninst_arm is the
 * uninstall row's first-press arm, held as the same index: a row that can no
 * longer be pointed at cannot fire. */
static int s_door_slot  = -1;
static int s_uninst_arm = -1;

static void open_appact(const orb_app_t *a)
{
    s_door_slot = orb_app_index_of(a);
}
static int ord_appact(void)
{
    /* Which APPS row this app is on, so back lands where the owner came from
     * even if the row moved underneath them. */
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
    const int n = perm_rows();
    if (row >= n) {
        if (row == n) {   /* B5's cap, on the glass rather than in a manual */
            snprintf(out, outsz, "%d of %d slots used", n, ORB_APP_CAP);
            return;
        }
        snprintf(out, outsz, "<- back");
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
             on_glass ? "on the dial" : (a->enabled ? "enabled" : "on card"));
}

static void act_apps(int row)
{
    const orb_app_t *a = perm_row(row);
    if (!a) { go_back(); repaint(); return; }          /* the cap/back rows */
    /* The door page is armed through its own open hook, exactly as the top
     * page arms PG_APP: which app a page is about is the page's business, and
     * a second place that assigns it is a second place to get it wrong. */
    s_pages[PG_APPACT].open(a);
    go_to(PG_APPACT, 0);
    repaint();
}

static const char *title_apps(void) { return "APPS"; }
static const char *hint_apps(void)  { return "rotate: choose   push: open"; }

/* Leaving the list ends the door page's claim on an app and the arm with it:
 * an arm that outlived its page would uninstall on the next press anywhere. */
static void enter_apps(void)
{
    s_door_slot  = -1;
    s_uninst_arm = -1;
}

/* The three doors, in the order an owner wants them: put it on the glass,
 * switch it on or off, and only then take it off the machine. */
static void label_appact(int row, char *out, size_t outsz, bool *sel_hi)
{
    if (sel_hi) *sel_hi = false;
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    if (!a) { snprintf(out, outsz, "?"); return; }
    const bool on_glass = app_on_glass(a);
    switch (row) {
    case 0:
        if (sel_hi) *sel_hi = on_glass;
        snprintf(out, outsz, "%s", on_glass ? "showing on the dial"
                                            : "show on the dial");
        return;
    case 1:
        if (sel_hi) *sel_hi = a->enabled;
        snprintf(out, outsz, "switch: %s", a->enabled ? "on" : "off");
        return;
    case 2:
        /* The arm is stated on the row, not only in the status line: the owner
         * who walks back to this row after a detent sees the press is armed. */
        snprintf(out, outsz, "uninstall%s",
                 s_uninst_arm == s_door_slot ? " — press again" : "");
        return;
    default: snprintf(out, outsz, "<- back"); return;
    }
}

static const char *title_appact(void)
{
    static char act_title[48];
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    snprintf(act_title, sizeof act_title, "%s APP",
             a && a->title ? a->title : a ? a->slug : "?");
    return act_title;
}
/* Uninstall is the only irreversible row on the dial, so the hint names the
 * second press rather than letting a single push stand for consent. */
static const char *hint_appact(void) { return "rotate: choose   push: run   uninstall needs two presses"; }

/* slot in the high bits, the wanted enable state low. Same shape as cap_pack
 * for the same reason: this is the whole contract between a press and a write,
 * and the host suite builds packs against it directly. */
static int door_pack(const orb_app_t *a, int on)
{
    return (orb_app_index_of(a) << 2) | (on ? 1 : 0);
}

static void act_appact(int row)
{
    const orb_app_t *a = orb_app_row((size_t)s_door_slot);
    if (!a) { go_to(PG_APPS, 0); repaint(); return; }   /* uninstalled since */
    if (row == 0) {
        /* The carousel's own door, on the LVGL task where lv_screen_load()
         * must run. It refuses an app that cannot walk, and the line says so
         * rather than leaving the owner staring at a screen that did not move. */
        const esp_err_t e = orb_app_activate(a);
        snprintf(s_ui.status, sizeof(s_ui.status),
                 e == ESP_OK ? "\"%s\": on the dial"
                             : "\"%s\": %s will not go on the dial",
                 a->title ? a->title : a->slug, a->slug);
        ESP_LOGI(TAG, "apps: activate \"%s\" -> %s", a->slug, esp_err_to_name(e));
        repaint();
        return;
    }
    if (row == 1) { job_post(job_enable, door_pack(a, a->enabled ? 0 : 1)); repaint(); return; }
    if (row == 2) {
        /* Two presses, same row. Moving the cursor clears the arm (see
         * menu_view_apply_input), so a row the owner rotated away from can
         * never fire on the next press. */
        if (s_uninst_arm != s_door_slot) {
            s_uninst_arm = s_door_slot;
            snprintf(s_ui.status, sizeof(s_ui.status),
                     "\"%s\": press uninstall again to confirm",
                     a->title ? a->title : a->slug);
            repaint();
            return;
        }
        s_uninst_arm = -1;
        job_post(job_uninstall, door_pack(a, 0));
        repaint();
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
    /* The refusal sentence is the door's own, verbatim — including the one that
     * exists only on this surface: "carousel away from it first" is something
     * the dial can cause and the browser page cannot. Turning ON doubles as
     * C2's fault clear, which is why a failed row's press says "on" and the
     * next repaint drops its reason. */
    snprintf(out.status, sizeof out.status, "\"%s\": %s",
             a->title ? a->title : a->slug, e == ESP_OK ? (on ? "on" : "off") : why);
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

/* Repaint from the model. Called only from the LVGL task (via apply_input or
 * service), never from the worker — which is the entire point of the queue. */
static void repaint(void)
{
    if (!s_scr) return;

    const int rows = page_rows();
    s_painted_rows = rows;
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
    for (int i = 0; i < ROW_MAX; i++) {
        const int r = win + i;
        if (rows == 0 || r >= rows) {
            lv_label_set_text(s_row_lbl[i], "");
            lv_obj_set_style_bg_color(s_row[i], lv_color_black(), 0);
            continue;
        }
        bool mark = false;
        row_text(r, buf, sizeof(buf), &mark);
        lv_label_set_text(s_row_lbl[i], buf);
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
        lv_label_set_text(s_title, head);
        lv_label_set_text(s_hint, page_hint());
        lv_label_set_text(s_status, s_ui.status);
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
     * silent ESP_OK/ESP_FAIL otherwise leaves them to guess at. */
    s_status = lv_label_create(s_scr);
    if (!s_status) goto nomem;
    lv_obj_set_style_text_color(s_status, lv_color_make(0xe0, 0xb0, 0x40), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -66);

    s_hint = lv_label_create(s_scr);
    if (!s_hint) goto nomem;
    lv_obj_set_style_text_color(s_hint, COL_TXT_LO, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_hint, "push to open");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -34);

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
    s_scr = NULL; s_title = NULL; s_status = NULL; s_hint = NULL;
    for (int i = 0; i < ROW_MAX; i++) { s_row[i] = NULL; s_row_lbl[i] = NULL; }
    vSemaphoreDelete(s_res_mx);
    s_res_mx = NULL;
    return ESP_ERR_NO_MEM;
}

lv_obj_t *menu_view_screen(void) { return s_scr; }

bool menu_view_armed(void) { return s_armed; }

void menu_view_show(void) { repaint(); }

void menu_view_set_armed(bool armed)
{
    s_armed = armed;
    s_pg    = armed ? P(PG_TOP) : P(PG_LANDING);
    s_cur   = 0;
    /* Drop last attempt's line, so re-entering shows the menu and not a stale
     * complaint about a card that may since have been swapped. */
    s_ui.status[0] = '\0';
    repaint();
    ESP_LOGI(TAG, "%s", armed ? "menu open" : "menu idle");
}

/** The press a group header answers for: open it, or close the one already
 *  open. True means the press belonged to the header and the app must not hear
 *  about it (D12: the app answers what a row IS, and what a press on a header
 *  does belongs to the shell). The cursor does not move — an owner who just
 *  opened a list is looking at its header with its rows under it, and sliding
 *  the highlight out from under them would be the shell deciding what they
 *  meant. */
static bool group_press(int rows, int ar)
{
    if (drawn_kind(rows, ar) != OPT_GROUP) return false;
    s_group_open = (s_group_open == ar) ? -1 : ar;
    ESP_LOGI(TAG, "group row %d %s: %s", ar,
             s_group_open == ar ? "open" : "closed", orb_app_opt_label(s_app, ar));
    return true;
}

/* The app page's entry, and its two dynamic answers. One visit, one shape:
 * validated on entry rather than per repaint, because the log line is the
 * point and a page that logged it fifty times a second would bury the cursor
 * moves it is kept beside. The group starts closed so the page always opens
 * on the two rows that answer "what is on the card" at a glance, and the app
 * is armed so its rows can take the knob — both undone by leave_app() on the
 * way out, because an app left armed off-glass would take the next detent
 * from the view actually showing. */
static void open_app(const orb_app_t *a)
{
    s_app      = a;
    s_cur      = 0;
    s_group_open = -1;
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

/* The press of an app row — the half of this the generic dispatcher cannot
 * spell, because a group header answers for the rows under it. The press
 * belongs to the app, at the app's OWN row number, not the glass position the
 * cursor holds: handing an app a visible index would make a closed group
 * every app's problem to renumber. opt_sel() may start a job, and it may not
 * paint: repaint() below is what puts the app's new answer on the glass. */
static void act_app(int row)
{
    const int rows = orb_app_opt_count(s_app);
    const int ar = vis_to_row(rows, row);
    if (ar < 0) { go_back(); repaint(); return; }   /* unreachable: rows_app sized it */
    if (group_press(rows, ar)) { repaint(); return; }
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
    if (to == MENU_GO_EXIT) { menu_view_set_armed(false); input_router_home(); return; }
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

    /* Inside an open group the clamp is the group's, not the page's (D12): a detent
     * that could carry the cursor out past the header would leave the owner who
     * just opened a sub-menu somewhere down the page with no way back to the row
     * that shuts it, which is how a nesting feature becomes a lost knob. The way
     * out is the long press, and it is stated in the hint. */
    if (s_pg == P(PG_APP) && s_app && s_group_open >= 0) {
        const int arows = orb_app_opt_count(s_app);
        const int cur = vis_to_row(arows, s_cur);
        if (cur >= 0 && drawn_kind(arows, cur) == OPT_ITEM) {
            const int lo = row_to_vis(arows, group_edge(arows, false));
            const int hi = row_to_vis(arows, group_edge(arows, true));
            if (lo >= 0 && hi >= lo) {
                if (next < lo) next = lo;
                if (next > hi) next = hi;
            }
        }
    }
    if (next == s_cur) return;
    /* Any move of the cursor ends the uninstall arm (P5.3): the confirmation is
     * a second press on THE SAME ROW, and an arm that survived a detent would
     * let the press that follows a rotation erase an app the owner had already
     * walked away from. */
    s_uninst_arm = -1;
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
    /* Only an app's option page with a group actually open spends the press.
     * Everything else — the landing, the menu's own pages, a page with no
     * nesting — answers false and lets the hold home as C8 rules it.
     *
     * The cursor goes back to the header the items just vanished under: leaving
     * it on an un-drawn row is the knob appearing not to work, because the next
     * detent would move from a row the glass is not showing. */
    if (!s_armed || s_pg != P(PG_APP) || !s_app || s_group_open < 0) return false;
    const int rows = orb_app_opt_count(s_app);
    const int at = row_to_vis(rows, s_group_open);
    ESP_LOGI(TAG, "hold closes group row %d: %s", s_group_open,
             orb_app_opt_label(s_app, s_group_open));
    s_group_open = -1;
    s_cur = (at < 0) ? 0 : at;
    repaint();
    return true;
}

void menu_view_service(void)
{
    /* Two independent reasons to repaint, either of which can fire without the
     * other: this screen's own worker landing a result (s_dirty), and the app
     * PG_APP is showing gaining or losing rows out from under a paint. The
     * second is the one that makes a theme page reachable at all — entering it
     * posts the clock's card walk, that walk reaps in orb_app_service_all()
     * BEFORE menu_service is ever reached, so by the time this runs the app
     * already has four rows the glass has never drawn. Without the count
     * re-check the page opens on an empty list and only fills in once the owner
     * turns the knob, which reads as the card being ignored. */
    const bool app_grew = (s_pg == P(PG_APP) && s_app &&
                           page_rows() != s_painted_rows);
    const bool own_result = s_dirty;
    if (!own_result && !app_grew) return;
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


