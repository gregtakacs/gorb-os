/* orb_live_work.c — the live layers' worker (OS v1.2.38, radar-plan §6.27).
 *
 * Measured on Bronze (2026-11-08, radar Scope over rain): a frame's decode into the layer
 * held the radar's guest call ~117 ms, and its tick with it, so the sweep's 20 fps fell to a
 * median 9. Here the draws run on their own task, below the LVGL task, into the layer's back
 * copy; the front keeps drawing until the app presents the back. The present first has this
 * task compare back and front (LW_DIFF), so the compositor lays in only the rows the new
 * picture changed: a rain frame moves a fraction of the disc, the map under it none. */
#include "orb_live_work.h"
#include "orb_gfx.h"          /* radar-plan §7.7: LW_AHEAD */

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "clk_types.h"     /* CLK_SCR_W/H */
#include "orb_png8.h"
#include "orb_stage.h"
#include "orb_ui.h"

static const char *TAG = "livework";

#define LW_QUEUE   16
#define LW_STACK   6144
#define LW_PRIO    2                 /* under the LVGL task, as the map's renders */
/* radar-plan §7.7: the core LVGL is not on (DISPLAY_LCD_CORE 1): a decode or a swap's planes
 * built ahead take none of the frame's own core */
#define LW_CORE    0

static QueueHandle_t s_q;
static atomic_int    s_pending[4];
static atomic_bool   s_diff_ready[4];
static int16_t      *s_dx0[4], *s_dx1[4];  /* PSRAM, CLK_SCR_H each: the last diff's rows */
static void         *s_scratch;      /* the decoder's, the worker's own (orb_ui's is the LVGL task's) */

static void job_free(lw_job_t *j)
{
    heap_caps_free(j->png); j->png = NULL;
    heap_caps_free(j->pal); j->pal = NULL;
}

static void run(lw_job_t *j)
{
    uint16_t *px, *spx = NULL;
    uint8_t *al, *sa = NULL;
    const int src = j->op == LW_BEGIN ? j->src : j->op == LW_DIFF ? j->layer : -1;
    if (!stage_live_back_lock(j->layer, &px, &al, &spx, &sa, src)) return;
    const orb_ui_tgt_t t = { px, al, CLK_SCR_W, CLK_SCR_H, CLK_SCR_W };
    switch (j->op) {
    case LW_BEGIN: {
        atomic_store(&s_diff_ready[j->layer], false);  /* a diff of an abandoned picture */
        /* the disc alone, as stage_live_copy and live_fill (§6.19): the corners are never seen */
        const uint16_t *x0 = orb_ui_disc_left();
        const uint32_t c = j->argb & 0xFFFFFFu;
        const uint16_t c565 = (uint16_t)(((c >> 8) & 0xF800u) | ((c >> 5) & 0x07E0u) | ((c >> 3) & 0x001Fu));
        for (int y = 0; y < CLK_SCR_H; y++) {
            const size_t i = (size_t)y * CLK_SCR_W + x0[y], k = (size_t)(CLK_SCR_W - 2 * x0[y]);
            if (spx) {
                memcpy(px + i, spx + i, 2 * k);
                if (al) { if (sa) memcpy(al + i, sa + i, k); else memset(al + i, 0xFF, k); }
            } else {
                for (size_t q = 0; q < k; q++) px[i + q] = c565;
                if (al) memset(al + i, (int)(j->argb >> 24), k);
            }
        }
        break;
    }
    case LW_PNG8: {
        if (!s_scratch) s_scratch = heap_caps_malloc(orb_png8_scratch_size(), MALLOC_CAP_SPIRAM);
        const int32_t r = s_scratch ? orb_ui_tgt_png8_s(&t, s_scratch, j->png, j->len, j->a[0], j->a[1], j->a[2], j->a[3],
                                                         j->a[4], j->a[5], j->a[6], j->pal)
                                    : -1;
        if (r) ESP_LOGW(TAG, "layer %d: a frame did not draw (%d)", j->layer, (int)r);
        break;
    }
    case LW_DIFF: {
        const int l = j->layer;
        if (!s_dx0[l]) s_dx0[l] = heap_caps_malloc(2 * CLK_SCR_H, MALLOC_CAP_SPIRAM);
        if (!s_dx1[l]) s_dx1[l] = heap_caps_malloc(2 * CLK_SCR_H, MALLOC_CAP_SPIRAM);
        if (!s_dx0[l] || !s_dx1[l]) { atomic_store(&s_diff_ready[l], true); break; }   /* NULL rows: the whole disc */
        const uint16_t *x0 = orb_ui_disc_left();
        for (int y = 0; y < CLK_SCR_H; y++) {
            const int a = x0[y], b = CLK_SCR_W - 1 - x0[y];
            const size_t i = (size_t)y * CLK_SCR_W;
            int lo = a, hi = b;
            if (spx) {
                /* first and last pixel that differ, colour or (unmerged) alpha */
                while (lo <= b && px[i + lo] == spx[i + lo] && (!al || !sa || al[i + lo] == sa[i + lo])) lo++;
                while (hi >= lo && px[i + hi] == spx[i + hi] && (!al || !sa || al[i + hi] == sa[i + hi])) hi--;
            }
            s_dx0[l][y] = (int16_t)lo;
            s_dx1[l][y] = (int16_t)hi;
        }
        atomic_store(&s_diff_ready[l], true);
        break;
    }
    case LW_AHEAD: gfx_prep_ahead_run(); break;      /* reads the back under its lock */
    case LW_RING: orb_ui_tgt_ring(&t, j->a[0], j->a[1], j->a[2], j->argb); break;
    case LW_LINE: orb_ui_tgt_line(&t, j->a[0], j->a[1], j->a[2], j->a[3], j->argb); break;
    default: break;
    }
    stage_live_back_unlock();
}

