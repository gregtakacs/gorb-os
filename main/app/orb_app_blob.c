/* Kept bodies — contract in orb_app_blob.h.
 *
 * Two tiers since radar-plan §7 (owner, 2026-11-09): a body in PSRAM (the glass region where
 * it fits), which is what an app draws from, and a copy on the card, which is where a body
 * the app parks waits until it loads it again. The table is the LVGL task's: it owns every
 * PSRAM pointer, takes a load's buffer before the read and adopts it after, and frees a
 * parked body's PSRAM once it is written (orb_blob_service). The `blobio` task only moves
 * bytes, in pieces, between a buffer it was handed and the card; the job fields it shares
 * with the LVGL task sit under s_mx. */
#include "orb_app_blob.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "orb_app.h"        /* ORB_APP_CAP */
#include "orb_glass.h"      /* §6.21: a kept body lives in the glass region when it fits */
#include "orb_jobs.h"
#include "orb_sd.h"
#include "orb_strip.h"
#include "orb_theme.h"      /* orb_theme_upload_busy */
#include "orb_usb_msc.h"
#include "input_router.h"
#include "theme_strip.h"

static const char *TAG = "blob";

#define BLOB_DIR      ORB_SD_BASE "/System/blobs"
#define PIECE         (16u * 1024u)   /* one hold of the card lock: ~25 ms read, ~65 ms write */
#define WRITE_QUIET_MS 1500           /* another writer this recently: the park waits */
#define IO_STACK      6144            /* FatFs and stdio; PSRAM */
/* Below everything that draws: lvgl (5), the network (4), the jobs worker (3) and the live
 * layers' worker (2), which decodes the next rain frame; and on the core lvgl is not on. The
 * SD driver's polled transfers spin, and at 2 a load shared a core with that decode. */
#define IO_PRIO       1
#define IO_CORE       0

enum { J_NONE, J_WRITE, J_READ };
enum { D_NONE, D_OK, D_FAIL };

typedef struct {
    /* the LVGL task's */
    uint8_t *buf;                 /* the PSRAM copy, or NULL */
    uint32_t len;                 /* the body's length, whatever tier it is in */
    bool     card;                /* a clean copy is on the card */
    uint32_t gen;                 /* bumped when the slot is freed or replaced */
    /* shared with blobio, under s_mx */
    uint8_t  job, done;
    bool     running;
    uint8_t *jbuf;                /* the buffer a job writes from or reads into */
    uint32_t jlen, jgen;
    bool     orphan;              /* the slot moved on: the job's buffer is the LVGL task's to free */
} ent_t;

typedef struct {
    ent_t    e[ORB_BLOB_N];
    uint32_t used;                /* PSRAM bytes, a load counted from when it is asked */
    int64_t  off_since_us;        /* 0 while on the glass or empty */
    bool     wipe;                /* blobio: the app's card files go */
} app_blobs_t;

/* PSRAM: a few kilobytes nobody reads on the frame path. */
EXT_RAM_BSS_ATTR static app_blobs_t s_b[ORB_APP_CAP];
static SemaphoreHandle_t s_mx;
EXT_RAM_BSS_ATTR static StaticSemaphore_t s_mx_buf;
static TaskHandle_t s_io;
static volatile bool s_booted_wipe;

static app_blobs_t *of(int app_slot) { return app_slot >= 0 && app_slot < ORB_APP_CAP ? &s_b[app_slot] : NULL; }
static void lock(void)   { if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf); xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void path_of(char *out, size_t n, int app_slot, int i)
{
    snprintf(out, n, BLOB_DIR "/s%d-%d.bin", app_slot, i);
}

/* The slot's body leaves: its PSRAM (now, or when a job holding it finishes) and its card copy
 * (left on the card: the next park of the slot writes over it, and a wipe takes it). */
static void drop(app_blobs_t *b, int i)
{
    ent_t *e = &b->e[i];
    lock();
    const bool busy = e->job != J_NONE;
    const bool holds_buf = busy && e->job == J_WRITE && e->jbuf == e->buf;
    if (busy) {
        e->orphan = true;
        if (e->job == J_READ) b->used -= e->jlen;          /* the reservation goes now */
    }
    e->gen++;
    unlock();
    if (e->buf) {
        b->used -= e->len;
        if (!holds_buf) orb_glass_free(e->buf);             /* else freed when the write ends */
    }
    e->buf = NULL;
    e->len = 0;
    e->card = false;
}

/* Under s_mx. An orphaned job nobody is running gives up its buffer (the caller frees it,
 * outside the lock) and the entry is free again. A running one stays: blobio still holds its
 * buffer, and reaps it itself when the piece in hand ends. Whoever reuses an entry calls this
 * first, so a new job never writes over an orphan's jbuf and loses it. */
