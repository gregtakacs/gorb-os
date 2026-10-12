/**
 * clock_themes.c — the clock app's answer to the shell's options hooks (B2).
 *
 * What moved here from menu_view.c is the theme page's *knowledge*: that the
 * rows are the card's themes, that the one on the dial is marked, that a damaged
 * entry is shown with its reason instead of hidden, and that choosing one
 * stages it and asks about the reboot. What did not move is the list's feel —
 * the cursor, the clamp and the row window stayed in the shell, so the theme
 * page and the menu's own pages cannot drift apart on how a detent behaves.
 *
 * The two halves of the state are deliberate. s_ui is read by the repaint and
 * touched only on the LVGL task. s_shared is written by the worker and copied
 * out under s_mx. s_dirty is the handshake, set last by the writer because the
 * reader trusts the copy: publishing the flag before the data would let a poll
 * repaint a half-copied list.
 */

#include "clock_themes.h"
#include "orb_jobs.h"
#include "orb_theme.h"
#include "orb_sd.h"        /* orb_sd_mounted(), for the reason line only */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "clock_theme";

#define THEME_MAX 8     /* a card realistically holds a handful of themes */

typedef struct {
    int               n;
    orb_theme_entry_t e[THEME_MAX];
    bool              staged;
    char              staged_slug[ORB_THEME_SLUG_MAX + 1];
    char              status[72];
} cui_t;

static cui_t   s_ui;        /* LVGL task only */
static cui_t   s_shared;    /* worker writes this under s_mx */
static SemaphoreHandle_t s_mx;
static volatile bool s_dirty;
static bool    s_armed;

typedef enum { J_SCAN = 0, J_STAGE } job_t;
typedef struct { job_t kind; char slug[ORB_THEME_SLUG_MAX + 1]; } req_t;

static void publish(const cui_t *src)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_shared = *src;
    xSemaphoreGive(s_mx);
    s_dirty = true;                 /* published last: the reader trusts the copy */
}

/* Runs on the worker. Nothing in here touches a widget, and nothing in here may
 * start to: this is the task that holds the card while a directory the size of
 * the framebuffer is walked. */
static void job_run(void *arg)
{
    const req_t *r = arg;
    cui_t out;
    memset(&out, 0, sizeof out);

    if (r->kind == J_SCAN) {
        if (orb_theme_upload_busy()) {
            snprintf(out.status, sizeof out.status, "an upload is using the card");
        } else if (!orb_sd_mounted()) {
            snprintf(out.status, sizeof out.status, "no card in the socket");
        } else {
            const int n = orb_theme_scan(out.e, THEME_MAX);
            if (n < 0) {
                snprintf(out.status, sizeof out.status, "the card could not be read");
            } else {
                out.n = n;
                if (n == 0) snprintf(out.status, sizeof out.status, "no themes on the card yet");
            }
        }
    } else {
        /* Streams the whole container through CRC before anything is committed,
         * so a damaged file answers here instead of costing a reboot into a slot
         * that will not validate. */
        const esp_err_t er = orb_theme_stage_load(r->slug);
        if (er != ESP_OK)
            snprintf(out.status, sizeof out.status, "\"%s\" refused: %s",
                     r->slug, esp_err_to_name(er));
        else {
            out.staged = true;
            snprintf(out.staged_slug, sizeof out.staged_slug, "%s", r->slug);
        }
    }
    publish(&out);
}

static void post(req_t *r)
{
    const esp_err_t e = orb_jobs_post(job_run, r, sizeof *r);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "theme job refused (%s)", esp_err_to_name(e));
}

void clock_themes_request_scan(void)
{
    if (!s_mx) return;          /* clock_themes_init() did not run */
    req_t r = { .kind = J_SCAN };
    post(&r);
}

bool clock_themes_busy(void)
{
    return orb_jobs_busy();
}

const char *clock_themes_status(void)
{
    return s_ui.status;
}

void clock_themes_reset(void)
{
    /* Nothing to carry: the rows are re-walked on entry, so there is no mid-list
     * position to restore and no stale row to point at. */
}

void clock_themes_service(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_ui = s_shared;
    xSemaphoreGive(s_mx);

    if (s_ui.staged) {
        /* The reboot question is the clock's to ask because it is the clock's
         * theme that is waiting; the reboot itself stays the OS's to perform. */
        ESP_LOGI(TAG, "staged \"%s\" from the card", s_ui.staged_slug);
    }
}

