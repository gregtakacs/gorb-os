/**
 * orb_jobs.c — the shell's one worker task (see orb_jobs.h).
 *
 * The sizing and the watermark log are lifted from the menu's worker, which is
 * where they were measured. The original 4096 held against a real 3.87 MB
 * container through the copy chain as it stood then; the C4 dialect re-stamp,
 * the §31 verdict gate and the §5.3 page-billing loop each added frames to
 * orb_theme_install_bundle() on top of FatFs and the esp_partition write path.
 * 2026-10-24, orb-B: two stores of the same 4.94 MB bundle overflowed this
 * stack identically at PC 0x4037c87b during copy_bundle3 — "***ERROR*** A
 * stack overflow in task orb_jobs" — each wiping the slot and rebooting to
 * the base face. The worker now carries the store chain's demand at 8192, and
 * that number is boot-coupled, not just a depth: this stack's allocation is
 * part of the block history httpd's own contiguous 8 KB task stack (orb_net.c
 * cfg.stack_size) reads from at its boot instant. The day's alternatives all
 * failed that reading — 4096 alone died at largest=8192, 4096 with the OTA
 * chunk's 4 KB reclaimed to PSRAM died at 12288, and deferred httpd starts
 * died in every later phase (a failed httpd_start leaks its listen socket
 * inside esp_http_server and poisons the boot's retries with ENOMEM 112).
 * No HEAD build served a page on this board the day its slot was torn —
 * the torn-slot boot carries a ~13 KB internal pool penalty (see the
 * .clinerules note of 2026-10-24) that the 8192 worker does not cause but
 * must live with. Do not shrink this stack without re-running the
 * boot-ladder audit (P5.5, the heap_init / post-wifi_start / httpd_start:
 * grep) on both orbs, slot whole; the high-water line below is where the
 * store's real depth finally shows.
 */

#include "orb_jobs.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "orb_svc.h"        /* orb_svc_note_clean_reboot: an owned reboot bills nobody */

static const char *TAG = "jobs";

/* The payload is copied by value into the queue, so a job's argument may live on
 * the poster's stack. One fixed slot per queued job keeps that copy bounded and
 * keeps the queue's own storage in static memory rather than on a heap the
 * framebuffer is competing with. */
#define ORB_JOB_ARG_MAX 64
#define ORB_JOB_Q_LEN   2

typedef struct {
    orb_job_fn fn;
    uint8_t    arg[ORB_JOB_ARG_MAX];
    size_t     arglen;
} job_t;

static QueueHandle_t     s_q;
static TaskHandle_t      s_task;
static TaskHandle_t volatile s_worker;  /* job_worker's own handle, set on entry */
static volatile bool     s_busy;
static bool              s_started;

bool orb_jobs_on_worker(void)
{
    return xTaskGetCurrentTaskHandle() == s_worker;
}

static void job_worker(void *arg)
{
    (void)arg;
    s_worker = xTaskGetCurrentTaskHandle();
    job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        /* Run with the gate still closed: a job that finishes and a second post
         * from the LVGL task must not interleave, or two walks hit one card. */
        j.fn(j.arglen ? j.arg : NULL);
        ESP_LOGI(TAG, "job done, %u B stack never used",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        s_busy = false;
    }
}

esp_err_t orb_jobs_start(void)
{
    if (s_started) return ESP_OK;
    s_q = xQueueCreate(ORB_JOB_Q_LEN, sizeof(job_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreate(job_worker, "orb_jobs", 8192, NULL, 3, &s_task) != pdPASS) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

esp_err_t orb_jobs_post(orb_job_fn fn, void *arg, size_t arglen)
{
    if (!fn) return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;
    if (arglen > ORB_JOB_ARG_MAX) {
        ESP_LOGE(TAG, "job arg %u B over the %u B slot",
                 (unsigned)arglen, (unsigned)ORB_JOB_ARG_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_busy) return ESP_ERR_INVALID_STATE;

    job_t j = { .fn = fn, .arglen = arglen };
    if (arglen) memcpy(j.arg, arg, arglen);
    s_busy = true;                       /* before the send: see job_worker */
    if (xQueueSend(s_q, &j, 0) != pdTRUE) {
        s_busy = false;
        ESP_LOGW(TAG, "job queue full — request dropped");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool orb_jobs_busy(void)
{
    return s_busy;
}

/* ---- the OS-owned reboot ---------------------------------------------------
 *
 * One 750 ms one-shot, shared by every "restart to make this take" flow. The
 * delay is not politeness: esp_restart() from inside the code that just painted
 * "installed" makes the last thing the owner ever sees the frame before it, so
 * the answer to their action is the thing that never reaches the glass. The
 * window also has to outlive the httpd response, which is why h_reboot() could
 * not simply restart inline either.
 */

static void reboot_fire(void *arg)
{
    (void)arg;
    /* C2: this reboot is the owner's, not the app's misfortune. Disarm the
     * reset record first so the boot it causes bills nobody — an owner who
     * restarts after a theme install must not hand the clock a fault. */
    orb_svc_note_clean_reboot();
    ESP_LOGW(TAG, "rebooting");
    esp_restart();
}

esp_err_t orb_jobs_reboot_soon(const char *why)
{
    static esp_timer_handle_t rt;
    if (!rt) {
        const esp_timer_create_args_t ta = { .callback = reboot_fire };
        if (esp_timer_create(&ta, &rt) != ESP_OK) {
            ESP_LOGE(TAG, "reboot timer could not be created%s%s", why ? ": " : "", why ? why : "");
            return ESP_FAIL;
        }
    }
    if (esp_timer_start_once(rt, 750 * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "reboot could not be armed%s%s", why ? ": " : "", why ? why : "");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "rebooting in 750 ms%s%s", why ? ": " : "", why ? why : "");
    return ESP_OK;
}