static uint8_t *reap_locked(ent_t *e)
{
    if (e->job == J_NONE || e->running || !(e->orphan || e->jgen != e->gen)) return NULL;
    uint8_t *jb = e->jbuf;
    e->job = J_NONE;
    e->done = D_NONE;
    e->jbuf = NULL;
    e->orphan = false;
    return jb;
}

esp_err_t orb_blob_put(int app_slot, int i, uint8_t *buf, uint32_t len)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N || !buf) return ESP_ERR_INVALID_ARG;
    ent_t *e = &b->e[i];
    if (b->used - (e->buf ? e->len : 0) + len > ORB_BLOB_BUDGET) return ESP_ERR_NO_MEM;
    drop(b, i);
    b->used += len;
    e->buf = buf;
    e->len = len;
    return ESP_OK;
}

const uint8_t *orb_blob_get(int app_slot, int i, uint32_t *len)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N || !b->e[i].buf) { if (len) *len = 0; return NULL; }
    if (len) *len = b->e[i].len;
    return b->e[i].buf;
}

uint32_t orb_blob_len(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    return b && i >= 0 && i < ORB_BLOB_N ? b->e[i].len : 0;
}

void orb_blob_free(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N) return;
    drop(b, i);
}

void orb_blob_free_all(int app_slot)
{
    app_blobs_t *b = of(app_slot);
    if (!b) return;
    bool any = false;
    for (int i = 0; i < ORB_BLOB_N; i++) { any |= b->e[i].card || b->e[i].job; drop(b, i); }
    b->off_since_us = 0;
    if (any) {
        lock();
        b->wipe = true;
        unlock();
        if (s_io) xTaskNotifyGive(s_io);
    }
}

uint32_t orb_blob_used(int app_slot)
{
    const app_blobs_t *b = of(app_slot);
    return b ? b->used : 0;
}

int orb_blob_state(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N) return ORB_BLOB_EMPTY;
    const ent_t *e = &b->e[i];
    lock();
    const bool busy = e->job != J_NONE && !e->orphan;
    unlock();
    if (busy) return ORB_BLOB_MOVING;
    return e->buf ? ORB_BLOB_RAM : e->card ? ORB_BLOB_CARD : ORB_BLOB_EMPTY;
}

/* ---- the card ---------------------------------------------------------------------------- */

static bool card_usable(void)
{
    return orb_sd_mounted() && !orb_sd_usb_owned() && !orb_usb_msc_window_open();
}

/* What a park stands aside for (radar-plan §7.3): the card's last priority. */
static const char *write_waits_for(void)
{
    if (orb_strip_active()) return "the carousel";
    if (theme_strip_active()) return "the theme carousel";
    if (input_router_switching()) return "an app switch";
    if (orb_jobs_busy()) return "a job";
    if (orb_theme_upload_busy()) return "an upload";
    if (orb_sd_write_idle_ms() < WRITE_QUIET_MS) return "another writer";
    return NULL;
}

static bool io_up(void);

esp_err_t orb_blob_park(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N) return ESP_ERR_INVALID_ARG;
    ent_t *e = &b->e[i];
    if (!e->buf) return e->card ? ESP_OK : ESP_ERR_NOT_FOUND;
    lock();
    uint8_t *gone = reap_locked(e);
    const uint8_t job = e->job == J_NONE ? J_NONE : e->orphan ? 0xff : e->job;
    unlock();
    orb_glass_free(gone);
    if (job == J_WRITE) return ESP_OK;
    if (job != J_NONE) return ESP_ERR_INVALID_STATE;
    if (e->card) {                                      /* already there: the PSRAM goes now */
        orb_glass_free(e->buf);
        b->used -= e->len;
        e->buf = NULL;
        return ESP_OK;
    }
    if (!card_usable() || !io_up()) return ESP_ERR_NOT_SUPPORTED;
    lock();
    e->job = J_WRITE;
    e->done = D_NONE;
    e->running = false;
    e->orphan = false;
    e->jbuf = e->buf;
    e->jlen = e->len;
    e->jgen = e->gen;
    unlock();
    xTaskNotifyGive(s_io);
    return ESP_OK;
}

