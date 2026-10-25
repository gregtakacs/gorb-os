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
 * The page nests one level deep (ruling D12): one group header for the card, and
 * the card's themes as its items. Migration 1 dropped the plain factory-default
 * row that used to sit above the group: under the single slot the base face is
 * what an empty or torn slot already draws, and there is no second copy a press
 * could store a default into. Which rows the glass draws is still the shell's
 * decision and not this file's — the count here is the whole page either way, so
 * the group can be closed without a single row of this file knowing, and an app
 * cannot hide a row from its owner by accident.
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
#include "orb_app.h"       /* D35: this app's own ledger row */
#include "orb_svc.h"       /* orb_grant_ok — the caps this manifest declares */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"     /* the preview's settle timer */
#include "esp_heap_caps.h" /* the plane's PSRAM buffer */

static const char *TAG = "clock_theme";

#define THEME_MAX 8     /* a card realistically holds a handful of themes */

/* The page's own row numbers, in the order the shell will draw them. The group
 * header comes first so the card list is always in the same place, and a closed
 * group leaves a page whose row 0 the owner can still press. Migration 1 retired
 * the plain "Factory default" row: with one slot the base face is what an empty or
 * torn slot already draws, and there is no second copy to store a default into. */
#define ROW_GROUP   0   /* OPT_GROUP: "From the card" */
#define ROW_ITEMS   1   /* the scanned themes, one OPT_ITEM each */

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

/* ---- the preview plane (docs/theme-preview-plan.md §2) --------------------
 *
 * The picker draws one theme's `preview` plane when the cursor lands on its
 * row, and the card walk that reads it cannot run on the LVGL task: holding
 * the card there freezes the dial for as long as the mutex is held elsewhere.
 * So this is the same worker/service split the row list already uses, one
 * plane deep — and the same discipline: nothing that touches a widget runs on
 * the worker, and nothing that blocks runs on the LVGL task.
 *
 * s_pvx is the worker's answer (written under s_mx); s_pvu is the copy the
 * paint and the opt_preview answer read (LVGL task only). The plane buffer is
 * allocated transiently per read on the worker and freed on the LVGL task at
 * the reap — a buffer the shell may still be blitting is never freed from
 * under it, so a superseded read's buffer is freed only once its answer is no
 * longer the one being displayed. An absent preview is an empty answer
 * (ORB_ASSET_NO_CONTENT), never an error: the row stays text-only.
 */
#define PV_SETTLE_MS 120     /* the cursor rests this long before a read starts */

typedef struct {
    uint8_t  *px;                     /* RGB565 plane, w*h*2 bytes; NULL = none */
    uint16_t  w, h;                   /* its extent                             */
    char      id[ORB_THEME_HANDLE_MAX + 1];  /* the theme it was read for — the
                                              * whole card stem, the same width
                                              * orb_theme_entry_t.id carries    */
    uint32_t  gen;                    /* bumped per publish, incl. empty ones   */
    esp_err_t e;                      /* ESP_OK, or why the read refused        */
    char      why[72];                /* the reader's sentence when e != ESP_OK */
} pv_t;

static pv_t    s_pvx;                 /* worker writes under s_mx              */
static pv_t    s_pvu;                 /* LVGL task only: what paint answers    */
static volatile bool s_pv_dirty;      /* an answer is waiting to be reaped     */
static int     s_pv_row = -1;         /* the app row the cursor rests on       */
static int64_t s_pv_since;            /* us at the last row change             */
static uint32_t s_pv_gen;             /* publish counter, worker-owned         */

static void pv_publish(const pv_t *src)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_pvx = *src;
    xSemaphoreGive(s_mx);
    s_pv_dirty = true;                /* published last, as publish() does */
}

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

/* Runs on the worker. arg is the theme id the cursor rested on, copied by the
 * queue. The read is transient: the shape probe (cap 0 — a plane that exists
 * answers ROOM with its extents, the header's documented probe idiom) sizes
 * the buffer to the plane's actual bytes, the class's square 2 B/px shape (a
 * preview for the 466 panel is 232x232, 107,648 B), and the buffer is
 * published for the LVGL task to free. NO_SLOT and NO_CONTENT are the
 * absent-preview answers the design makes quiet (the row stays text-only), not
 * errors, so they publish an empty; the caller that shows status reads e for
 * the rest. A post that raced a card pull answers NO_SLOT from the reader's
 * own door and is reported the same quiet way. */
