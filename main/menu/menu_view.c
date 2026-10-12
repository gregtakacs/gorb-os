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

#include "orb_net.h"
#include "orb_sd.h"
#include "orb_app.h"
#include "idle_dim.h"          /* DISPLAY submenu: the two runtime settings */
#include "input_router.h"     /* exit -> input_router_home() */

static const char *TAG = "menu";

/* Rows painted at once. The dial is round: content stays inside a centred
 * ~320 px column so the curve never clips a label. */
#define ROW_MAX      5

/* ---- the model ---- */

typedef enum {
    PG_LANDING = 0,   /* inert title; the knob belongs to the router  */
    PG_TOP,           /* OS items, then one row per app with options, then exit */
    PG_TZ,            /* the zone table                               */
    PG_APP,           /* one app's own options, rendered from its opt_* answers */
    PG_DISPLAY,       /* the two idle-dim settings                    */
    PG_TO,            /* the timeout presets                          */
    PG_COUNT
} page_t;

/* The OS's own rows on the top page. The theme picker is NOT among them: B2
 * rules the clock app supplies it and the shell only renders it, so it arrives
 * through the app rows below rather than as a page this file knows about.
 *
 * APPS and ABOUT are the plan's §7.2 top level too; they wait for P4, because
 * an APPS page has nothing to list until a card app and a catalog exist, and a
 * row that says "nothing installed yet" on a device with two compiled-in apps
 * would be a lie about the thing the page is for. */
typedef enum { IT_AP = 0, IT_TZ, IT_DISPLAY, IT_OS_N } top_item_t;

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

static page_t s_page = PG_LANDING;
/* Which app PG_APP is showing. Set on entry from the registry and cleared on
 * leave, so a repaint can never render the previous app's rows under the new
 * one's header — the two-second card walk that lands after the owner has
 * already rotated away is exactly when that would show. */
static const orb_app_t *s_app;
static bool   s_armed;
static int    s_cur;              /* cursor within the current page */
static int    s_painted_rows;     /* page_rows() the last repaint drew, for the PG_APP reap */

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
static int page_rows(void)
{
    /* Unarmed means no list at all, decided here rather than at each caller so
     * the painter and the cursor clamp cannot drift into disagreeing about
     * whether rows exist. This is the single authority for that question. */
    if (!s_armed) return 0;

    switch (s_page) {
        case PG_TOP:    return IT_OS_N + app_rows() + 1;   /* +1: exit */
        case PG_APP:    return s_app ? orb_app_opt_count(s_app) + 1 : 0;  /* +1: back */
        case PG_TZ:     return orb_net_tz_count() + 1;      /* last row is back */
        case PG_DISPLAY:return 3;                            /* dim, timeout, back */
        case PG_TO:     return TO_PRESET_N + 1;              /* last row is back */
        default:        return 0;
    }
}

/* Which page a "<- back" row returns to. */
static void go_back(void)
{
    const page_t from = s_page;
    if (from == PG_TO) {                 /* the timeout picker returns to its
                                          * parent, mid-list where it was left */
        s_page = PG_DISPLAY;
        s_cur  = 1;
        return;
    }
    if (from == PG_APP) {
        /* Leave the page the way it was entered: back on the row that opened it,
         * and the app disarmed. An app left armed off-glass would take the next
         * detent from the view actually showing. */
        int ai = 0;
        for (int i = 0; app_row(i); i++) { if (app_row(i) == s_app) { ai = i; break; } }
        orb_app_opt_arm(s_app, false);
        s_app  = NULL;
        s_page = PG_TOP;
        s_cur  = IT_OS_N + ai;
        return;
    }
    s_page = PG_TOP;
    s_cur  = (from == PG_TZ)     ? IT_TZ
           : (from == PG_DISPLAY)? IT_DISPLAY
           : 0;
}

