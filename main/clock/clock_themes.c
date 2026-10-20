/**
 * clock_themes.c — the clock app's answer to the shell's options hooks (B2).
 *
 * What moved here from menu_view.c is the theme page's *knowledge*: that the
 * rows are the card's themes, that the one on the dial is marked, that a damaged
 * entry is shown with its reason instead of hidden, and that choosing one means
 * installing it and rebooting into it (ruling D11 — the stage-then-ask flow this
 * file replaced left the owner to press a second row on faith). What did not
 * move is the list's feel — the cursor, the clamp and the row window stayed in
 * the shell, so the theme page and the menu's own pages cannot drift apart on
 * how a detent behaves.
 *
 * The page nests one level deep (ruling D12): a plain row for the factory
 * default, one group header for the card, and the card's themes as its items.
 * Which of those rows the glass draws is the shell's decision and not this
 * file's — the count here is the whole page either way, so the group can be
 * closed without a single row of this file knowing, and an app cannot hide a
 * row from its owner by accident.
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

/* The page's own row numbers, in the order the shell will draw them. The two
 * fixed rows come first so the card list is always in the same place, and a
 * closed group leaves a page whose row 0 the owner can still press. */
#define ROW_DEFAULT 0   /* OPT_ROW:   the factory dial */
#define ROW_GROUP   1   /* OPT_GROUP: "From the card" */
#define ROW_ITEMS   2   /* the scanned themes, one OPT_ITEM each */

typedef struct {
    int               n;
    orb_theme_entry_t e[THEME_MAX];
    char              status[72];
} cui_t;

static cui_t   s_ui;        /* LVGL task only */
static cui_t   s_shared;    /* worker writes this under s_mx */
static SemaphoreHandle_t s_mx;
static volatile bool s_dirty;
static bool    s_armed;

static void publish(const cui_t *src)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_shared = *src;
    xSemaphoreGive(s_mx);
    s_dirty = true;                 /* published last: the reader trusts the copy */
}

/* Runs on the worker. Nothing in here touches a widget, and nothing in here may
 * start to: this is the task that holds the card while a directory the size of
 * the framebuffer is walked. Storing is NOT here — orb_theme_post_bundle()
 * owns that job, so the dial's picker and the browser's Store button queue the
 * same worker function and cannot disagree about what a pick does. */
static void job_run(void *arg)
{
    (void)arg;                        /* the scan takes no payload */
    cui_t out;
    memset(&out, 0, sizeof out);

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
    publish(&out);
}

/* The one job this file owns. A refused post is reported, not swallowed: a page
 * that sits at its old rows because the queue was busy is the "card ignored me"
 * report, and the shell's busy gate is the only reason it happens. */
void clock_themes_request_scan(void)
{
    if (!s_mx) return;          /* clock_themes_init() did not run */
    const esp_err_t e = orb_jobs_post(job_run, NULL, 0);
    if (e == ESP_ERR_INVALID_STATE)
        strlcpy(s_ui.status, "the card is already being read", sizeof s_ui.status);
    else if (e != ESP_OK)
        ESP_LOGW(TAG, "theme scan refused (%s)", esp_err_to_name(e));
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
}

/* Chosen by index, never by a cursor of this module's own: the shell owns the
 * highlight, the clamp and the window, and an app that tracked a second cursor
 * would be one missed repaint away from installing the theme nobody is on.
 *
 * A press here IS the confirmation (ruling D11). There is no second row to press
 * on faith in and no staged key waiting for a boot: the worker either verifies
 * the container, copies it and reboots the dial, or it puts the reason on the row
 * and the device stays exactly as it was. The queue answer is reported, not
 * swallowed — a press the firmware dropped has to say so on the glass (C8). */
void clock_themes_pick(int idx)
{
    if (s_ui.n <= 0 || idx < 0 || idx >= s_ui.n) return;
    const orb_theme_entry_t *e = &s_ui.e[idx];
    ESP_LOGI(TAG, "pick %d/%d: %s (%s)", idx, s_ui.n - 1,
             e->name[0] ? e->name : e->id, e->id);
    if (e->err != ESP_OK) {
        strlcpy(s_ui.status, e->why[0] ? e->why : "not loadable", sizeof s_ui.status);
        return;
    }
    if (e->active) {
        snprintf(s_ui.status, sizeof s_ui.status, "\"%s\" is already on the dial", e->name);
        return;
    }
    const esp_err_t q = orb_theme_post_bundle(e->id);
    if (q == ESP_ERR_INVALID_STATE)
        strlcpy(s_ui.status, "one theme is already being installed", sizeof s_ui.status);
    else if (q != ESP_OK)
        snprintf(s_ui.status, sizeof s_ui.status, "\"%s\" could not start: %s",
                 e->name, esp_err_to_name(q));
    else
        snprintf(s_ui.status, sizeof s_ui.status, "storing \"%s\" …", e->name);
}

/* The "Factory default" row: the promise that there is always a way back to the
 * dial drawn from slot A holds. Nothing here goes near the card — slot A is the
 * bank `idf.py flash` wrote at partition offset 0 (and, since §26 slice 3's
 * ping-pong, possibly the copy a store wrote over it), and choosing it is one NVS
 * write and a reboot. */
