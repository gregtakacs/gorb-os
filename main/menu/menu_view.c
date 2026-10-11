/**
 * menu_view.c — the dial's options screen (see header for the contract).
 *
 * Rendered as a small pool of row objects repainted from module statics, the way
 * color_picker.c does it, rather than as an lv_list: the dial is 466x466 and the
 * router owns the only knob, so a real scrollable container would fight the
 * gesture that is supposed to drive it.
 *
 * ---- why there is a task in here ---------------------------------------------
 *
 * Two of the four options block for seconds, on the CARD MUTEX, which a browser
 * theme upload can be holding for 38-48 s (measured, .clinerules):
 *
 *   orb_theme_scan()        walks /sd/themes and opens every container to read
 *                           its header — unbounded by whatever else holds the card.
 *   orb_theme_stage_load()  streams a ~3.7 MB container through CRC — measured at
 *                           2952 ms on the bench.
 *
 * Neither may run where the widgets are touched. input_router's poll is an
 * lv_timer on the LVGL task, and that same callback is the ONLY reader of the
 * knob and the only thing that repaints the dial. Calling them inline means a
 * frozen screen and a dead knob for as long as the card takes — the exact failure
 * orb_theme.c's upload_busy guard was written to avoid for the web page, one
 * layer up. So those calls happen on the `orb_menu` task below and only their
 * small, already-copied results reach widgets, via menu_view_service().
 *
 * This module never takes the card lock itself; it goes through the public
 * orb_theme_* entry points, which own their own locking. Holding the mutex
 * across a repaint would be a self-deadlock with a screen attached.
 */

#include "menu_view.h"
#include "board_config.h"
#include "knob.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "orb_net.h"
#include "orb_sd.h"
#include "orb_theme.h"
#include "idle_dim.h"          /* DISPLAY submenu: the two runtime settings */
#include "input_router.h"     /* exit -> input_router_home() */

static const char *TAG = "menu";

/* Rows painted at once. The dial is round: content stays inside a centred
 * ~320 px column so the curve never clips a label. */
#define ROW_MAX      5
#define THEME_MAX    8        /* a card realistically holds a handful of themes */

/* ---- the model ---- */

typedef enum {
    PG_LANDING = 0,   /* inert title; the knob belongs to the router  */
    PG_TOP,           /* AP / TZ / THEME / DISPLAY / exit             */
    PG_TZ,            /* the zone table                               */
    PG_THEME,         /* the card's themes                            */
    PG_ASK,           /* staged: reboot now, or keep the current dial */
    PG_DISPLAY,       /* the two idle-dim settings                    */
    PG_TO,            /* the timeout presets                          */
    PG_COUNT
} page_t;

typedef enum { IT_AP = 0, IT_TZ, IT_THEME, IT_DISPLAY, IT_EXIT, IT_TOP_N } top_item_t;

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
static bool   s_armed;
static int    s_cur;              /* cursor within the current page */

/* What the worker last brought back, copied out for the UI. */
typedef struct {
    int               n;                                    /* themes listed */
    orb_theme_entry_t e[THEME_MAX];
    bool              staged;                               /* stage_load ok */
    char              staged_slug[ORB_THEME_SLUG_MAX + 1];
    char              status[72];                           /* line, or ""   */
} ui_t;

static ui_t   s_ui;               /* read by the repaint, LVGL task only  */
static ui_t   s_shared;           /* written by the worker, under s_res_mx */
static SemaphoreHandle_t s_res_mx;
static TaskHandle_t      s_job_task;
static QueueHandle_t     s_job_q;
static volatile bool     s_dirty; /* worker published, UI has not reaped it */
static volatile bool     s_busy;  /* a job is outstanding                   */

typedef enum { JOB_SCAN = 0, JOB_STAGE, JOB_TZ, JOB_DIM_EN, JOB_DIM_TO } job_kind_t;
typedef struct {
    job_kind_t kind;
    char       slug[ORB_THEME_SLUG_MAX + 1];
    int        idx;                       /* JOB_TZ: the row to apply;
                                           * JOB_DIM_TO: index into s_to_preset */
} job_t;


/* Reboot is armed on a timer rather than called inline so the "installed" frame
 * actually reaches the panel: esp_restart() from inside the repaint would make
 * the last thing the owner ever sees be the frame before it. Same shape as
 * h_reboot()'s 750 ms one-shot in orb_net.c, for the same reason. */
static void reboot_fire(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rebooting to install the staged theme");
    esp_restart();
}

