/**
 * themes_page.c — the dial's theme rows: each app's Themes sub-menu.
 *
 * §58 (2026-11-07): the owner wanted everything about an app in one place, so this is
 * no longer a top menu row: it is each app's Themes sub-menu. The menu aims it at one
 * app (themes_page_for) and opens this row's page; the card's listing is walked whole,
 * and the rows are the themes whose stem names that app (e->app). The
 * history below is the page's, from when it was a row, and the clock's before that.
 *
 * §43 moved it here from the clock app (it was main/stage/clock_themes.c, answering
 * the clock's options hooks under B2). A theme covers every app at once (D21's
 * capacity note), so choosing one is the OS's act, not one app's; and the clock is a
 * wasm module now, which reaches the OS through orb.* and could not own a page that
 * walks the card and installs into the assets partition. The page is the owner on
 * the dial's own menu, so the clock's sd_read and themes grants no longer gate it.
 * The history below is the page's, from when it was the clock's.
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

#include "themes_page.h"
#include "orb_jobs.h"
#include "orb_theme.h"
#include "orb_sd.h"        /* orb_sd_mounted(), for the reason line only */
#include "orb_app.h"       /* D35: this app's own ledger row */
#include "orb_svc.h"
#include "display.h"      /* §60.2: display_dma_parked, the prefetch's pacing */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"      /* §60.2: the preview work's own task */
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"     /* the preview's settle timer */
#include "esp_heap_caps.h" /* the plane's PSRAM buffer */
#include "esp_attr.h"      /* EXT_RAM_BSS_ATTR: the list copies live in PSRAM */

static const char *TAG = "clock_theme";

#define THEME_MAX ORB_THEME_LIST_MAX   /* the one listing ceiling, orb_theme.h */

/* The page's own row numbers, in the order the shell will draw them. The group
 * header comes first so the card list is always in the same place, and a closed
 * group leaves a page whose row 0 the owner can still press. Migration 1 retired
 * the plain "Factory default" row: with one slot the base face is what an empty or
 * torn slot already draws, and there is no second copy to store a default into. */
#define ROW_GROUP   0   /* OPT_GROUP: "Themes" */
#define ROW_ITEMS   1   /* the scanned themes, one OPT_ITEM each */

typedef struct {
    int               n;
    orb_theme_entry_t e[THEME_MAX];
    char              status[72];
} cui_t;

/* PSRAM: at the listing ceiling one copy is ~30 kB, and there are three. */
EXT_RAM_BSS_ATTR static cui_t s_ui;      /* LVGL task only */
EXT_RAM_BSS_ATTR static cui_t s_shared;  /* worker writes this under s_mx */
static SemaphoreHandle_t s_mx;
static volatile bool s_dirty;
static volatile bool s_walk;      /* a list walk is posted and unreaped     */
static bool    s_armed;
/* §58: the app the rows are for, by slug (LVGL task only). The menu sets it with
 * themes_page_for() before it opens this row's page. */
static char    s_for[ORB_THEME_APP_MAX + 1];

void themes_page_for(const char *slug)
{
    strlcpy(s_for, slug ? slug : "", sizeof s_for);
}
/* The hooks are asked on behalf of this provider row; any other caller names its
 * own app, and that wins. */
static void for_app(const orb_app_t *app)
{
    if (app && app != &orb_app_themes_row && app->slug) themes_page_for(app->slug);
}
static int count_for(const char *slug)
{
    int n = 0;
    for (int k = 0; k < s_ui.n; k++) if (!strcmp(s_ui.e[k].app, slug)) n++;
    return n;
}
/** How many of the listing's entries are this app's. */
static int mine_n(void) { return count_for(s_for); }

int themes_page_count_for(const char *slug) { return slug ? count_for(slug) : 0; }

const char *themes_page_active_for(const char *slug)
{
    for (int k = 0; slug && k < s_ui.n; k++)
        if (!strcmp(s_ui.e[k].app, slug) && s_ui.e[k].active)
            return s_ui.e[k].name[0] ? s_ui.e[k].name : s_ui.e[k].id;
    return NULL;
}
bool themes_page_item(const char *slug, int i, const char **id, const char **name,
                      bool *active, bool *ok)
{
    for (int k = 0; slug && k < s_ui.n; k++) {
        if (strcmp(s_ui.e[k].app, slug) != 0 || i-- != 0) continue;
        const orb_theme_entry_t *e = &s_ui.e[k];
        if (id) *id = e->id;
        if (name) *name = e->name[0] ? e->name : e->id;
        if (active) *active = e->active;
        if (ok) *ok = e->err == ESP_OK;
        return true;
    }
    return false;
}