/* Chosen by index, never by a cursor of this module's own: the shell owns the
 * highlight, the clamp and the window, and an app that tracked a second cursor
 * would be one missed repaint away from staging the row nobody is on. */
void clock_themes_pick(int idx)
{
    if (s_ui.n <= 0 || idx < 0 || idx >= s_ui.n) return;
    const orb_theme_entry_t *e = &s_ui.e[idx];
    ESP_LOGI(TAG, "pick %d/%d: %s", idx, s_ui.n - 1,
             e->title[0] ? e->title : e->slug);
    if (e->err != ESP_OK) {
        strlcpy(s_ui.status, e->why[0] ? e->why : "not loadable", sizeof s_ui.status);
        return;
    }
    if (e->active) {
        snprintf(s_ui.status, sizeof s_ui.status, "\"%s\" is already on the dial", e->slug);
        return;
    }
    req_t r = { .kind = J_STAGE };
    snprintf(r.slug, sizeof r.slug, "%s", e->slug);
    post(&r);
}

/* ---- the ABI the shell calls (B1/B2, ruling B2) ----------------------------
 *
 * These are the answers, not the page. The shell draws the rows, owns the
 * cursor and decides what a detent does to it; what the rows say and what a
 * press means is decided here, which is the whole of "the OS renders, the app
 * decides".
 *
 * The rows are the card's themes, and the row count answers 0 rather than a
 * stale number while a walk is outstanding — a page that showed five rows from
 * a previous card would offer a press on a file that may no longer exist.
 */

esp_err_t clock_themes_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    return s_mx ? ESP_OK : ESP_ERR_NO_MEM;
}

int clock_opt_count(orb_app_t *app)
{
    (void)app;
    /* The reboot question, when a theme is waiting, is two more rows: install
     * now, or keep the dial that is up. They live here rather than in the shell
     * because only the clock knows that its theme is the thing pending. */
    return s_ui.n + (s_ui.staged ? 2 : 0);
}

const char *clock_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    if (idx < s_ui.n) {
        const orb_theme_entry_t *e = &s_ui.e[idx];
        return e->title[0] ? e->title : e->slug;
    }
    if (idx == s_ui.n)     return "reboot now";
    if (idx == s_ui.n + 1) return "keep this dial";
    return "";
}

const char *clock_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    if (idx >= s_ui.n) return "";
    const orb_theme_entry_t *e = &s_ui.e[idx];
    /* One value per row, chosen by what the owner has to decide. A damaged
     * theme shows why rather than being dropped: "no themes on the card" and
     * "three themes, all damaged" are the card's problem and the bake's
     * problem, and hiding the entries is what makes that undiagnosable. */
    if (e->err != ESP_OK)      return e->why[0] ? e->why : "not loadable";
    if (e->active)             return "on the dial";
    if (e->pending)            return "waiting to install";
    return "";
}

void clock_opt_inc(orb_app_t *app, int idx, int32_t steps)
{
    (void)app;
    /* Rows here are chosen, not spun: there is no value to increment, so the
     * shell moves the highlight and the press does the work. A theme page with
     * a spinner row would be a different shape and would say so here. */
    (void)idx; (void)steps;
}

void clock_opt_sel(orb_app_t *app, int idx)
{
    (void)app;
    if (idx < s_ui.n) { clock_themes_pick(idx); return; }
    if (!s_ui.staged) return;
    if (idx == s_ui.n) {
        /* Ask; never call. esp_restart() from an app would be an app owning the
         * device's power state, and the reboot belongs to the OS — which also
         * owns arming the delay that lets the last frame reach the panel. */
        orb_jobs_reboot_soon("install the staged theme");
        return;
    }
    /* "keep this dial" must un-stage: a pending slot installs itself at the next
     * reboot whoever triggers, so leaving it set would apply this theme on some
     * later boot the owner never asked for. */
    const esp_err_t us = orb_theme_unstage();
    snprintf(s_ui.status, sizeof s_ui.status, "%s",
             (us == ESP_OK) ? "kept current dial" : "left staged (nvs refused)");
    s_ui.staged = false;
}

void clock_opt_arm(orb_app_t *app, bool armed)
{
    (void)app;
    s_armed = armed;
    if (armed) {
        /* Entering the page is when the card is worth walking: the rows are the
         * card's contents, and contents change between visits. */
        clock_themes_request_scan();
    } else {
        clock_themes_reset();
    }
}

bool clock_opt_armed(void)
{
    return s_armed;
}