static void pv_job_run(void *arg)
{
    const char *id = (const char *)arg;
    pv_t out;
    memset(&out, 0, sizeof out);
    strlcpy(out.id, id, sizeof out.id);
    out.gen = ++s_pv_gen;                 /* worker-owned counter, one writer */

    if (!orb_sd_mounted() || orb_theme_upload_busy()) {
        pv_publish(&out);                 /* empty, quiet: no card, no plane */
        return;
    }

    uint16_t w = 0, h = 0;
    char why[72] = "";
    orb_asset_ans_t a = orb_theme_preview_read(id, NULL, 0, &w, &h,
                                               why, sizeof why);
    if (a == ORB_ASSET_ROOM && w && h) {  /* a plane exists; this is its size */
        const uint32_t bytes = (uint32_t)w * (uint32_t)h * 2u;
        uint8_t *px = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!px) {
            out.e = ESP_ERR_NO_MEM;
        } else {
            a = orb_theme_preview_read(id, px, bytes, &w, &h, why, sizeof why);
            if (a == ORB_ASSET_OK) { out.px = px; out.w = w; out.h = h; }
            else heap_caps_free(px);      /* read refused after the alloc */
        }
    }
    /* NO_SLOT/NO_CONTENT stay e=ESP_OK with a NULL plane: the quiet no-preview
     * answer. Anything else the caller can name carries the reader's sentence,
     * and the reap puts it on the status line the page already owns. */
    if (a != ORB_ASSET_OK && a != ORB_ASSET_ROOM &&
        a != ORB_ASSET_NO_SLOT && a != ORB_ASSET_NO_CONTENT) {
        out.e = ESP_FAIL;
        strlcpy(out.why, why[0] ? why : "the preview could not be read",
                sizeof out.why);
        ESP_LOGW(TAG, "preview \"%s\": %s", id, why);
    }
    pv_publish(&out);
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
     * position to restore and no stale row to point at. The preview goes with
     * the rows — the leave path drops the plane, so re-entering answers "what
     * is on the card" again rather than holding a picture of the theme the
     * cursor last sat on. */
    clock_themes_drop_preview();
}

/* Drop the displayed plane and any published-but-unreaped answer. Runs on the
 * LVGL task only (the leave path, and service()'s discard): the buffer it
 * frees may be the one the shell blitted on the last repaint, and the worker
 * never paints, so the worker never frees. */
void clock_themes_drop_preview(void)
{
    if (s_pvu.px) heap_caps_free(s_pvu.px);
    memset(&s_pvu, 0, sizeof s_pvu);
    s_pv_row = -1;
    s_pv_since = 0;
}