static void task(void *arg)
{
    (void)arg;
    lw_job_t j;
    for (;;) {
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(30000)) != pdTRUE) {
            heap_caps_free(s_scratch);                    /* idle 30 s: the scratch back */
            s_scratch = NULL;
            continue;
        }
        run(&j);
        job_free(&j);
        if (j.layer < 4) atomic_fetch_sub(&s_pending[j.layer], 1);
    }
}

static bool started(void)
{
    if (s_q) return true;
    /* the queue's 16 jobs in PSRAM too (~900 B): only its control block is internal */
    static StaticQueue_t qcb;
    static uint8_t *qbuf;                         /* kept across a failed start, reused by the next */
    if (!qbuf) qbuf = heap_caps_malloc(LW_QUEUE * sizeof(lw_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_q = qbuf ? xQueueCreateStatic(LW_QUEUE, sizeof(lw_job_t), qbuf, &qcb) : NULL;
    if (!s_q) return false;
    /* The stack in PSRAM, as the map task's: an internal stack takes the block the Wi-Fi
     * driver's transmit buffers come from (gorb-os-internal-ram-under-clock). */
    StackType_t *stk = heap_caps_malloc(LW_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = heap_caps_calloc(1, sizeof *tcb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stk || !tcb || !xTaskCreateStaticPinnedToCore(task, "livework", LW_STACK, NULL, LW_PRIO, stk, tcb, LW_CORE)) {
        heap_caps_free(stk);
        heap_caps_free(tcb);
        vQueueDelete(s_q);
        s_q = NULL;
        return false;
    }
    return true;
}

esp_err_t orb_live_work_post(lw_job_t *j)
{
    if (j->layer >= 4 || !started()) { job_free(j); return ESP_ERR_NO_MEM; }
    atomic_fetch_add(&s_pending[j->layer], 1);
    if (xQueueSend(s_q, j, pdMS_TO_TICKS(200)) != pdTRUE) {
        atomic_fetch_sub(&s_pending[j->layer], 1);
        job_free(j);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

int orb_live_work_diff(int l, const int16_t **x0, const int16_t **x1)
{
    if (l < 0 || l >= 4 || !atomic_exchange(&s_diff_ready[l], false)) return 0;
    *x0 = s_dx0[l] && s_dx1[l] ? s_dx0[l] : NULL;
    *x1 = s_dx0[l] && s_dx1[l] ? s_dx1[l] : NULL;
    return 1;
}

/* The comparison's rows without taking them (radar-plan §7.7: the swap's build-ahead reads
 * them while the present is still paced). Valid until the next comparison. */
int orb_live_work_diff_peek(int l, const int16_t **x0, const int16_t **x1)
{
    if (l < 0 || l >= 4 || !atomic_load(&s_diff_ready[l]) || !s_dx0[l] || !s_dx1[l]) return 0;
    *x0 = s_dx0[l];
    *x1 = s_dx1[l];
    return 1;
}

bool orb_live_work_diff_ready(int l)
{
    return l >= 0 && l < 4 && atomic_load(&s_diff_ready[l]);
}

int orb_live_work_pending(int l)
{
    return l >= 0 && l < 4 ? atomic_load(&s_pending[l]) : 0;
}