/** The listing index of this app's i-th theme, or -1. */
static int mine(int i)
{
    if (i < 0) return -1;
    for (int k = 0; k < s_ui.n; k++)
        if (!strcmp(s_ui.e[k].app, s_for) && i-- == 0) return k;
    return -1;
}

/* ---- the preview plane (docs/history/theme-preview-plan.md §2) --------------------
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
#define PV_SETTLE_CACHED_MS 40   /* §60.3: or this long when the bytes are already in PSRAM */

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

/* ---- §60.2: the compressed-preview cache ------------------------------------------
 * Owner, 2026-11-07: the page should load ahead and let the owner rotate through themes
 * without each one stopping to load. A card read lends the display's DMA ring for its whole
 * length (orb_sd_lock), so the page freezes while one runs; inflating bytes already in PSRAM
 * does not touch the card. So while the cursor rests, the worker fetches the stored previews
 * of the themes nearest it (here, then +1, -1, +2, -2), one at a time, into this cache; a
 * landing on a cached theme inflates from memory, never the card; and a turn never starts a
 * card read. Clock themes run ~150-225 KB each and twenty of them would not fit in PSRAM, so
 * the cache is bounded and evicts the entry farthest from the cursor. LVGL task only: the
 * worker gets a pointer at post time, and nothing is evicted while a job is outstanding. */
#define ZC_MAX       8
#define ZC_BUDGET    (1100u * 1024u)
#define ZC_REST_MS   250       /* the cursor rests this long before a fetch starts */
typedef struct {
    char      id[ORB_THEME_HANDLE_MAX + 1];
    uint8_t  *z;
    uint32_t  n;
    uint16_t  side;
} zc_t;
static zc_t     s_zc[ZC_MAX];
static struct { char id[ORB_THEME_HANDLE_MAX + 1]; uint8_t *z; uint32_t n; uint16_t side; } s_zx;
static volatile bool s_z_dirty;        /* a fetch's answer is waiting in s_zx    */
static bool     s_z_fly;               /* a fetch is posted and unreaped         */
static int64_t  s_moved_us;            /* the cursor's last move                 */
static bool     s_zc_drop;             /* the page left: empty the cache once free */
static int64_t  s_z_done_us;           /* when the last fetch was filed           */
static bool     s_zc_full;             /* §60.3: nothing nearer fits; rest until a move */
static int8_t   s_dir = -1;            /* §60.3: the way the last move went (+1 down the list) */
#define ZC_GAP_MS    150       /* the glass draws this long between two fetches */

static const zc_t *zc_find(const char *id)
{
    for (int i = 0; i < ZC_MAX; i++) if (s_zc[i].z && !strcmp(s_zc[i].id, id)) return &s_zc[i];
    return NULL;
}

/* §60.3: how many of this app's themes lie between a cached id and the cursor (far, for an id
 * the list no longer has). */