esp_err_t orb_blob_load(int app_slot, int i)
{
    app_blobs_t *b = of(app_slot);
    if (!b || i < 0 || i >= ORB_BLOB_N) return ESP_ERR_INVALID_ARG;
    ent_t *e = &b->e[i];
    if (e->buf) return ESP_OK;
    if (!e->card) return ESP_ERR_NOT_FOUND;
    lock();
    uint8_t *gone = reap_locked(e);
    const uint8_t job = e->job == J_NONE ? J_NONE : e->orphan ? 0xff : e->job;
    unlock();
    orb_glass_free(gone);
    if (job == J_READ) return ESP_OK;
    if (job != J_NONE) return ESP_ERR_INVALID_STATE;
    if (b->used + e->len > ORB_BLOB_BUDGET) return ESP_ERR_NO_MEM;
    if (!card_usable() || !io_up()) return ESP_ERR_NOT_SUPPORTED;
    uint8_t *rb = orb_glass_alloc_only(e->len);
    if (!rb) rb = heap_caps_malloc(e->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rb) return ESP_ERR_NO_MEM;
    b->used += e->len;
    lock();
    e->job = J_READ;
    e->done = D_NONE;
    e->running = false;
    e->orphan = false;
    e->jbuf = rb;
    e->jlen = e->len;
    e->jgen = e->gen;
    unlock();
    xTaskNotifyGive(s_io);
    return ESP_OK;
}

/* The LVGL task adopts what blobio finished: a load's buffer becomes the body, a written
 * park's PSRAM goes, and an orphaned job's buffer is freed. */
void orb_blob_service(int app_slot)
{
    app_blobs_t *b = of(app_slot);
    if (!b || !s_mx) return;
    for (int i = 0; i < ORB_BLOB_N; i++) {
        ent_t *e = &b->e[i];
        lock();
        if (e->job == J_NONE || e->done == D_NONE) { unlock(); continue; }
        const uint8_t job = e->job, done = e->done;
        uint8_t *jb = e->jbuf;
        const uint32_t jlen = e->jlen;
        const bool orphan = e->orphan || e->jgen != e->gen;
        e->job = J_NONE;
        e->done = D_NONE;
        e->jbuf = NULL;
        e->orphan = false;
        unlock();
        if (orphan) {                                   /* the slot moved on */
            if (job == J_READ || jb != e->buf) orb_glass_free(jb);
            continue;
        }
        if (job == J_WRITE) {
            if (done == D_OK) {
                e->card = true;
                if (e->buf == jb) { orb_glass_free(e->buf); b->used -= e->len; e->buf = NULL; }
            } else {
                ESP_LOGW(TAG, "slot %d body %d: the card would not take it; it stays in PSRAM", app_slot, i);
            }
        } else if (done == D_OK) {
            e->buf = jb;                                /* counted since the ask */
        } else {
            orb_glass_free(jb);
            b->used -= jlen;
            e->card = false;                            /* the copy is no good: the app fetches again */
            ESP_LOGW(TAG, "slot %d body %d: its card copy would not read", app_slot, i);
        }
    }
}

void orb_blob_note_glass(int app_slot, bool on_glass, int64_t now_us)
{
    app_blobs_t *b = of(app_slot);
    if (!b) return;
    bool any = b->used != 0;
    for (int i = 0; !any && i < ORB_BLOB_N; i++) any = b->e[i].card;
    if (!any) { b->off_since_us = 0; return; }
    if (on_glass) { b->off_since_us = 0; return; }
    if (!b->off_since_us) { b->off_since_us = now_us; return; }
    if (now_us - b->off_since_us >= (int64_t)ORB_BLOB_LINGER_S * 1000000) {
        ESP_LOGI(TAG, "slot %d: %u B freed, and its card copies, after %d min off the glass", app_slot,
                 (unsigned)b->used, ORB_BLOB_LINGER_S / 60);
        orb_blob_free_all(app_slot);
    }
}

/* ---- blobio --------------------------------------------------------------------------- */

static void wipe_files(int app_slot)              /* -1: every app's (a new boot's) */
{
    char pre[16];
    snprintf(pre, sizeof pre, "s%d-", app_slot);
    orb_sd_lock();
    DIR *d = opendir(BLOB_DIR);
    int n = 0;
    for (struct dirent *de; d && (de = readdir(d)) != NULL;) {
        if (app_slot >= 0 && strncmp(de->d_name, pre, strlen(pre))) continue;
        char p[96];
        snprintf(p, sizeof p, BLOB_DIR "/%.40s", de->d_name);
        if (unlink(p) == 0) n++;
    }
    if (d) closedir(d);
    orb_sd_unlock();
    if (n) ESP_LOGI(TAG, "%d card cop%s gone (%s)", n, n == 1 ? "y" : "ies",
                    app_slot < 0 ? "a new boot" : "the app's bodies went");
}

/* Whether the slot's job is still wanted (it may be orphaned at any piece). */
static bool still(app_blobs_t *b, int i, uint32_t gen)
{
    lock();
    const bool ok = !b->e[i].orphan && b->e[i].gen == gen;
    unlock();
    return ok;
}