static void row_text(int row, char *out, size_t outsz, bool *sel_hi)
{
    out[0] = '\0';
    if (sel_hi) *sel_hi = false;

    switch (s_page) {
        case PG_TOP:
            /* Rows after the OS's own are one per app that declares options,
             * labelled with the app's TITLE; the row after those is exit. Two
             * ways to get this wrong and both read as the knob being ignored:
             * casting the index to top_item_t and letting default answer "exit"
             * paints "exit" on the Clock row, and testing only `>= IT_OS_N`
             * sends the exit row looking for an app that is not there. */
            if (row >= IT_OS_N && row < IT_OS_N + app_rows()) {
                const orb_app_t *a = app_row(row - IT_OS_N);
                if (!a) { snprintf(out, outsz, "?"); return; }   /* unreachable: page_rows sized it */
                snprintf(out, outsz, "%s", a->title ? a->title : a->slug);
                return;
            }
            switch ((top_item_t)row) {
                case IT_AP:     snprintf(out, outsz, "AP");      break;
                case IT_TZ:     snprintf(out, outsz, "TZ");      break;
                case IT_DISPLAY: snprintf(out, outsz, "DISPLAY"); break;
                default:        snprintf(out, outsz, "exit");    break;
            }
            return;

        case PG_TZ: {
            const int n = orb_net_tz_count();
            if (row >= n) { snprintf(out, outsz, "<- back"); return; }
            /* The row the dial is actually on is marked, so re-entering the
             * picker answers "where am I?" without the owner having to remember
             * what they set. */
            const bool cur = (row == orb_net_tz_current_index());
            if (sel_hi) *sel_hi = cur;
            snprintf(out, outsz, "%s%s", orb_net_tz_label(row), cur ? "  *" : "");
            return;
        }

        case PG_APP: {
            if (!s_app) return;
            const int rows = orb_app_opt_count(s_app);
            if (row >= rows) { snprintf(out, outsz, "<- back"); return; }
            /* Label then value, in the shell's own words, off the app's answers:
             * this is the one line every options page shares, which is why the
             * dial's theme page and the browser's copy cannot drift. */
            const char *v = orb_app_opt_value(s_app, row);
            if (v && v[0]) snprintf(out, outsz, "%s  (%s)", orb_app_opt_label(s_app, row), v);
            else           snprintf(out, outsz, "%s", orb_app_opt_label(s_app, row));
            return;
        }

        case PG_DISPLAY:
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
            return;

        case PG_TO: {
            if (row >= TO_PRESET_N) { snprintf(out, outsz, "<- back"); return; }
            /* Same current-value mark as the zone list: the picker must answer
             * "which one am I on?" before anybody starts scrolling. */
            const bool cur = (s_to_preset[row].s == idle_dim_timeout_s());
            if (sel_hi) *sel_hi = cur;
            snprintf(out, outsz, "%s%s", s_to_preset[row].lbl, cur ? "  *" : "");
            return;
        }

        default: return;
    }
}

static const char *page_title(void)
{
    switch (s_page) {
        case PG_TZ:     return "TIME ZONE";
        case PG_APP:    return (s_app && s_app->title) ? s_app->title : "OPTIONS";
        case PG_DISPLAY:return "DISPLAY";
        case PG_TO:     return "DIM TIMEOUT";
        default:        return "MENU";
    }
}

static const char *page_hint(void)
{
    if (orb_jobs_busy()) return "working...";
    switch (s_page) {
        case PG_TZ:      return "rotate: choose   push: set";
        case PG_DISPLAY: return "rotate: choose   push: change";
        case PG_APP:     return "rotate: choose   push: change";
        case PG_TO:      return "rotate: choose   push: set";
        default:         return "rotate: choose   push: open";
    }
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
     * it. Keyed off s_armed rather than s_page, because s_page is only authoritative
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
        lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
        lv_label_set_text(s_title, page_title());
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

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);

    s_title = lv_label_create(s_scr);
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
        lv_obj_set_size(s_row[i], row_w, row_h);
        lv_obj_set_style_radius(s_row[i], 10, 0);
        lv_obj_set_style_pad_all(s_row[i], 0, 0);
        lv_obj_set_style_border_width(s_row[i], 0, 0);
        lv_obj_set_scrollable(s_row[i], false);
        lv_obj_set_pos(s_row[i], (SCREEN_WIDTH - row_w) / 2, row_top + i * (row_h + row_gap));

        s_row_lbl[i] = lv_label_create(s_row[i]);
        lv_obj_set_style_text_font(s_row_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_row_lbl[i], COL_TXT_LO, 0);
        lv_label_set_text(s_row_lbl[i], "");
        lv_obj_center(s_row_lbl[i]);
    }

    /* Result line: why a scan failed, or what was just set. It is the only place
     * the dial can tell the owner why the card did not answer, which orb_sd's
     * silent ESP_OK/ESP_FAIL otherwise leaves them to guess at. */
    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_status, lv_color_make(0xe0, 0xb0, 0x40), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -66);

    s_hint = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_hint, COL_TXT_LO, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_hint, "push to open");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -34);

    repaint();
    ESP_LOGI(TAG, "menu ready (worker up, %d zones, %s)", orb_net_tz_count(),
             orb_sd_mounted() ? "card mounted" : "no card");
    return ESP_OK;
}

lv_obj_t *menu_view_screen(void) { return s_scr; }

bool menu_view_armed(void) { return s_armed; }

void menu_view_show(void) { repaint(); }

void menu_view_set_armed(bool armed)
{
    s_armed = armed;
    s_page  = armed ? PG_TOP : PG_LANDING;
    s_cur   = 0;
    /* Drop last attempt's line, so re-entering shows the menu and not a stale
     * complaint about a card that may since have been swapped. */
    s_ui.status[0] = '\0';
    repaint();
    ESP_LOGI(TAG, "%s", armed ? "menu open" : "menu idle");
}