static void worker_publish(const ui_t *src)
{
    xSemaphoreTake(s_res_mx, portMAX_DELAY);
    s_shared = *src;
    xSemaphoreGive(s_res_mx);
    s_busy  = false;
    s_dirty = true;                 /* published last: the reader trusts the copy */
}

static void job_worker(void *arg)
{
    (void)arg;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_job_q, &j, portMAX_DELAY) != pdTRUE) continue;

        ui_t out;
        memset(&out, 0, sizeof(out));

        if (j.kind == JOB_SCAN) {
            /* An upload owns the card for tens of seconds; say so rather than
             * queueing behind it, which from the dial would just look like a
             * menu that stopped answering. */
            if (orb_theme_upload_busy()) {
                snprintf(out.status, sizeof(out.status),
                         "an upload is using the card");
            } else if (!orb_sd_mounted()) {
                snprintf(out.status, sizeof(out.status), "no card in the socket");
            } else {
                const int n = orb_theme_scan(out.e, THEME_MAX);
                if (n < 0) {
                    snprintf(out.status, sizeof(out.status),
                             "the card could not be read");
                } else {
                    out.n = n;
                    if (n == 0) {
                        snprintf(out.status, sizeof(out.status),
                                 "no themes on the card yet");
                    }
                }
            }
        } else if (j.kind == JOB_STAGE) {
            const esp_err_t er = orb_theme_stage_load(j.slug);
            if (er != ESP_OK) {
                snprintf(out.status, sizeof(out.status), "\"%s\" refused: %s",
                         j.slug, esp_err_to_name(er));
            } else {
                out.staged = true;
                snprintf(out.staged_slug, sizeof(out.staged_slug), "%s", j.slug);
            }
        } else if (j.kind == JOB_DIM_EN || j.kind == JOB_DIM_TO) {
            /* The DISPLAY submenu's two writes, here for the JOB_TZ reason
             * verbatim: both setters nvs_commit, and an NVS commit can hit a
             * sector erase. The getters stay on the LVGL task (row_text reads
             * RAM only); only the persistence crosses over. */
            if (j.kind == JOB_DIM_EN) {
                const bool want = !idle_dim_enabled();
                const esp_err_t er = idle_dim_set_enabled(want);
                if (er == ESP_ERR_NOT_SUPPORTED)
                    snprintf(out.status, sizeof(out.status),
                             "the dim is not in this build");
                else
                    /* NVS failure still left the RAM setting in force, and the
                     * line says which it cost: without the qualifier a
                     * "dim after idle: off" that dies at the next reboot reads
                     * as a switch that un-toggles itself. */
                    snprintf(out.status, sizeof(out.status),
                             "dim after idle: %s%s", want ? "on" : "off",
                             (er == ESP_OK) ? "" : " (until reboot, nvs refused)");
            } else {
                const to_preset_t *p = &s_to_preset[j.idx];
                const esp_err_t er = idle_dim_set_timeout_s(p->s);
                if (er == ESP_ERR_NOT_SUPPORTED)
                    snprintf(out.status, sizeof(out.status),
                             "the dim is not in this build");
                else
                    snprintf(out.status, sizeof(out.status),
                             "timeout: %s%s", p->lbl,
                             (er == ESP_OK) ? "" : " (until reboot, nvs refused)");
            }
        } else {
            /* TZ is here rather than inline for the same reason as the card
             * work: net_tz_set() writes NVS and then tzset()s. An NVS write is
             * normally milliseconds but can hit a sector erase, and a dial that
             * drops frames while somebody chooses a city is exactly the sort of
             * thing nobody can reproduce later. */
            const esp_err_t er = orb_net_tz_select(j.idx);
            if (er != ESP_OK) {
                snprintf(out.status, sizeof(out.status), "\"%s\" not set (%s)",
                         orb_net_tz_key(j.idx), esp_err_to_name(er));
            } else {
                snprintf(out.status, sizeof(out.status), "zone set: %s",
                         orb_net_tz_label(j.idx));
            }
        }

        worker_publish(&out);

        /* High-water mark after every job: the sizing above is a copy of the
         * httpd worker's proven value, and this is the line that says whether it
         * is actually holding for the file sizes on a real card. Zero-ish here
         * means the next theme that overflows is a crash, not a log line. */
        ESP_LOGI(TAG, "job %d done, %u B stack never used",
                 (int)j.kind, (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    }
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
        case PG_TOP:    return IT_TOP_N;
        case PG_TZ:     return orb_net_tz_count() + 1;      /* last row is back */
        case PG_THEME:  return s_busy ? 0 : (s_ui.n + 1);
        case PG_ASK:    return 2;
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
    s_page = PG_TOP;
    s_cur  = (from == PG_TZ)     ? IT_TZ
           : (from == PG_THEME)  ? IT_THEME
           : (from == PG_DISPLAY)? IT_DISPLAY
           : 0;
}