static int zc_dist(const char *id, int cur)
{
    for (int k = 0, m = 0; k < s_ui.n; k++) {
        if (strcmp(s_ui.e[k].app, s_for) != 0) continue;
        if (!strcmp(s_ui.e[k].id, id)) return m > cur ? m - cur : cur - m;
        m++;
    }
    return 1 << 20;
}
static uint32_t zc_bytes(void)
{
    uint32_t t = 0;
    for (int i = 0; i < ZC_MAX; i++) if (s_zc[i].z) t += s_zc[i].n;
    return t;
}
static void zc_drop_all(void)
{
    for (int i = 0; i < ZC_MAX; i++) { if (s_zc[i].z) heap_caps_free(s_zc[i].z); memset(&s_zc[i], 0, sizeof s_zc[i]); }
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
static void job_run(void *arg);
void themes_page_walk(void) { job_run(NULL); }

static void job_run(void *arg)
{
    (void)arg;                        /* the scan takes no payload */
    /* Static, not a local: a full listing is far past the worker's stack, and
     * only the worker runs this, so one scratch copy is never shared. */
    EXT_RAM_BSS_ATTR static cui_t out;
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
 * queue. The read is transient: the shape probe (NULL buf — a plane that exists
 * answers OK with its extents and copies nothing, the header's documented probe
 * idiom) sizes the buffer to the plane's actual bytes, the class's square 2 B/px
 * shape (a preview for the 466 panel is 232x232, 107,648 B), and the buffer is
 * published for the LVGL task to free. NO_SLOT and NO_CONTENT are the
 * absent-preview answers the design makes quiet (the row stays text-only), not
 * errors, so they publish an empty; the caller that shows status reads e for
 * the rest. A post that raced a card pull answers NO_SLOT from the reader's
 * own door and is reported the same quiet way. */
/* §60.2: what a display job is handed. From the cache: the stored bytes, inflated here with
 * no card at all. Not cached: the id alone, and the job reads the card as it always has. */
typedef struct {
    char            id[ORB_THEME_HANDLE_MAX + 1];
    const uint8_t  *z;
    uint32_t        n;
    uint16_t        side;
    uint32_t        moves;            /* s_moves at the post: a move since abandons the read */
} pv_ask_t;

/* §60.3: every cursor move counts here (LVGL task, one writer). A card read in flight asks
 * between segments whether the cursor is still where it was posted, and gives the card up
 * when it is not, so the theme the owner turned to starts its read without waiting out the
 * one they turned past. */
static volatile uint32_t s_moves;
static bool still_here(void *arg, uint32_t done, uint32_t total)
{
    (void)done; (void)total;
    return ((const pv_ask_t *)arg)->moves == s_moves;
}

/* §60.2: a prefetch, on the worker: one theme's stored preview into PSRAM, published for the
 * service to file in the cache. The card read is the cost the cache exists to pay ahead. */
static void zc_job_run(void *arg)
{
    pv_ask_t *ask = (pv_ask_t *)arg;
    const char *id = ask->id;
    char why[72] = "";
    uint32_t n = 0;
    uint16_t side = 0;
    uint8_t *z = (orb_sd_mounted() && !orb_theme_upload_busy())
               ? orb_theme_preview_zcopy_ex(id, &n, &side, why, sizeof why, still_here, ask) : NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    strlcpy(s_zx.id, id, sizeof s_zx.id);
    s_zx.z = z; s_zx.n = n; s_zx.side = side;
    xSemaphoreGive(s_mx);
    s_z_dirty = true;
}

static void pv_job_run(void *arg)
{
    const pv_ask_t *ask = (const pv_ask_t *)arg;
    const char *id = ask->id;
    pv_t out;
    memset(&out, 0, sizeof out);
    strlcpy(out.id, id, sizeof out.id);
    out.gen = ++s_pv_gen;                 /* worker-owned counter, one writer */

    if (ask->z) {                         /* §60.2: cached — memory only, no card */
        out.px = orb_preview_inflate(ask->z, ask->n, ask->side);
        if (out.px) { out.w = out.h = ask->side; }
        else { out.e = ESP_FAIL; strlcpy(out.why, "the cached preview did not inflate", sizeof out.why); }
        pv_publish(&out);
        return;
    }

    if (!orb_sd_mounted() || orb_theme_upload_busy()) {
        pv_publish(&out);                 /* empty, quiet: no card, no plane */
        return;
    }

    /* §60.3: a deflated preview is fetched as its stored bytes (one short card hold) and
     * inflated here with the card free, and the bytes go to the cache as a prefetch's do,
     * so coming back to this theme costs no card at all. Only a raw preview (a theme baked
     * before v1.2.20) takes the plane read below, which inflates nothing. */
    {
        uint32_t zn = 0;
        uint16_t side = 0;
        char zwhy[72] = "";
        uint8_t *z = orb_theme_preview_zcopy_ex(id, &zn, &side, zwhy, sizeof zwhy,
                                                still_here, (void *)ask);
        if (!z && !still_here((void *)ask, 0, 0)) {    /* abandoned: the cursor moved on */
            pv_publish(&out);
            return;
        }
        if (z) {
            out.px = orb_preview_inflate(z, zn, side);
            if (out.px) { out.w = out.h = side; }
            else { out.e = ESP_FAIL; strlcpy(out.why, "the preview did not inflate", sizeof out.why); }
            bool filed = false;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            if (!s_zx.z) {                        /* else a fetch's answer is still unreaped */
                strlcpy(s_zx.id, id, sizeof s_zx.id);
                s_zx.z = z; s_zx.n = zn; s_zx.side = side;
                filed = true;
            }
            xSemaphoreGive(s_mx);
            if (filed) s_z_dirty = true;
            else heap_caps_free(z);
            pv_publish(&out);
            return;
        }
    }

    uint16_t w = 0, h = 0;
    char why[72] = "";
    /* The probe answers OK with the extent and copies nothing (NULL buf is the
     * shape probe orb_theme.h documents); ROOM is the over-full-buffer refusal,
     * which a probe cannot earn. A probe that found no row leaves w/h zero. */
    orb_asset_ans_t a = orb_theme_preview_read(id, NULL, 0, &w, &h,
                                               why, sizeof why);
    if (a == ORB_ASSET_OK && w && h) {   /* a plane exists; this is its size */
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
void themes_page_request_scan(void)
{
    if (!s_mx) return;          /* themes_page_init() did not run */
    const esp_err_t e = orb_jobs_post(job_run, NULL, 0);
    if (e == ESP_OK) s_walk = true;   /* in flight from here to the publish */
    else if (e == ESP_ERR_INVALID_STATE)
        strlcpy(s_ui.status, "the card is already being read", sizeof s_ui.status);
    else if (e != ESP_OK)
        ESP_LOGW(TAG, "theme scan refused (%s)", esp_err_to_name(e));
}

/* The page's own busy answer, asked by the shell before it lets the knob move.
 * True from an accepted post until the list publish lands — including the
 * whole window before the worker starts, which the old s_dirty handshake
 * left unguarded and let the shell paint stale rows under a live walk (the
 * card's contents ARE the rows; an answer replaces them whole, so a cursor
 * that could move or press through the walk would stand on a row the publish
 * may delete). The rejected posts are not outstanding — the page shows the
 * refusal instead of a lock the queue never armed. */
bool themes_page_walk_outstanding(void)
{
    return s_walk || s_dirty;
}

bool themes_page_busy(void)
{
    return orb_jobs_busy();
}

const char *themes_page_status(void)
{
    return s_ui.status;
}

void themes_page_reset(void)
{
    /* Nothing to carry: the rows are re-walked on entry, so there is no mid-list
     * position to restore and no stale row to point at. The preview goes with
     * the rows — the leave path drops the plane, so re-entering answers "what
     * is on the card" again rather than holding a picture of the theme the
     * cursor last sat on. */
    themes_page_drop_preview();
    s_zc_drop = true;                  /* §60.2: the cache goes with the page, once no job holds it */
    s_moved_us = 0;
}

/* Drop the displayed plane and any published-but-unreaped answer. Runs on the
 * LVGL task only (the leave path, and service()'s discard): the buffer it
 * frees may be the one the shell blitted on the last repaint, and the worker
 * never paints, so the worker never frees. */
void themes_page_drop_preview(void)
{
    if (s_pvu.px) heap_caps_free(s_pvu.px);
    memset(&s_pvu, 0, sizeof s_pvu);
    s_pv_row = -1;
    s_pv_since = 0;
}

/* §60.2: the preview work's own task. On the jobs worker it froze the page: the shell drops
 * every turn while orb_jobs_busy() (a running job owns the cursor's row), so a prefetch in
 * flight swallowed the owner's turns (Gold, 2026-11-07: three detents, two landed). Here it
 * never counts as the page being busy; the card lock still orders it against the worker.
 * One request at a time, the service retrying a post the task is not free to take. */
static TaskHandle_t s_pvt;
static volatile bool s_pvt_busy;
static void (*s_pvt_fn)(void *);
static uint8_t s_pvt_arg[sizeof(pv_ask_t)];
static void pvt_main(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_pvt_fn) s_pvt_fn(s_pvt_arg);
        s_pvt_busy = false;
    }
}
static esp_err_t pvt_post(void (*fn)(void *), const void *arg, size_t n)
{
    if (!s_pvt) {
        if (xTaskCreatePinnedToCoreWithCaps(pvt_main, "theme_pv", 8192, NULL, 2, &s_pvt,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_pvt = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_pvt_busy || n > sizeof s_pvt_arg) return ESP_ERR_INVALID_STATE;
    s_pvt_busy = true;
    s_pvt_fn = fn;
    memset(s_pvt_arg, 0, sizeof s_pvt_arg);
    memcpy(s_pvt_arg, arg, n);
    xTaskNotifyGive(s_pvt);
    return ESP_OK;
}

void themes_page_service(void)
{
    if (s_dirty) {
        s_dirty = false;
        s_walk = false;               /* the walk's answer is in hand: no longer in flight */
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

        const int i = mine(s_pv_row - ROW_ITEMS);
        const bool ours = i >= 0 && next.id[0] &&
                          strcmp(next.id, s_ui.e[i].id) == 0;
        if (!ours) {
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

    /* §60.2: the page left — give the memory back once no job can hold a pointer into it */
    if (s_zc_drop && !s_armed && !s_pvt_busy && !s_z_fly) {
        zc_drop_all();
        s_zc_drop = false;
    }
    if (s_armed) s_zc_drop = false;
    /* §60.2: a fetch landed: file it, making room by dropping what is farthest from the
     * cursor (never while a job may still hold a pointer — the worker is free here). */
    if (s_z_dirty && !s_pvt_busy) {
        s_z_dirty = false;
        s_z_fly = false;
        s_z_done_us = esp_timer_get_time();
        xSemaphoreTake(s_mx, portMAX_DELAY);
        zc_t got = { .z = s_zx.z, .n = s_zx.n, .side = s_zx.side };
        strlcpy(got.id, s_zx.id, sizeof got.id);
        memset(&s_zx, 0, sizeof s_zx);
        xSemaphoreGive(s_mx);
        if (got.z && !zc_find(got.id)) {
            const int cur = s_pv_row - ROW_ITEMS;
            /* §60.3: only what is farther from the cursor than the newcomer makes room for it.
             * Evicting a nearer one let the prefetch fetch it straight back, evict the
             * newcomer, and loop, reading the card while the cursor rested (Gold). */
            const int dnew = zc_dist(got.id, cur);
            for (;;) {
                bool slot_free = false;
                for (int i = 0; i < ZC_MAX; i++) if (!s_zc[i].z) slot_free = true;
                if (slot_free && zc_bytes() + got.n <= ZC_BUDGET) break;
                int far = -1, fd = -1;
                for (int i = 0; i < ZC_MAX; i++) {
                    if (!s_zc[i].z) continue;
                    const int d = zc_dist(s_zc[i].id, cur);
                    if (d > fd) { fd = d; far = i; }
                }
                if (far < 0 || fd <= dnew) { s_zc_full = true; break; }
                heap_caps_free(s_zc[far].z);
                memset(&s_zc[far], 0, sizeof s_zc[far]);
            }
            int slot = -1;
            for (int i = 0; i < ZC_MAX && slot < 0; i++) if (!s_zc[i].z) slot = i;
            if (slot >= 0 && zc_bytes() + got.n <= ZC_BUDGET) s_zc[slot] = got;
            else { heap_caps_free(got.z); s_zc_full = true; }
        } else if (got.z) {
            heap_caps_free(got.z);
        } else {
            /* §61: a fetch that came back empty (no memory, a moved cursor) rests the
             * prefetch until the next move; retrying at once read the card in a loop with
             * PSRAM gone (Gold, 2026-11-08). */
            s_zc_full = true;
        }
    }

    /* The settle: post one read once the cursor has rested on a theme row for
     * PV_SETTLE_MS with the worker free. The rest rule is the whole reason a
     * land queues a read at all — a read per detent through a 62-row spin is
     * ~108 KB of card work per stop and the knob never answers; a read after
     * the spin costs the one theme the owner stopped on. */
    const int pvi = mine(s_pv_row - ROW_ITEMS);
    /* §60.3: a cached theme costs no card, so it waits only long enough to let a spin's
     * next detent arrive; the card read keeps the longer rest. */
    const int settle_ms = pvi >= 0 && zc_find(s_ui.e[pvi].id) ? PV_SETTLE_CACHED_MS : PV_SETTLE_MS;
    if (pvi >= 0 && s_pv_since && !orb_jobs_busy() && !s_pvt_busy &&
        esp_timer_get_time() / 1000 - s_pv_since / 1000 >= settle_ms) {
        const int i = pvi;
        if (!s_pvu.px || strcmp(s_pvu.id, s_ui.e[i].id) != 0) {
            pv_ask_t ask;
            memset(&ask, 0, sizeof ask);
            strlcpy(ask.id, s_ui.e[i].id, sizeof ask.id);
            const zc_t *c = zc_find(ask.id);   /* §60.2: cached = no card */
            if (c) { ask.z = c->z; ask.n = c->n; ask.side = c->side; }
            ask.moves = s_moves;
            const esp_err_t e = pvt_post(pv_job_run, &ask, sizeof ask);
            if (e == ESP_OK) s_pv_since = 0;   /* one post per land, settled */
            if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)
                ESP_LOGW(TAG, "preview post refused (%s)", esp_err_to_name(e));
        }
    }

    /* §60.2: the prefetch. Only while the page is up, the cursor has rested ZC_REST_MS,
     * the worker is free and its own preview is in hand; nearest the cursor first. */
    /* ... and never back to back: each fetch holds the card, and the card holds the display's
     * DMA ring, so five in a row kept the glass still for seconds and the owner's turns looked
     * ignored. The next waits for the ring to come home and the glass to draw a while. */
    /* §60.3: the ring is no longer waited home. It comes back on the next flush, and a page
     * at rest draws nothing, so the wait spaced fetches ~1.5 s apart for no frame at all;
     * each fetch now gives the card back between short segments, and the gap stands. The
     * order runs ahead the way the owner is turning, then one behind. */
    if (s_armed && !s_z_fly && !s_z_dirty && !s_zc_full && !orb_jobs_busy() && !s_pvt_busy &&
        s_pv_since == 0 && esp_timer_get_time() - s_z_done_us >= (int64_t)ZC_GAP_MS * 1000 &&
        s_moved_us && esp_timer_get_time() - s_moved_us >= (int64_t)ZC_REST_MS * 1000) {
        const int cur = s_pv_row - ROW_ITEMS;
        const int8_t d = s_dir;
        const int8_t ORDER[] = { 0, d, (int8_t)(2 * d), (int8_t)-d, (int8_t)(3 * d) };
        for (size_t o = 0; o < sizeof ORDER; o++) {
            const int k = mine(cur + ORDER[o]);
            if (k < 0 || zc_find(s_ui.e[k].id) || s_ui.e[k].err != ESP_OK) continue;
            pv_ask_t zq;
            memset(&zq, 0, sizeof zq);
            strlcpy(zq.id, s_ui.e[k].id, sizeof zq.id);
            zq.moves = s_moves;
            if (pvt_post(zc_job_run, &zq, sizeof zq) == ESP_OK)
                s_z_fly = true;
            break;
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
static void pick_entry(const orb_theme_entry_t *e);
void themes_page_pick(int idx)
{
    const int k = mine(idx);              /* this app's idx-th theme */
    if (k < 0) return;
    ESP_LOGI(TAG, "pick %d/%d: %s (%s)", idx, s_ui.n - 1,
             s_ui.e[k].name[0] ? s_ui.e[k].name : s_ui.e[k].id, s_ui.e[k].id);
    pick_entry(&s_ui.e[k]);
}

const char *themes_page_pick_id(const char *id)
{
    for (int k = 0; id && k < s_ui.n; k++) {
        if (strcmp(s_ui.e[k].id, id) != 0) continue;
        ESP_LOGI(TAG, "pick %s (%s) from the carousel",
                 s_ui.e[k].name[0] ? s_ui.e[k].name : id, id);
        pick_entry(&s_ui.e[k]);
        return s_ui.status;
    }
    strlcpy(s_ui.status, "that theme is no longer on the card", sizeof s_ui.status);
    return s_ui.status;
}

static void pick_entry(const orb_theme_entry_t *e)
{
    if (e->err != ESP_OK) {
        strlcpy(s_ui.status, e->why[0] ? e->why : "not loadable", sizeof s_ui.status);
        return;
    }
    if (e->active) {
        snprintf(s_ui.status, sizeof s_ui.status, "\"%.40s\" is already on the dial", e->name);
        return;
    }
    const esp_err_t q = orb_theme_post_bundle(e->id);
    if (q == ESP_ERR_INVALID_STATE)
        strlcpy(s_ui.status, "one theme is already being installed", sizeof s_ui.status);
    else if (q != ESP_OK)
        snprintf(s_ui.status, sizeof s_ui.status, "\"%.36s\" could not start: %s",
                 e->name, esp_err_to_name(q));
    else
        snprintf(s_ui.status, sizeof s_ui.status, "storing \"%.48s\" …", e->name);
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

esp_err_t themes_page_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    return s_mx ? ESP_OK : ESP_ERR_NO_MEM;
}

int themes_opt_count(orb_app_t *app)
{
    for_app(app);
    /* §58: an app with no theme on the card has an empty sub-menu (its page shows
     * no Themes row then, so this is reached only mid-walk). */
    if (mine_n() == 0) return 0;
    /* The one group header plus the card's themes. Nothing here ever counted the
     * reboot question any more — ruling D11 deleted it, because a pick installs
     * and reboots by itself and there is no second press to have faith in. Migration
     * 1 dropped the factory-default row with the A/B slot it pointed at. The items
     * stay in the count while their group is CLOSED: the shell hides them, and an
     * app that dropped them from its own number would be a second opinion about what
     * is on the page, which is the one thing §13.4 forbids. */
    return ROW_ITEMS + mine_n();
}

/** Which of the two shapes a row has (ruling D12). One group and its items — the
 *  shape the shell validates and renders, and the reason the card's themes are
 *  ITEMs rather than loose rows. Migration 1 dropped the plain factory-default row
 *  that used to lead the page. */
orb_opt_kind_t themes_opt_kind(orb_app_t *app, int idx)
{
    for_app(app);
    if (idx == ROW_GROUP)   return OPT_GROUP;
    if (mine(idx - ROW_ITEMS) >= 0) return OPT_ITEM;
    return OPT_ROW;                     /* out of range: the shell's bounds answer */
}

const char *themes_opt_label(orb_app_t *app, int idx)
{
    for_app(app);
    /* The card is a GROUP, at row 0, always. Migration 1 retired the factory-default
     * ROW that used to lead the page: under the single slot there is no press that
     * returns the dial to a second copy, so the group is the whole page and the
     * base face needs no row to name it — it is what a slot with nothing valid in
     * it already draws. */
    if (idx == ROW_GROUP)   return "Themes";
    const int i = mine(idx - ROW_ITEMS);
    if (i < 0) return "";
    const orb_theme_entry_t *e = &s_ui.e[i];
    return e->name[0] ? e->name : e->id;
}

const char *themes_opt_value(orb_app_t *app, int idx)
{
    for_app(app);
    static char v[48];                  /* one row's answer, repainted per poll */
    v[0] = '\0';
    /* Every string below is a read of state the theme layer already holds: the
     * source boot resolved at boot, what its last install attempt could not do, and
     * the witness a store was armed and torn. None of them touches the card, NVS or
     * the network, which is the rule these answers live under — the same callback
     * is the dial's animation. */
    if (idx == ROW_GROUP) {
        /* The header answers with a number, or says nothing. The list opens
         * with the page, so the glass already shows what lives here and the
         * value the owner asked for is how many. Before the first walk lands
         * s_ui.n is honestly 0 and the row says nothing; the walk's publish
         * grows the row count, and a grown page is what makes the shell
         * repaint the glass without a knob turn. Load state and refusal
         * reasons live on their own theme rows, never on this line (owner
         * ruling, 2026-10-26: the header does not editorialize). */
        const int n = mine_n();
        if (n <= 0) return "";
        snprintf(v, sizeof v, "%d theme%s", n, n == 1 ? "" : "s");
        return v;
    }
    const int i = mine(idx - ROW_ITEMS);
    if (i < 0) return "";
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

void themes_opt_inc(orb_app_t *app, int idx, int32_t steps)
{
    (void)app;
    /* Rows here are chosen, not spun: there is no value to increment, so the
     * shell moves the highlight and the press does the work. A theme page with
     * a spinner row would be a different shape and would say so here. */
    (void)idx; (void)steps;
}

void themes_opt_sel(orb_app_t *app, int idx)
{
    /* D35/D19: a theme pick is the clock asking for the D11 install, and the
     * clock's manifest DECLARES themes — so the declaration is consulted here,
     * not skipped because the module predates its own row. A revoked cap
     * degrades the page visibly: the refusal is status text on the group the
     * owner just pressed, and the install never reaches the worker. Migration 1
     * left the group header as the only non-item row, so an ITEM press is the
     * only one that needs the themes cap. */
    if (idx == ROW_GROUP)   return;
    for_app(app);
    themes_page_pick(idx - ROW_ITEMS);
}

void themes_opt_arm(orb_app_t *app, bool armed)
{
    for_app(app);
    s_armed = armed;
    if (armed) {
        /* Entering the page is when the card is worth walking: the rows are the
         * card's contents, and contents change between visits. D35/D19: the walk
         * reads the card, the manifest declares sd_read, and the ledger counts
         * the refusal the same way it counts a guest's — denied is a state the
         * page shows, not an error the app hides. */
        themes_page_request_scan();
    } else {
        themes_page_reset();
    }
}

void themes_opt_focus(orb_app_t *app, int idx)
{
    /* The preview read is a card read: the manifest declares sd_read and the
     * gate says whether it still runs, exactly as themes_opt_arm's walk does.
     * A revoked cap makes the focus a no-op — the revoke is already on the
     * status line from the arm, and a row with no preview is the same page
     * every theme without one draws. */
    for_app(app);
    /* The shell tells the app where its cursor landed, in the app's own row
     * numbers. A land on the group header (row 0) or past the walk's end shows
     * nothing; the plane drops so the list the owner can actually see is the
     * list on the glass. A land on the same row is not a new land: the settle
     * only re-arms when the row changes. */
    if (idx == s_pv_row) return;
    if (s_pv_row >= 0) s_dir = idx > s_pv_row ? 1 : -1;   /* §60.3: the way it is turning */
    s_pv_row = idx;
    s_moved_us = esp_timer_get_time();   /* §60.2: a move restarts the prefetch's rest */
    s_moves++;                           /* §60.3: and gives up a read for the row it left */
    s_zc_full = false;
    s_pv_since = idx >= ROW_ITEMS ? esp_timer_get_time() : 0;
    if (mine(idx - ROW_ITEMS) < 0) {
        if (s_pvu.px) heap_caps_free(s_pvu.px);
        memset(&s_pvu, 0, sizeof s_pvu);
    }
}

const void *themes_opt_preview(orb_app_t *app, int idx, uint16_t *w,
                              uint16_t *h, uint32_t *stamp)
{
    for_app(app);
    /* The answer half, not the ask: it reads only the reaped plane (s_pvu), the
     * same rule opt_label and opt_value live under — the callback that answers
     * a row is the dial's animation, and a card read here would hold the task
     * that paints it. NULL is the text-only answer: nothing reaped for this
     * row, a theme with no preview (NO_CONTENT, never an error), a page whose
     * cursor is not on an item, or a group that is closed. */
    if (!s_pvu.px || idx != s_pv_row) return NULL;
    const int k = mine(idx - ROW_ITEMS);
    if (k < 0) return NULL;
    /* §6's identity rule applies to a picture too: the plane answers the row
     * that carries the same id it was read for. A card re-walk that moved the
     * theme out from under the cursor answers text, not the old face. */
    if (strcmp(s_ui.e[k].id, s_pvu.id) != 0) return NULL;
    *w = s_pvu.w;
    *h = s_pvu.h;
    *stamp = s_pvu.gen;
    return s_pvu.px;
}

bool themes_opt_armed(void)
{
    return s_armed;
}

/* ---- the provider row --------------------------------------------------------------
 * No face, no ledger slot and no menu row (§58): orb_app.c initialises and services it,
 * and appends its hooks' rows to every card app's page. */
static esp_err_t page_init(orb_app_t *app)    { (void)app; return themes_page_init(); }
static void      page_service(orb_app_t *app) { (void)app; themes_page_service(); }

static const orb_app_iface_t s_themes_iface = {
    .init        = page_init,
    .service     = page_service,
    .opt_count   = themes_opt_count,
    .opt_label   = themes_opt_label,
    .opt_value   = themes_opt_value,
    .opt_kind    = themes_opt_kind,
    .opt_inc     = themes_opt_inc,
    .opt_sel     = themes_opt_sel,
    .opt_arm     = themes_opt_arm,
    .opt_focus   = themes_opt_focus,
    .opt_preview = themes_opt_preview,
};

orb_app_t orb_app_themes_row = {
    .slug       = "themes",
    .title      = "Themes",
    .kind       = ORB_APP_KIND_APP,
    .behaviour_kind = ORB_BEHAVIOUR_NONE,
    .art_slot   = -1,
    .flash_slot = -1,
    .iface      = &s_themes_iface,
    .installed  = true,
};