/* Act on the row under the cursor. */
static void activate(void)
{
    switch (s_page) {
        case PG_TOP:
            /* Rows IT_OS_N .. IT_OS_N+app_rows()-1 are apps; the row after them
             * is exit. Testing the index range rather than falling through the
             * switch is the only way to keep both, because the enum has no name
             * for a row the table decides at runtime. */
            if (s_cur >= IT_OS_N && s_cur < IT_OS_N + app_rows()) {
                const int ai = s_cur - IT_OS_N;
                const orb_app_t *a = app_row(ai);
                if (!a) { s_ui.status[0] = '\0'; repaint(); return; }
                s_app  = a;
                s_page = PG_APP;
                s_cur  = 0;
                orb_app_opt_arm(a, true);
                repaint();
                return;
            }
            switch ((top_item_t)s_cur) {
                case IT_AP:
                    /* Stubbed on purpose: joining needs a passphrase, and a
                     * single push-input cannot type one. The browser page stays
                     * the way in until a keypad screen exists. */
                    snprintf(s_ui.status, sizeof(s_ui.status),
                             "AP needs a passphrase: use the web page");
                    ESP_LOGI(TAG, "AP selected (stub)");
                    repaint();
                    return;
                case IT_TZ: {
                    s_page = PG_TZ;
                    /* Start on the zone in effect rather than at row 0: the
                     * common reason to open this is to nudge a neighbouring
                     * city, and UTC-first would make every visit a scroll. */
                    const int c = orb_net_tz_current_index();
                    s_cur = (c < 0) ? 0 : c;
                    repaint();
                    return;
                }
                case IT_DISPLAY:
                    s_page = PG_DISPLAY;
                    s_cur  = 0;
                    repaint();
                    return;
                default:
                    /* Reaching past the OS rows means an app row, and the only
                     * row after those is exit. Both were handled above by the
                     * PG_TOP guard, so this arm is exit alone: disarming here
                     * rather than leaving it to the home path is what stops an
                     * app being left armed off-glass after the owner leaves. */
                    menu_view_set_armed(false);
                    input_router_home();
                    return;
            }
            return;

        case PG_APP: {
            if (!s_app) { go_back(); repaint(); return; }
            const int rows = orb_app_opt_count(s_app);
            if (s_cur >= rows) { go_back(); repaint(); return; }    /* <- back */
            /* The press belongs to the app; the cursor never leaves this file.
             * opt_sel() may start a job, and it may not paint: repaint() below
             * is what puts the app's new answer on the glass. */
            orb_app_opt_sel(s_app, s_cur);
            repaint();
            return;
        }

        case PG_TZ: {
            const int n = orb_net_tz_count();
            if (s_cur >= n) { go_back(); repaint(); return; }   /* <- back */
            /* Applied on the worker: net_tz_set() writes NVS then tzset()s, and
             * the rule in this file is that nothing which can block runs on the
             * task that paints the dial. The result line arrives via
             * menu_view_service(). */
            job_post(job_tz, s_cur);
            repaint();                                  /* shows "working..." */
            return;
        }

        case PG_DISPLAY: {
            if (s_cur >= 2) { go_back(); repaint(); return; }        /* <- back */
            if (s_cur == 0) {
                /* The toggle runs on the worker: the setter nvs_commits, and the
                 * rule in this file is that nothing which can block runs on the
                 * task that paints the dial (same reason as the zone write). The row
                 * repaints from the getter as soon as the result lands. */
                job_post(job_dim_en, 0);
                repaint();                                  /* shows "working..." */
                return;
            }
            /* Timeout row: open the picker, started on the row that matches the
             * value in force -- the same "where am I?" courtesy PG_TZ gives. */
            s_page = PG_TO;
            s_cur  = 0;
            const uint32_t cur = idle_dim_timeout_s();
            for (int i = 0; i < TO_PRESET_N; i++)
                if (s_to_preset[i].s == cur) { s_cur = i; break; }
            repaint();
            return;
        }

        case PG_TO: {
            if (s_cur >= TO_PRESET_N) { go_back(); repaint(); return; }  /* back */
            job_post(job_dim_to, s_cur);
            /* Choosing IS leaving: back to DISPLAY, parked on the timeout row,
             * rather than leaving the picker up showing "working...". The row
             * repaints from the getter when the worker's write lands, so the
             * value the user just picked is already right when they arrive. */
            go_back();
            repaint();
            return;
        }

        default: return;
    }
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
    if (next == s_cur) return;
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
    const bool app_grew = (s_page == PG_APP && s_app &&
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