static void pick_default(void)
{
    const char *const lv = orb_theme_live();
    if (strcmp(lv, THEME_LIVE_DEFAULT) == 0 || strcmp(lv, THEME_LIVE_SLOT_A) == 0) {
        strlcpy(s_ui.status, "the factory dial is what is on the wall",
                sizeof s_ui.status);
        return;
    }
    ESP_LOGI(TAG, "pick: the factory default");
    const esp_err_t e = orb_theme_post_default();
    if (e == ESP_ERR_INVALID_STATE)
        strlcpy(s_ui.status, "a theme job is already running", sizeof s_ui.status);
    else if (e != ESP_OK)
        snprintf(s_ui.status, sizeof s_ui.status, "the default could not start: %s",
                 esp_err_to_name(e));
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
    /* The two fixed rows plus the card's themes. Nothing here ever counted the
     * reboot question any more — ruling D11 deleted it, because a pick installs
     * and reboots by itself and there is no second press to have faith in.
     * The items stay in the count while their group is CLOSED: the shell hides
     * them, and an app that dropped them from its own number would be a second
     * opinion about what is on the page, which is the one thing §13.4 forbids. */
    return ROW_ITEMS + s_ui.n;
}

/** Which of the three shapes a row has (ruling D12). One group, its items, and
 *  the plain row above them — the shape the shell validates and renders, and the
 *  reason the card's themes are ITEMs rather than five rows the owner has to
 *  scroll past to reach the factory default. */
orb_opt_kind_t clock_opt_kind(orb_app_t *app, int idx)
{
    (void)app;
    if (idx == ROW_DEFAULT) return OPT_ROW;
    if (idx == ROW_GROUP)   return OPT_GROUP;
    if (idx >= ROW_ITEMS && idx < ROW_ITEMS + s_ui.n) return OPT_ITEM;
    return OPT_ROW;                     /* out of range: the shell's bounds answer */
}

const char *clock_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    /* The default is a ROW and the card is a GROUP, in that order, always: the
     * owner who arrives with no card must still find the row that gets them back
     * to the dial the box came with, which is the promise ruling D13 exists to
     * keep. Its title is the compiled pair's, because until D10's .orbapp wiring
     * ships the factory dial's source is the flashed bank, not a file. */
    if (idx == ROW_DEFAULT) return "Factory default";
    if (idx == ROW_GROUP)   return "From the card";
    const int i = idx - ROW_ITEMS;
    if (i < 0 || i >= s_ui.n) return "";
    const orb_theme_entry_t *e = &s_ui.e[i];
    return e->name[0] ? e->name : e->id;
}

const char *clock_opt_value(orb_app_t *app, int idx)
{
    (void)app;
    static char v[48];                  /* one row's answer, repainted per poll */
    v[0] = '\0';
    /* Every string below is a read of state the theme layer already holds: the
     * choice it resolved at boot, what its last install attempt could not do, and
     * what boot found damaged in the slot. None of them touches the card, NVS or
     * the network, which is the rule these answers live under — the same callback
     * is the dial's animation. */
    if (idx == ROW_DEFAULT) {
        const char *const lv = orb_theme_live();
        return (strcmp(lv, THEME_LIVE_DEFAULT) == 0 ||
                strcmp(lv, THEME_LIVE_SLOT_A) == 0) ? "on the dial" : "";
    }
    if (idx == ROW_GROUP) {
        /* What is on the wall, named, so a closed group still answers the question
         * a knock on the door asks. The shell overwrites this with "open" while the
         * group is showing its items, which is why it is a summary and not a row. */
        if (s_ui.n <= 0) return "the card holds none";
        for (int i = 0; i < s_ui.n; i++) {
            if (!s_ui.e[i].active) continue;
            snprintf(v, sizeof v, "%d of %d: %.20s", i + 1, s_ui.n,
                     s_ui.e[i].name[0] ? s_ui.e[i].name : s_ui.e[i].id);
            return v;
        }
        /* Nothing active with a choice in force: the named slot did not load, and
         * the damage pair on h_sd says why. "default" with nothing active is not
         * damage — it is a board whose slot A holds no bundle and no card was ever
         * picked. */
        return strcmp(orb_theme_live(), THEME_LIVE_DEFAULT) != 0
                   ? "the stored copy did not load" : "none chosen";
    }
    const int i = idx - ROW_ITEMS;
    if (i < 0 || i >= s_ui.n) return "";
    const orb_theme_entry_t *e = &s_ui.e[i];
    /* One value per row, chosen by what the owner has to decide, and one rule: the
     * reason a theme is not on the wall goes on that theme's row, because the row
     * is the only thing on this screen the owner was looking at when they asked.
     *   - a file the card holds but cannot load: the list's own reason
     *   - the slot boot was asked to draw and could not: boot's sentence
     *   - the install that was just refused: the install ladder's sentence
     * The last two name one theme each, by id, so the page says which theme. */
    if (e->err != ESP_OK) return e->why[0] ? e->why : "not loadable";
    if (s_ui.n > 0 && orb_theme_damage_id()[0] &&
        strcmp(e->id, orb_theme_damage_id()) == 0)
        return orb_theme_damage_why();
    if (orb_theme_try_id()[0] && strcmp(e->id, orb_theme_try_id()) == 0 &&
        orb_theme_try_why()[0])
        return orb_theme_try_why();
    if (e->active) return "on the dial";
    /* §6's other half on the glass: the name matches the wall's and the bytes do
     * not, so this row is the one that will actually change the dial. Without the
     * hint an overwrite leaves two rows that both say nothing, and pressing the
     * stale one is the silent no-op ruling D11 exists to prevent. */
    if (e->changed) return "card copy changed since install";
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
    /* A press on the GROUP is the shell's to act on (it opens and closes), and it
     * reaches here only if the shell forwards it: doing nothing is the right answer
     * either way, because the clock has no opinion about whether its own header is
     * showing its rows. */
    if (idx == ROW_DEFAULT) { pick_default(); return; }
    if (idx == ROW_GROUP)   return;
    clock_themes_pick(idx - ROW_ITEMS);
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