void clock_themes_service(void)
{
    if (s_dirty) {
        s_dirty = false;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        s_ui = s_shared;
        xSemaphoreGive(s_mx);
    }

    /* A cursor that moved while a read was in flight invalidates that read: the
     * row the answer names is no longer the one on the glass. The reaper
     * compares the answer's id against the row the cursor rests on and frees a
     * plane that arrived for the wrong one — so a fast spin past a theme costs
     * one wasted card read and never one wrong picture. */
    if (s_pv_dirty) {
        s_pv_dirty = false;
        pv_t next;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        next = s_pvx;
        memset(&s_pvx, 0, sizeof s_pvx);      /* ownership moves with the copy */
        xSemaphoreGive(s_mx);

        const int i = (s_pv_row >= ROW_ITEMS && s_pv_row < ROW_ITEMS + s_ui.n)
                    ? s_pv_row - ROW_ITEMS : -1;
        const bool mine = i >= 0 && next.id[0] &&
                          strcmp(next.id, s_ui.e[i].id) == 0;
        if (!mine) {
            if (next.px) heap_caps_free(next.px);
            if (s_pvu.px) heap_caps_free(s_pvu.px);
            memset(&s_pvu, 0, sizeof s_pvu);
            if (next.gen >= s_pvu.gen) s_pvu.gen = next.gen;   /* paint still keys off it */
        } else {
            if (s_pvu.px) heap_caps_free(s_pvu.px);
            s_pvu = next;
            if (s_pvu.e != ESP_OK && s_pvu.why[0])
                strlcpy(s_ui.status, s_pvu.why, sizeof s_ui.status);
        }
    }

    /* The settle: post one read once the cursor has rested on a theme row for
     * PV_SETTLE_MS with the worker free. The rest rule is the whole reason a
     * land queues a read at all — a read per detent through a 62-row spin is
     * ~108 KB of card work per stop and the knob never answers; a read after
     * the spin costs the one theme the owner stopped on. */
    if (s_pv_row >= ROW_ITEMS && s_pv_row < ROW_ITEMS + s_ui.n && s_pv_since &&
        !orb_jobs_busy() &&
        esp_timer_get_time() / 1000 - s_pv_since / 1000 >= PV_SETTLE_MS) {
        const int i = s_pv_row - ROW_ITEMS;
        if (!s_pvu.px || strcmp(s_pvu.id, s_ui.e[i].id) != 0) {
            s_pv_since = 0;                   /* one post per land, settled */
            const esp_err_t e = orb_jobs_post(pv_job_run, s_ui.e[i].id,
                                              strlen(s_ui.e[i].id) + 1);
            if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
                ESP_LOGW(TAG, "preview post refused (%s)", esp_err_to_name(e));
        }
    }
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
    /* The one group header plus the card's themes. Nothing here ever counted the
     * reboot question any more — ruling D11 deleted it, because a pick installs
     * and reboots by itself and there is no second press to have faith in. Migration
     * 1 dropped the factory-default row with the A/B slot it pointed at. The items
     * stay in the count while their group is CLOSED: the shell hides them, and an
     * app that dropped them from its own number would be a second opinion about what
     * is on the page, which is the one thing §13.4 forbids. */
    return ROW_ITEMS + s_ui.n;
}

/** Which of the two shapes a row has (ruling D12). One group and its items — the
 *  shape the shell validates and renders, and the reason the card's themes are
 *  ITEMs rather than loose rows. Migration 1 dropped the plain factory-default row
 *  that used to lead the page. */
orb_opt_kind_t clock_opt_kind(orb_app_t *app, int idx)
{
    (void)app;
    if (idx == ROW_GROUP)   return OPT_GROUP;
    if (idx >= ROW_ITEMS && idx < ROW_ITEMS + s_ui.n) return OPT_ITEM;
    return OPT_ROW;                     /* out of range: the shell's bounds answer */
}

const char *clock_opt_label(orb_app_t *app, int idx)
{
    (void)app;
    /* The card is a GROUP, at row 0, always. Migration 1 retired the factory-default
     * ROW that used to lead the page: under the single slot there is no press that
     * returns the dial to a second copy, so the group is the whole page and the
     * base face needs no row to name it — it is what a slot with nothing valid in
     * it already draws. */
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
     * source boot resolved at boot, what its last install attempt could not do, and
     * the witness a store was armed and torn. None of them touches the card, NVS or
     * the network, which is the rule these answers live under — the same callback
     * is the dial's animation. */
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
        /* Nothing active with themes on the card: the slot holds no valid bundle, so
         * the dial is on the base face. The staged witness on h_sd says when that is a
         * torn store rather than a board that has never stored a theme. */
        return strcmp(orb_theme_source(), "slot") == 0
                   ? "the stored copy did not load" : "on the base face";
    }
    const int i = idx - ROW_ITEMS;
    if (i < 0 || i >= s_ui.n) return "";
    const orb_theme_entry_t *e = &s_ui.e[i];
    /* One value per row, chosen by what the owner has to decide, and one rule: the
     * reason a theme is not on the wall goes on that theme's row, because the row
     * is the only thing on this screen the owner was looking at when they asked.
     *   - a file the card holds but cannot load: the list's own reason
     *   - the store that was armed and never committed: the staged witness
     *   - the install that was just refused: the install ladder's sentence
     * The last two name one theme each, by id, so the page says which theme. */
    if (e->err != ESP_OK) return e->why[0] ? e->why : "not loadable";
    if (s_ui.n > 0 && orb_theme_staged_id()[0] &&
        strcmp(e->id, orb_theme_staged_id()) == 0 && orb_theme_staged_why()[0])
        return orb_theme_staged_why();
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
    /* D35/D19: a theme pick is the clock asking for the D11 install, and the
     * clock's manifest DECLARES themes — so the declaration is consulted here,
     * not skipped because the module predates its own row. A revoked cap
     * degrades the page visibly: the refusal is status text on the group the
     * owner just pressed, and the install never reaches the worker. Migration 1
     * left the group header as the only non-item row, so an ITEM press is the
     * only one that needs the themes cap. */
    if (idx >= ROW_ITEMS && !orb_grant_ok(app, CAP_THEMES)) {
        strlcpy(s_ui.status, "theme permission revoked", sizeof s_ui.status);
        return;
    }
    if (idx == ROW_GROUP)   return;
    clock_themes_pick(idx - ROW_ITEMS);
}