static void row_text(int row, char *out, size_t outsz, bool *sel_hi)
{
    out[0] = '\0';
    if (sel_hi) *sel_hi = false;

    switch (s_page) {
        case PG_TOP:
            switch ((top_item_t)row) {
                case IT_AP:     snprintf(out, outsz, "AP");      break;
                case IT_TZ:     snprintf(out, outsz, "TZ");      break;
                case IT_THEME:  snprintf(out, outsz, "THEME");   break;
                case IT_DISPLAY:snprintf(out, outsz, "DISPLAY"); break;
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

        case PG_THEME: {
            if (row >= s_ui.n) { snprintf(out, outsz, "<- back"); return; }
            const orb_theme_entry_t *e = &s_ui.e[row];
            if (e->err != ESP_OK) {
                /* A damaged theme is shown, not hidden — "no themes" and "three
                 * damaged themes" are different problems, and the owner has to
                 * be able to tell a bad card from a bad bake. */
                snprintf(out, outsz, "%s  (%s)", e->title[0] ? e->title : e->slug,
                         e->why[0] ? e->why : "not loadable");
            } else if (e->active) {
                snprintf(out, outsz, "%s  (on the dial)", e->title[0] ? e->title : e->slug);
            } else if (e->pending) {
                snprintf(out, outsz, "%s  (waiting to install)", e->title[0] ? e->title : e->slug);
            } else {
                snprintf(out, outsz, "%s", e->title[0] ? e->title : e->slug);
            }
            return;
        }

        case PG_ASK:
            snprintf(out, outsz, (row == 0) ? "reboot now" : "keep this dial");
            return;

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
        case PG_THEME:  return "THEME ON CARD";
        case PG_ASK:    return "THEME STAGED";
        case PG_DISPLAY:return "DISPLAY";
        case PG_TO:     return "DIM TIMEOUT";
        default:        return "MENU";
    }
}

static const char *page_hint(void)
{
    if (s_busy) return "working...";
    switch (s_page) {
        case PG_TZ:      return "rotate: choose   push: set";
        case PG_THEME:   return "rotate: choose   push: stage";
        case PG_ASK:     return "rotate: choose   push: confirm";
        case PG_DISPLAY: return "rotate: choose   push: change";
        case PG_TO:      return "rotate: choose   push: set";
        default:         return "rotate: choose   push: open";
    }
}

/* Ask the worker for a job. Drops the request while one is outstanding — a
 * double-tap on a row must not queue two multi-second card walks, and the first
 * result repaints the list anyway. */
static void job_post(job_kind_t kind, const char *slug, int idx)
{
    if (s_busy) return;
    job_t j = { .kind = kind, .idx = idx };
    if (slug) snprintf(j.slug, sizeof(j.slug), "%s", slug);
    s_busy = true;
    if (xQueueSend(s_job_q, &j, 0) != pdTRUE) {
        s_busy = false;
        ESP_LOGW(TAG, "job queue full -- dropped %s",
                 kind == JOB_SCAN  ? "scan"  :
                 kind == JOB_STAGE ? "stage" :
                 kind == JOB_TZ    ? "tz"    :
                 kind == JOB_DIM_EN? "dim toggle" : "dim timeout");
    }
}

/* Repaint from the model. Called only from the LVGL task (via apply_input or
 * service), never from the worker — which is the entire point of the queue. */
static void repaint(void)
{
    if (!s_scr) return;

    const int rows = page_rows();

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
    s_res_mx = xSemaphoreCreateMutex();
    if (!s_res_mx) return ESP_ERR_NO_MEM;

    /* Depth 2: the scan and the stage that follows it. More would let a fumbled
     * sequence of presses queue card work nobody is waiting for. */
    s_job_q = xQueueCreate(2, sizeof(job_t));
    if (!s_job_q) return ESP_ERR_NO_MEM;

    /* 4096, not net_task_loop's 3072: this worker runs the same two calls the
     * httpd worker runs (orb_theme_scan / orb_theme_stage_load, through FatFs
     * with its own stack frames), and 4096 is the size that has been proven on
     * hardware against a real 3.87 MB container. The watermark log below is what
     * turns "it worked" into a number to check on the next change. */
    if (xTaskCreate(job_worker, "orb_menu", 4096, NULL, 3, &s_job_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

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
                case IT_THEME:
                    s_page = PG_THEME;
                    s_cur  = 0;
                    s_ui.n = 0;
                    s_ui.status[0] = '\0';
                    job_post(JOB_SCAN, NULL, 0);     /* the card walk, off-task */
                    repaint();
                    return;
                case IT_DISPLAY:
                    s_page = PG_DISPLAY;
                    s_cur  = 0;
                    repaint();
                    return;
                default:                             /* exit */
                    menu_view_set_armed(false);
                    input_router_home();
                    return;
            }
            return;

        case PG_TZ: {
            const int n = orb_net_tz_count();
            if (s_cur >= n) { go_back(); repaint(); return; }   /* <- back */
            /* Applied on the worker: net_tz_set() writes NVS then tzset()s, and
             * the rule in this file is that nothing which can block runs on the
             * task that paints the dial. The result line arrives via
             * menu_view_service(). */
            job_post(JOB_TZ, NULL, s_cur);
            repaint();                                  /* shows "working..." */
            return;
        }

        case PG_THEME: {
            if (s_cur >= s_ui.n) { go_back(); repaint(); return; }   /* <- back */
            const orb_theme_entry_t *e = &s_ui.e[s_cur];
            if (e->err != ESP_OK) {
                snprintf(s_ui.status, sizeof(s_ui.status), "\"%s\": %s", e->slug,
                         e->why[0] ? e->why : "not loadable");
                repaint();
                return;
            }
            if (e->active) {
                snprintf(s_ui.status, sizeof(s_ui.status),
                         "\"%s\" is already on the dial", e->slug);
                repaint();
                return;
            }
            /* Verified on the worker: this streams the whole container through
             * CRC, measured at 2952 ms for 3.87 MB on the bench. */
            job_post(JOB_STAGE, e->slug, 0);
            repaint();                                  /* shows "working..." */
            return;
        }

        case PG_DISPLAY: {
            if (s_cur >= 2) { go_back(); repaint(); return; }        /* <- back */
            if (s_cur == 0) {
                /* The toggle runs on the worker: the setter nvs_commits, and the
                 * rule in this file is that nothing which can block runs on the
                 * task that paints the dial (same reason as JOB_TZ). The row
                 * repaints from the getter as soon as the result lands. */
                job_post(JOB_DIM_EN, NULL, 0);
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
            job_post(JOB_DIM_TO, NULL, s_cur);
            repaint();                                  /* shows "working..." */
            return;
        }

        case PG_ASK: {
            if (s_cur == 0) {                            /* reboot now */
                snprintf(s_ui.status, sizeof(s_ui.status), "rebooting...");
                repaint();
                static esp_timer_handle_t rt;
                if (!rt) {
                    const esp_timer_create_args_t ta = { .callback = reboot_fire };
                    if (esp_timer_create(&ta, &rt) != ESP_OK) {
                        snprintf(s_ui.status, sizeof(s_ui.status), "reboot timer failed");
                        repaint();
                        return;
                    }
                }
                if (esp_timer_start_once(rt, 750 * 1000) != ESP_OK) {
                    snprintf(s_ui.status, sizeof(s_ui.status), "reboot refused");
                    repaint();
                }
                return;
            }
            /* "keep this dial" must un-stage: a pending slot installs itself at
             * the next reboot whoever triggers, so leaving it set would apply
             * this theme on some later boot the owner never asked for. */
            const esp_err_t us = orb_theme_unstage();
            snprintf(s_ui.status, sizeof(s_ui.status), "%s",
                     (us == ESP_OK) ? "kept current dial" : "left staged (nvs)");
            s_page = PG_THEME;
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
    if (s_busy) return;

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
    repaint();
}

void menu_view_service(void)
{
    if (!s_dirty) return;
    s_dirty = false;

    xSemaphoreTake(s_res_mx, portMAX_DELAY);
    s_ui = s_shared;
    xSemaphoreGive(s_res_mx);

    /* A finished stage moves to the reboot question; a finished scan merely
     * populates the list it was already showing as pending. */
    if (s_ui.staged) {
        s_page = PG_ASK;
        s_cur  = 0;
        ESP_LOGI(TAG, "staged \"%s\" from the card", s_ui.staged_slug);
    }
    repaint();
}