static bool run_job(int a, int i)
{
    app_blobs_t *b = &s_b[a];
    ent_t *e = &b->e[i];
    lock();
    const uint8_t job = e->job;
    uint8_t *jb = e->jbuf;
    const uint32_t len = e->jlen, gen = e->jgen;
    e->running = true;
    unlock();
    char path[64];
    path_of(path, sizeof path, a, i);
    const int64_t t0 = esp_timer_get_time();
    bool ok = false, said = false;
    if (job == J_WRITE) {
        orb_sd_lock();
        mkdir(ORB_SD_BASE "/System", 0777);
        mkdir(BLOB_DIR, 0777);
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        orb_sd_unlock();
        uint32_t off = 0;
        while (fd >= 0 && off < len) {
            for (const char *w; (w = write_waits_for()) != NULL && still(b, i, gen);) {
                if (!said) { ESP_LOGI(TAG, "a park stands aside for %s", w); said = true; }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!still(b, i, gen) || !card_usable()) break;
            const uint32_t n = len - off < PIECE ? len - off : PIECE;
            orb_sd_lock();
            const ssize_t r = write(fd, jb + off, n);
            orb_sd_unlock();
            if (r != (ssize_t)n) break;
            off += n;
            vTaskDelay(pdMS_TO_TICKS(5));               /* the card's other users get in */
        }
        if (fd >= 0) { orb_sd_lock(); ok = close(fd) == 0 && off == len; orb_sd_unlock(); }
        if (!ok && fd >= 0) { orb_sd_lock(); unlink(path); orb_sd_unlock(); }
    } else {
        orb_sd_lock();
        const int fd = open(path, O_RDONLY);
        orb_sd_unlock();
        uint32_t off = 0;
        while (fd >= 0 && off < len && still(b, i, gen) && card_usable()) {
            const uint32_t n = len - off < PIECE ? len - off : PIECE;
            orb_sd_lock();
            const ssize_t r = read(fd, jb + off, n);
            orb_sd_unlock();
            if (r != (ssize_t)n) break;
            off += n;
            vTaskDelay(1);                              /* the card's other users get in */
        }
        if (fd >= 0) { orb_sd_lock(); close(fd); orb_sd_unlock(); }
        ok = off == len;
    }
    lock();
    e->running = false;
    e->done = ok ? D_OK : D_FAIL;
    uint8_t *gone = reap_locked(e);     /* the slot moved on: its app may be gone, and with it */
    unlock();                           /* the service call that would have freed this */
    orb_glass_free(gone);
    ESP_LOGI(TAG, "slot %d body %d: %s %u B %s in %d ms", a, i, job == J_WRITE ? "parked" : "loaded",
             (unsigned)len, ok ? "" : "(failed)", (int)((esp_timer_get_time() - t0) / 1000));
    return true;
}

/* The next job: any app's load first, then any app's park. */
static bool pick(int *a_out, int *i_out, bool writes)
{
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t want = pass == 0 ? J_READ : J_WRITE;
        if (want == J_WRITE && !writes) break;
        lock();
        for (int a = 0; a < ORB_APP_CAP; a++)
            for (int i = 0; i < ORB_BLOB_N; i++) {
                const ent_t *e = &s_b[a].e[i];
                if (e->job != want || e->done != D_NONE || e->running) continue;
                if (e->orphan || e->jgen != e->gen) {            /* nothing to move */
                    uint8_t *gone = reap_locked(&s_b[a].e[i]);
                    unlock();
                    orb_glass_free(gone);
                    lock();
                    continue;
                }
                unlock();
                *a_out = a;
                *i_out = i;
                return true;
            }
        unlock();
    }
    return false;
}

static void io_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        if (!card_usable()) continue;
        if (!s_booted_wipe) { wipe_files(-1); s_booted_wipe = true; }
        for (int a = 0; a < ORB_APP_CAP; a++) {
            lock();
            const bool w = s_b[a].wipe;
            s_b[a].wipe = false;
            unlock();
            if (w) wipe_files(a);
        }
        int a, i;
        while (card_usable() && pick(&a, &i, true)) (void)run_job(a, i);
    }
}

static bool io_up(void)
{
    if (s_io) return true;
    if (!s_mx) s_mx = xSemaphoreCreateMutexStatic(&s_mx_buf);
    StackType_t *stk = heap_caps_malloc(IO_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = heap_caps_calloc(1, sizeof *tcb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stk || !tcb) { heap_caps_free(stk); heap_caps_free(tcb); return false; }
    s_io = xTaskCreateStaticPinnedToCore(io_task, "blobio", IO_STACK, NULL, IO_PRIO, stk, tcb, IO_CORE);
    return s_io != NULL;
}