void clock_opt_arm(orb_app_t *app, bool armed)
{
    s_armed = armed;
    if (armed) {
        /* Entering the page is when the card is worth walking: the rows are the
         * card's contents, and contents change between visits. D35/D19: the walk
         * reads the card, the manifest declares sd_read, and the ledger counts
         * the refusal the same way it counts a guest's — denied is a state the
         * page shows, not an error the app hides. */
        if (orb_grant_ok(app, CAP_SD_READ)) clock_themes_request_scan();
        else strlcpy(s_ui.status, "card permission revoked", sizeof s_ui.status);
    } else {
        clock_themes_reset();
    }
}

void clock_opt_focus(orb_app_t *app, int idx)
{
    /* The preview read is a card read: the manifest declares sd_read and the
     * gate says whether it still runs, exactly as clock_opt_arm's walk does.
     * A revoked cap makes the focus a no-op — the revoke is already on the
     * status line from the arm, and a row with no preview is the same page
     * every theme without one draws. */
    if (!orb_grant_ok(app, CAP_SD_READ)) return;
    /* The shell tells the app where its cursor landed, in the app's own row
     * numbers. A land on the group header (row 0) or past the walk's end shows
     * nothing; the plane drops so the list the owner can actually see is the
     * list on the glass. A land on the same row is not a new land: the settle
     * only re-arms when the row changes. */
    if (idx == s_pv_row) return;
    s_pv_row = idx;
    s_pv_since = idx >= ROW_ITEMS ? esp_timer_get_time() : 0;
    if (idx < ROW_ITEMS || idx >= ROW_ITEMS + s_ui.n) {
        if (s_pvu.px) heap_caps_free(s_pvu.px);
        memset(&s_pvu, 0, sizeof s_pvu);
    }
}

const void *clock_opt_preview(orb_app_t *app, int idx, uint16_t *w,
                              uint16_t *h, uint32_t *stamp)
{
    (void)app;
    /* The answer half, not the ask: it reads only the reaped plane (s_pvu), the
     * same rule opt_label and opt_value live under — the callback that answers
     * a row is the dial's animation, and a card read here would hold the task
     * that paints it. NULL is the text-only answer: nothing reaped for this
     * row, a theme with no preview (NO_CONTENT, never an error), a page whose
     * cursor is not on an item, or a group that is closed. */
    if (!s_pvu.px || idx != s_pv_row) return NULL;
    if (idx < ROW_ITEMS || idx >= ROW_ITEMS + s_ui.n) return NULL;
    /* §6's identity rule applies to a picture too: the plane answers the row
     * that carries the same id it was read for. A card re-walk that moved the
     * theme out from under the cursor answers text, not the old face. */
    if (strcmp(s_ui.e[idx - ROW_ITEMS].id, s_pvu.id) != 0) return NULL;
    *w = s_pvu.w;
    *h = s_pvu.h;
    *stamp = s_pvu.gen;
    return s_pvu.px;
}

bool clock_opt_armed(void)
{
    return s_armed;
}
