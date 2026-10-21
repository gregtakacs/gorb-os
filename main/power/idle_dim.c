/**
 * idle_dim.c -- the ten-minute dim and its two wake sources.
 *
 * One task, two poll rates, one decision. Awake it ticks at 1 Hz: the knob
 * already reports every touch, so the fastest question left is whether ten
 * minutes have passed, and that does not need a faster clock. Dimmed it ticks at
 * 20 Hz, because the accelerometer is then the only way a person gets their dial
 * back and a knock on the case is over in a few tens of milliseconds. The
 * accelerometer is read on both ticks -- awake, only to record the deviation
 * high-water the threshold is calibrated from, never to act on it.
 *
 * Why the level is restored to a snapshot rather than to 100: see idle_dim.h.
 * The practical consequence is that this feature stays invisible until the
 * timeout actually fires -- the panel keeps whatever its own init left in
 * register 0x51, and nothing here touches it before then.
 */
#include "idle_dim.h"
#include "imu.h"
#include "display.h"
#include "board_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"   /* the render-task handoff that carries the brightness write */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_co5300.h"
#include "nvs.h"
#include "nvs_flash.h"   /* nvs_flash_init_partition: see settings_load */
#include "sdkconfig.h"
#include <stdio.h>       /* the holder-name copy in idle_dim_app_request */
#include <string.h>      /* the holder check in idle_dim_app_release */

/* The two clocks, per the argument in the header. */
#define TICK_MS_LIGHT   1000
#define TICK_MS_DARK      50

/* The decision task's priority. It outranks the LVGL task (5) on purpose.
 *
 * The clock keeps animating while dimmed — dimming never parks it, so a dial is
 * as live in the dark as in the light, and a dial whose owner turned dimming off
 * is animating 24/7. Both the animation tick and the flush's TE wait run on the
 * priority-5 LVGL task, and that TE wait is a busy spin with no yield. The task
 * that DECIDES to lift the face after a touch is this one, and it must preempt
 * that spin to take its dark 20 Hz IMU poll and its wake notice; at the system
 * idle priority it could not get a slice while the animation spun, so a touch
 * arrived (the poll stamps it on the LVGL task) but the decision to wake lagged
 * however long the renderer kept the CPU — the "wakes late, like it never wakes"
 * symptom, and it would not go away by pausing the animation because the owner
 * may have paused nothing. Above the renderer, the decision preempts the spin on
 * the next dark tick regardless of animation load. It then posts the brightness
 * target and blocks on the ack (see apply_level), so it never spends its priority
 * spinning, and the actual write runs back on the render task — this task no
 * longer touches the QSPI bus at all, which is what lets it keep this priority
 * without the frozen-dial the direct write caused.
 *
 * 6, not 5: equal priority shares the slice round-robin and a flush in progress
 * still defers the decision to the next tick. */
#define IDLE_DIM_TASK_PRIO 6

bool idle_motion_decide(idle_motion_t *st, const int16_t s[3], int16_t thresh_lsb)
{
    /* Outside the feature switch: this is the part worth testing on a host, and
     * a test must be able to link it whether or not the dim is compiled in. */
    if (!st) return false;
    if (!st->have_base) {                     /* seed now, judge the NEXT sample */
        for (int i = 0; i < 3; i++) st->base[i] = s[i];
        st->worst     = 0;
        st->have_base = true;
        return false;
    }
    int32_t worst = 0;
    for (int i = 0; i < 3; i++) {
        const int32_t d = (int32_t)s[i] - st->base[i];
        const int32_t a = d < 0 ? -d : d;
        if (a > worst) worst = a;
    }
    /* Kept for the bench: the honest way to set ORB_IDLE_MOTION_MG is above the
     * still-reading figure and below a pick-up, and nothing but this counter can
     * tell you where the still-reading figure actually is on this board. */
    if (worst > st->worst) st->worst = worst;
    /* Judged against the baseline BEFORE it is moved, so the follow below cannot
     * absorb the very motion it is meant to report. */
    const bool motion = worst > (int32_t)thresh_lsb;
    /* Follow on EVERY sample, motion included. Skipping it while moving is the
     * tempting version and it is broken: hold the orb at a new angle and the lag
     * never decays, so the detector latches MOTION forever and the dial, being
     * "touched" every tick, never dims again. Measured at the 50 ms dark poll
     * (1/8 gives a 374 ms time constant): a tilt held at 160 mg reads motion for
     * 12 samples then goes still, 400 mg for 19, 800 mg for 24 -- under a second
     * for a real move -- while a continuing shake reads motion on every sample, so
     * the orb being carried stays lit. Pinned by tools/host_idle_dim_test.py. */
    for (int i = 0; i < 3; i++)
        st->base[i] += ((int32_t)s[i] - st->base[i]) >> 3;
    return motion;
}

bool idle_motion_wake_decide(idle_wake_t *st, bool motion, uint8_t need)
{
    if (!st) return false;
    if (!motion) {                    /* one still sample is enough to end a run */
        st->run = 0;
        st->fired = false;            /* re-arm: the next run must be able to fire */
        return false;
    }
    if (st->run < 255) st->run++;   /* saturate, do not wrap: the task stamps its
                                     * idle clock from run >= need, so a run that
                                     * wrapped back through 0 would stop holding
                                     * the face lit for a sample or two in the
                                     * middle of somebody carrying the orb. Pinned
                                     * by the held-lit count of long_run_wakes_once.
                                     */
    if (need < 1) need = 1;
    if (st->run >= need && !st->fired) {
        st->fired = true;
        return true;
    }
    return false;
}

uint32_t idle_dim_clamp_timeout_s(uint32_t s)
{
    if (s < IDLE_DIM_TIMEOUT_MIN_S) return IDLE_DIM_TIMEOUT_MIN_S;
    if (s > IDLE_DIM_TIMEOUT_MAX_S) return IDLE_DIM_TIMEOUT_MAX_S;
    return s;
}

bool idle_dim_elapsed_decide(uint32_t now_ms, uint32_t last_ms,
                             uint32_t timeout_ms, bool enabled)
{
    if (!enabled) return false;
    /* Unsigned subtraction only: a wrap of the 49-day esp_timer counter reads
     * as a big idle time, never a negative one, so the wrap cannot postpone the
     * dim nor cause an early one. Pinned by tools/host_idle_dim_test.py. */
    return (uint32_t)(now_ms - last_ms) >= timeout_ms;
}

bool idle_dim_app_decide(uint32_t now_ms, uint32_t last_ms,
                         uint32_t timeout_ms, bool enabled,
                         bool app_hold, uint32_t app_until_ms)
{
    /* The owner's switch outranks every app (§4.2): with dim off the face is
     * lit anyway and no app ask changes that, in either direction. */
    if (!enabled) return false;
    if (app_hold) return false;
    /* An open window: until_ms was stamped at request time as now + a
     * clamped span, so "still open" means until lies within the ceiling's
     * reach AHEAD of now — the same unsigned-subtraction shape the elapsed
     * test uses, and for the same reason: a wrap of the 49-day clock must
     * read as closed (expired), never as an open window. A zero is the
     * no-request sentinel, and the Kconfig range (<= 3600 s) keeps the
     * ceiling a millisecond count well under 2^31, so the test cannot alias. */
    if (app_until_ms != 0 &&
        (uint32_t)(app_until_ms - now_ms) <
            (uint32_t)CONFIG_ORB_APP_POWER_MAX_S * 1000u)
        return false;
    return idle_dim_elapsed_decide(now_ms, last_ms, timeout_ms, enabled);
}

bool idle_dim_level_apply_decide(bool want, bool dimmed, bool wrote)
{
    /* What the task is allowed to believe about the panel after ONE attempt at
     * it: the new level if the write landed, the old one if it did not.
     *
     * Committing `want` before the write is answered is the bug this pins, and
     * it is a dial that "no longer wakes from the dimmed state". The brightness
     * level is one QSPI write to the panel's 0x51; like every other write to
     * that bus it can be refused, and the restore is the one transaction with
     * nobody waiting for the answer. Believe the intent anyway and the flag says
     * "lit", the glass says 20%, and no further tick ever differs from the flag
     * -- so the write is never retried and the dial sits dark with a clock
     * running behind it until the next timeout, which needs a touch to reach.
     * The same lost write also strands the second wake source: `per` is chosen
     * from this flag, so a falsely-lit task drops to the 1 Hz tick and the 20 Hz
     * dark poll that reads the accelerometer stops with it.
     *
     * Keeping the old value makes the next tick ask again, at the rate of the
     * state the panel is actually in: a lost wake leaves the task dark and
     * retrying every 50 ms, and a lost dim leaves it lit and retrying every
     * second. Both need no extra machinery to converge. Pinned by
     * tools/host_idle_dim_test.py. */
    return wrote ? want : dimmed;
}

#if CONFIG_ORB_IDLE_DIM_ENABLE

/* Inside the guard, not above it: every ESP_LOGx that uses it is compiled out
 * with the feature, and a file-scope TAG would then trip -Wunused-variable. */
static const char *TAG = "idle_dim";

static volatile uint32_t s_last_ms;           /* last touch, from any source     */
static volatile uint32_t s_user_pct = 100;    /* where the dial sits while awake */
static volatile bool     s_dimmed;
/* The wake task's handle, so a knob touch on the LVGL task can pull it off its
 * dark-tick sleep and restore the face now (ulTaskNotifyTake below) rather than
 * after the remaining 50 ms. Written once in idle_dim_init before the task can
 * be nudged; read only under `s_dimmed`, when a restore is actually pending. */
static TaskHandle_t      s_idle_task;
#if CONFIG_ORB_IDLE_MOTION_ENABLE
static volatile uint32_t s_wakes;             /* motion-credited wakes, since boot */
static volatile int32_t  s_worst_lsb;         /* boot-wide deviation high-water  */
static volatile int32_t  s_worst_dark_lsb;    /* same, from the 20 Hz dark poll only */
#endif
static volatile uint32_t s_dim_at_ms;         /* when the dim was applied, for the
                                               * wake log: tells a wake the dim
                                               * caused from one the desk did    */
static volatile uint32_t s_act_calls;         /* stamps claimed by any source    */
static volatile uint32_t s_act_folded;        /* stamps hidden by the log limit  */
static volatile uint32_t s_act_log_ms;        /* last touch line, for the rate cap */
static volatile uint32_t s_level_writes_failed; /* brightness writes the panel did
                                                 * not take: every one of them is
                                                 * a retry the next tick owes the
                                                 * owner, and the count on
                                                 * /api/v1/status is what says the
                                                 * dial went dark for a reason */

/* ---- the DISPLAY menu's two settings, persisted --------------------------
 * The Kconfig constants above seed these on a device that has never been
 * configured from the dial; the first menu choice writes NVS and from then on
 * NVS wins at boot. A getter is one 32-bit read, so the LVGL repaint and
 * h_status can ask without a lock; the setters are worker-task calls because
 * nvs_commit can hit a sector erase.
 *
 * Same partition and lazy pattern as orb_theme.c's settings, not orb_net.c's:
 * idle_dim_init() runs before orb_net boots the `storage` partition, so this
 * module must not treat an unopened partition as fatal (orb_theme.c's argument
 * about who owns that partition applies verbatim), and it must never erase. */
#define DIM_NVS_PARTITION "storage"
#define DIM_NVS_NAMESPACE "orb.idle"
#define DIM_NVS_KEY_EN    "en"      /* u32: 0/1; absent = CONFIG default       */
#define DIM_NVS_KEY_TO    "to"      /* u32: timeout seconds; absent = CONFIG   */

static nvs_handle_t  s_nvs;                   /* 0 = open failed; see settings_nvs() */
static volatile bool     s_enabled;           /* the switch, as last set            */
static volatile uint32_t s_timeout_ms;        /* timeout * 1000, pre-multiplied     */

/* The app voice (§4.2): the live hold flag, the far edge of the last timeout
 * request, and who asked. Every field is a single volatile word read by the
 * policy task and written from the broker on the LVGL task — the same
 * one-word-per-fact discipline as s_enabled above; the holder string is for
 * the status line only, and a half-copied name there is cosmetic at worst. */
static volatile bool     s_app_hold;
static volatile uint32_t s_app_until_ms;
static char              s_app_holder[24];     /* slug cap, plan §4.1 */

/* Open on first use, like orb_theme.c's theme_nvs(): by the time the menu
 * posts its first save the worker that boots `storage` has run, and an early
 * failure costs persistence, not behaviour. The read path (settings_load) has
 * no such grace -- it runs in idle_dim_init before anyone touched that
 * partition -- which is exactly why the values it could not read keep their
 * Kconfig seeds instead of erroring. */
static nvs_handle_t settings_nvs(void)
{
    if (!s_nvs) {
        if (nvs_open_from_partition(DIM_NVS_PARTITION, DIM_NVS_NAMESPACE,
                                    NVS_READWRITE, &s_nvs) != ESP_OK) {
            ESP_LOGE(TAG, "no NVS for display settings -- changes will not "
                          "survive a reboot");
            s_nvs = 0;
            return 0;
        }
    }
    return s_nvs;
}

static esp_err_t settings_persist(const char *key, uint32_t v)
{
    const nvs_handle_t h = settings_nvs();
    if (!h) return ESP_ERR_INVALID_STATE;
    const esp_err_t e = nvs_set_u32(h, key, v);
    if (e != ESP_OK) return e;
    return nvs_commit(h);                     /* durability, not correctness: the
                                               * RAM value already took effect   */
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* A refused brightness write is now something the task retries instead of
 * believing, so it is also something the task says out loud once per second at
 * most -- the same cap the touch line uses. A panel that has stopped answering
 * is refused at the dark poll's 20 Hz, and a serial line that floods is the
 * reason the next symptom cannot be seen; the count on /api/v1/status is the
 * number that matters, and it is not rate limited. */
static volatile uint32_t s_level_fail_log_ms;
static volatile uint32_t s_level_fail_folded;

static void level_fail_log(const char *why, uint8_t percent, const char *err)
{
    const uint32_t now = now_ms();
    if ((uint32_t)(now - s_level_fail_log_ms) < 1000u) {
        s_level_fail_folded++;
        return;
    }
    s_level_fail_log_ms = now;
    ESP_LOGE(TAG, "%s (%u%%) failed: %s -- retrying; %u refused since boot, %u "
                  "folded into this line", why, (unsigned)percent, err,
             (unsigned)s_level_writes_failed, (unsigned)s_level_fail_folded);
    s_level_fail_folded = 0;
}

/* The brightness write, marshalled to the LVGL task.
 *
 * This used to be the one function idle_task called to issue the QSPI 0x51 write
 * itself, and the safety of that relied entirely on the task's priority: at the
 * system idle priority it physically could not run while the renderer held the
 * bus, so the write only ever landed at a quiescent point. Raising this task
 * above the renderer (see IDLE_DIM_TASK_PRIO) to win the fast wake broke exactly
 * that guarantee -- the write then landed inside a frame flush. A single
 * draw_bitmap is three bus acquisitions (CASET, RASET, then an async RAMWR), and
 * a higher task slipping a 0x51 between them desyncs the in-flight bookkeeping:
 * the last band's trans-done no longer balances s_band_inflight, LVGL never
 * re-arms a flush, and the dial freezes with a live clock behind it. The write
 * returns ESP_OK throughout -- so writes_failed stays 0 and the wedge is
 * invisible on /api/v1/status -- which is why placement, not the transaction,
 * was the bug.
 *
 * The fix keeps the fast wake by moving only the WRITE off this task, never the
 * decision. idle_task still decides when and to what level; it records that
 * here and blocks until the LVGL task -- the one task the flush runs on, so the
 * same-task sequencing makes a mid-transaction write impossible -- performs it at
 * the top of its own loop. The ack is what lets the caller still "ask the panel
 * first, believe it second": this returns the panel's real answer, so a write the
 * render task never drained within the timeout reads as a lost write the next
 * tick retries, exactly like a refused transaction. */
static SemaphoreHandle_t s_lvl_ack;            /* render task -> here, per request */
static volatile bool     s_lvl_pending;        /* a target is set, not yet written */
static volatile uint8_t  s_lvl_target;         /* the percent the pending write is for */
static volatile esp_err_t s_lvl_result;        /* what the panel said, read after ack */
static uint32_t          s_lvl_seq;            /* author's request counter, not volatile */
static volatile uint32_t s_lvl_ack_seq;        /* the seq the render task just answered */

static bool apply_level(uint8_t percent, const char *why)
{
    if (!display_get_panel()) {
        /* Counted as a lost write, not warned about and forgiven: "no panel yet"
         * and "the panel refused" reach the owner as the same dark dial, and the
         * retry is the answer to both. Fast-pathed here rather than bounced to
         * the render task, which would only answer the same way. */
        s_level_writes_failed++;
        level_fail_log(why, percent, "no panel yet");
        return false;
    }
    if (!s_lvl_ack) {
        /* Pre-init (idle_dim_init's first apply runs before the semaphore and the
         * LVGL task exist): nothing can drain this, so treat it as a lost write
         * the next tick retries rather than blocking forever on an ack nobody
         * will give. Boot defaults leave the panel at its own init level anyway,
         * so no real dim is owed this early. */
        s_level_writes_failed++;
        level_fail_log(why, percent, "no renderer yet");
        return false;
    }

    const uint32_t seq = ++s_lvl_seq;
    s_lvl_target  = percent;
    s_lvl_pending = true;
    /* Flush any ack a previous request left behind by timing out and then being
     * drained late, so this wait cannot consume an ack that is not for `seq`. */
    while (xSemaphoreTake(s_lvl_ack, 0) == pdTRUE) { }
    /* One request at a time: this task is its only author. Bounded wait so a
     * render task wedged by anything ELSE -- not ours to paper over -- degrades
     * to a retried lost write, never a dim task that hangs on the ack. */
    if (xSemaphoreTake(s_lvl_ack, pdMS_TO_TICKS(500)) != pdTRUE) {
        s_lvl_pending = false;
        s_level_writes_failed++;
        level_fail_log(why, percent, "render task never drained");
        return false;
    }
    /* An ack for a different seq belongs to a request that already timed out;
     * believing it would let one wedged frame's write be mistaken for this one's.
     * Treat it as a lost write -- the safe direction, retried next tick. */
    const esp_err_t err = s_lvl_result;
    if (s_lvl_ack_seq != seq || err != ESP_OK) {
        s_level_writes_failed++;
        level_fail_log(why, percent,
                       s_lvl_ack_seq != seq ? "stale ack" : esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "%u%% -- %s", (unsigned)percent, why);
    return true;
}

/* Run the pending brightness write, from the LVGL task only, between frames.
 * Called every loop iteration of lvgl_task before lv_timer_handler(), where no
 * flush is in flight and the same-task rule makes a mid-transaction write
 * impossible. Cheap no-op when idle_task posted nothing. Public so main.c's
 * renderer can call it; the disabled build stubs it to the same no-op. */
void idle_dim_render_drain(void)
{
    if (!s_lvl_pending)
        return;
    /* Read the author's seq and target together, then clear pending BEFORE the
     * write, so a higher-priority idle_task resuming cannot post a new request
     * that this frame would then clobber or double-answer. */
    const uint32_t  seq     = s_lvl_seq;
    const uint8_t   percent = s_lvl_target;
    s_lvl_pending = false;
    esp_lcd_panel_handle_t panel = display_get_panel();
    s_lvl_result = panel ? esp_lcd_panel_co5300_set_brightness(panel, percent)
                         : ESP_ERR_INVALID_STATE;
    s_lvl_ack_seq = seq;                    /* published before the ack it rides */
    xSemaphoreGive(s_lvl_ack);
}

static void idle_task(void *arg)
{
    (void)arg;
    TickType_t     per = pdMS_TO_TICKS(TICK_MS_LIGHT);
#if CONFIG_ORB_IDLE_MOTION_ENABLE
    idle_motion_t  motion = { .have_base = false };
    idle_wake_t    wake   = { .run = 0, .fired = false };
    /* How many consecutive tripping samples a wake needs. See the argument at
     * idle_motion_wake_decide: one sample is a desk jolt, a dozen is a hand. */
    const uint8_t  need   = (uint8_t)CONFIG_ORB_IDLE_MOTION_SAMPLES;
    /* Threshold in LSB at the ±8 g / 4096 LSB-per-g scale imu.c configures. 40 mg
     * is a starting point, not a measured figure -- see ORB_IDLE_MOTION_MG, whose
     * help says how to settle it on the bench. */
    const int16_t  thresh = (int16_t)((CONFIG_ORB_IDLE_MOTION_MG * IMU_LSB_PER_G_8G)
                                      / 1000);
#endif

    for (;;) {
        /* Block up to `per`, but wake early if idle_dim_activity nudged us (a
         * knob touch while dimmed): pdTRUE clears any accumulated count on
         * exit, so a burst of turns coalesces into the one restore that follows.
         * This is why the task can now sit at a high priority and still cost no
         * frame budget — it spends that priority blocked, not spinning. */
        (void)ulTaskNotifyTake(pdTRUE, per);
        const uint32_t now = now_ms();

        /* One accelerometer reading every tick, in both states -- but the verdict
         * is acted on only in the dark. Awake, the knob already reports a touch,
         * and acting on motion too would let a desk fan hold the face lit forever.
         * Sampling anyway is what makes idle_dim_worst_mg() worth reading: the orb
         * is LIT while a hand shakes it on the bench, and a read confined to the
         * dark would leave the one number the threshold is set from sitting at
         * zero exactly when it is needed. Awake this costs one 6-byte burst read a
         * second, on a bus the frame flush dwarfs. */
#if CONFIG_ORB_IDLE_MOTION_ENABLE
        int16_t a[3];
        if (imu_read_accel(a) == ESP_OK) {
            const bool trip = idle_motion_decide(&motion, a, thresh);
            if (idle_motion_wake_decide(&wake, trip, need) && s_dimmed) {
                /* The log the whole diagnosis needs: a motion wake that nobody
                 * caused is the failure the owner sees as "it never stays dimmed",
                 * and from the outside it is indistinguishable from a timer that
                 * does not work. Which axis tripped, by how much, and how long
                 * after the dim it happened -- that last one separates a wake the
                 * dim caused itself (the 0x51 write, sub-100 ms) from a vibration
                 * the desk is feeding the case (arbitrary delay: measured 3.0 s
                 * and 26.6 s apart on two runs of the same untouched orb). */
                s_wakes++;
                ESP_LOGI(TAG, "motion wake %u: x %d y %d z %d | %u ms after "
                              "the dim | baseline %d/%d/%d | thresh %d mg",
                         (unsigned)s_wakes, a[0], a[1], a[2],
                         (unsigned)(now - s_dim_at_ms),
                         (int)motion.base[0], (int)motion.base[1],
                         (int)motion.base[2],
                         (int)(motion.worst * 1000 / IMU_LSB_PER_G_8G));
            }
            /* Held motion must keep refreshing the clock, or an orb carried across
             * a room dims on the way: the one-shot above reports the wake, and this
             * is what holds the face lit for the rest of the run. Gating the stamp
             * on the one-shot instead would have made the dial dim mid-walk, which
             * is the behaviour tools/host_idle_dim_test.py pins with its kept count.
             * Judged on the run, so a single-sample desk jolt stamps nothing. */
            if (s_dimmed && wake.run >= need)
                s_last_ms = now;
            /* Two high-waters, not one. Awake this task sees one sample a second
             * and dimmed it sees twenty, so the lit figure UNDER-reports the
             * spikes that matter -- the first bench reading was 7 mg lit and 62 mg
             * within seconds of the dark, which is the whole reason a threshold
             * set from the lit number alone dims the dial and immediately un-dims
             * it. Settle the threshold against the dark figure. */
            if (motion.worst > s_worst_lsb) s_worst_lsb = motion.worst;
            if (s_dimmed && motion.worst > s_worst_dark_lsb)
                s_worst_dark_lsb = motion.worst;
        }
#endif

        /* The two settings live in RAM the DISPLAY menu can change at any
         * moment, so both are read per tick, not cached across the loop; the
         * decision itself is the host-pinned idle_dim_app_decide, which folds
         * the app's third voice around the elapsed test. */
        const bool want = idle_dim_app_decide(now, s_last_ms,
                                              s_timeout_ms, s_enabled,
                                              s_app_hold, s_app_until_ms);
        if (want != s_dimmed) {
            /* Ask the panel first, believe it second. The write is the fact; the
             * flag is a claim about it, and a claim the next tick cannot retry is
             * how one refused transaction leaves a dark dial for ten minutes. See
             * idle_dim_level_apply_decide for the whole argument. */
            const bool wrote = apply_level(
                (uint8_t)(want ? CONFIG_ORB_IDLE_DIM_PCT : s_user_pct),
                want ? "idle dim" : "touch, back to the user's level");
            if (wrote && want) {
                s_dim_at_ms = now;            /* stamped only once the glass is
                                               * actually dark: the delay the wake
                                               * log reports is measured from the
                                               * dim that caused it             */
#if CONFIG_ORB_IDLE_MOTION_ENABLE
                motion.have_base = false;   /* re-seed: the orb may have moved
                                              * while it was lit and awake        */
                /* Reset the run too: it accumulates on the lit tick as well, so
                 * without this the face could arrive in the dark already "fired"
                 * and the first genuine pick-up would be swallowed. */
                wake.run = 0;
                wake.fired = false;

#endif
            }
            s_dimmed = idle_dim_level_apply_decide(want, s_dimmed, wrote);
        }
        per = pdMS_TO_TICKS(s_dimmed ? TICK_MS_DARK : TICK_MS_LIGHT);
    }
}

/* Seed the two settings for the task: Kconfig first, then NVS over the top if
 * the namespace is readable and holds a value.
 *
 * The init_partition call is load-bearing, not ceremony: idle_dim_init runs
 * before orb_net's net_open_storage(), and nvs_open_from_partition on a
 * partition nobody has initialized yet just returns NOT_INITIALIZED -- which is
 * how orb_theme.c silently lands its boot-time settings in the default
 * partition instead. Init is idempotent, so net's later call is unaffected.
 * What must never appear here is the NO_FREE_PAGES recovery erase net does:
 * this partition holds the owner's WiFi credentials too, and a settings read
 * has no business destroying them. Corrupt is treated exactly like empty --
 * the Kconfig seeds stand. */
static void settings_load(void)
{
    s_enabled    = CONFIG_ORB_IDLE_DIM_ENABLE;   /* only reachable when y */
    s_timeout_ms = (uint32_t)CONFIG_ORB_IDLE_DIM_TIMEOUT_S * 1000u;

    if (nvs_flash_init_partition(DIM_NVS_PARTITION) != ESP_OK)
        return;                                  /* absent or corrupt: defaults
                                                  * stand, nothing to log beyond
                                                  * net's own complaint later   */
    nvs_handle_t h = 0;
    if (nvs_open_from_partition(DIM_NVS_PARTITION, DIM_NVS_NAMESPACE,
                                NVS_READONLY, &h) != ESP_OK)
        return;                                  /* namespace never written:
                                                  * first boot -- defaults stand   */
    uint32_t v = 0;
    if (nvs_get_u32(h, DIM_NVS_KEY_EN, &v) == ESP_OK)
        s_enabled = (v != 0);
    if (nvs_get_u32(h, DIM_NVS_KEY_TO, &v) == ESP_OK)
        s_timeout_ms = idle_dim_clamp_timeout_s(v) * 1000u;   /* clamp on READ,
                                                              * too: a hand-
                                                              * flashed or older-
                                                              * firmware value is
                                                              * not trusted into
                                                              * the task         */
    nvs_close(h);                                /* read handle only; the
                                                  * setters open their own     */
}

esp_err_t idle_dim_init(void)
{
    s_last_ms = now_ms();
    settings_load();
    const char *wake = "knob only";
#if CONFIG_ORB_IDLE_MOTION_ENABLE
    /* An absent IMU is survivable on purpose: imu_init() reports it without
     * failing, and a dial with no motion wake is still a dial with an idle dim --
     * the knob remains, which is the source in use most. With the option itself
     * off, nothing here references imu.c at all, which is what makes
     * ORB_IDLE_MOTION_ENABLE the A/B control its Kconfig help promises. */
    if (imu_present()) wake = "knob + motion";
    else ESP_LOGW(TAG, "no IMU: idle dim will wake on the rotary only");
#endif
    /* The brightness handoff (see apply_level): created before both the task and
     * the render drain can use it. idle_dim_init runs before main.c starts the
     * LVGL task, so a render drain that finds a pending request here always has
     * a semaphore to ack against; apply_level refuses the write if it is not yet
     * there, which only happens before this line has run. */
    s_lvl_ack = xSemaphoreCreateBinary();
    if (!s_lvl_ack)
        return ESP_ERR_NO_MEM;
    if (xTaskCreate(idle_task, "idle_dim", 3072, NULL, IDLE_DIM_TASK_PRIO,
                    &s_idle_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    /* The effective values, not the Kconfig ones: after the first menu change
     * they are different facts, and this is the line the bench reads to confirm
     * a saved setting actually landed. Off is a visible state here too -- the
     * task runs and the switch can be turned back on. */
    ESP_LOGI(TAG, "%s: dim to %u%% after %u s (%s)",
             s_enabled ? "armed" : "idle dim off (menu setting)",
             (unsigned)CONFIG_ORB_IDLE_DIM_PCT,
             (unsigned)(s_timeout_ms / 1000u), wake);
    return ESP_OK;
}

void idle_dim_activity(const char *why)
{
    const uint32_t now = now_ms();
    s_last_ms  = now;
    s_act_calls++;
    /* A touch while dimmed must lift the face now, not on the next dark tick.
     * The restore decision stays on the task — it is the only thing that ever
     * moves the level — and the write it triggers is drained on the render task;
     * this only shortens how long the task waits to run, so a knob turn reads as
     * an instant wake however busy the animation keeps the renderer.
     * Only when dimmed: awake there is nothing to restore, and nudging on every
     * stamp would pull a higher-priority task ahead of the frames for nothing. */
    if (s_dimmed && s_idle_task)
        xTaskNotifyGive(s_idle_task);
    /* One line per second at most, and the count of what was dropped: a knob
     * bouncing at tens of Hz must not turn the serial line into the reason the
     * next symptom cannot be seen, while still saying how often it fired. */
    if ((uint32_t)(now - s_act_log_ms) >= 1000u) {
        s_act_log_ms = now;
        ESP_LOGI(TAG, "touch (%s): %u stamps since boot, %u folded into this line",
                 why ? why : "?", (unsigned)s_act_calls, (unsigned)s_act_folded);
        s_act_folded = 0;
    } else {
        s_act_folded++;
    }
}

uint32_t idle_dim_activity_count(void)
{
    return s_act_calls;
}

uint32_t idle_dim_level_write_fail_count(void)
{
    /* The difference between "it is dimmed and the touch did not land here" and
     * "the panel never took the restore". Without it the retry is invisible from
     * the bench, and a dial that wakes late reads exactly like a dial that does
     * not wake at all. */
    return s_level_writes_failed;
}

void idle_dim_set_user_level(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_user_pct = percent;
    /* Asking for a level IS touching the orb, and it must be answered even from
     * the dark -- otherwise the control looks dead for ten minutes. */
    s_last_ms = now_ms();
}

/* ---- the DISPLAY menu's settings ------------------------------------------
 * Contract with menu_view.c: getters only read RAM (safe on the LVGL task,
 * which repaints the rows); setters are called from the menu worker, never
 * from the LVGL task or the knob poll, because nvs_commit can hit a sector
 * erase. A setter applies the RAM value even when the NVS write fails: the
 * owner chose it, the dial obeys it now, and the returned code is what lets
 * the menu say "kept until reboot" honestly instead of claiming a save that
 * did not happen (same durability/correctness split as orb_net's tz write). */

bool idle_dim_enabled(void)
{
    return s_enabled;
}

esp_err_t idle_dim_set_enabled(bool on)
{
    s_enabled = on;
    /* The owner's DISPLAY choice outranks any app ask (§4.2:375 — "the owner
     * turns the dim back on" IS the release): a hold that outlived the
     * switch's use would be an app owning the power state by the back door. */
    s_app_hold = false;
    s_app_until_ms = 0;
    s_app_holder[0] = '\0';
    /* Setting from the menu IS a touch, stamped here rather than leaning on
     * input_router's stamp of the detent that opened the row: enabling after a
     * long off-period would otherwise hand the task a stale s_last_ms and dim
     * the face the instant the switch flips, which reads as the switch
     * misfiring. And while dimmed, this stamp is what lifts the face on the
     * task's next dark tick (50 ms) -- the restore stays the task's decision,
     * and the write it posts is drained on the render task per apply_level. */
    s_last_ms = now_ms();
    return settings_persist(DIM_NVS_KEY_EN, on ? 1u : 0u);
}

uint32_t idle_dim_timeout_s(void)
{
    return s_timeout_ms / 1000u;
}

esp_err_t idle_dim_set_timeout_s(uint32_t seconds)
{
    const uint32_t s = idle_dim_clamp_timeout_s(seconds);
    s_timeout_ms = s * 1000u;
    /* Shortening the timeout below the current countdown must not have to wait
     * out the old one: stamping is what the touch path does, and a menu choice
     * is a touch. */
    s_last_ms = now_ms();
    /* And the owner picking a timeout voids any app's longer ask — same
     * third-input rule as the switch above: the app asked, the owner chose. */
    s_app_hold = false;
    s_app_until_ms = 0;
    s_app_holder[0] = '\0';
    return settings_persist(DIM_NVS_KEY_TO, s);
}

/* ---- the app voice: the broker's two doors (see idle_dim.h) ----------------
 * Both run on the LVGL task through orb_svc.c and touch only single-word
 * volatile state, never NVS and never the brightness register — the task
 * stays the only mover of 0x51 (its decision, drained on the render task per
 * apply_level), and an app's ask is only ever an input to its next decision. */

esp_err_t idle_dim_app_request(const char *slug, uint32_t timeout_s, bool hold)
{
    if (!hold && timeout_s == 0) return ESP_ERR_INVALID_ARG;   /* (0,false) is
                                                  * the withdraw — it goes to
                                                  * idle_dim_app_release, not
                                                  * through here */
    if (hold) {
        s_app_hold = true;
        s_app_until_ms = 0;                      /* the hold outranks a stale
                                                  * window; clearing keeps the
                                                  * status line telling who
                                                  * holds what */
    } else {
        if (timeout_s > CONFIG_ORB_APP_POWER_MAX_S)
            timeout_s = CONFIG_ORB_APP_POWER_MAX_S;   /* clamp, not refuse:
                                                  * §4.2 — the countdown may
                                                  * stretch to the ceiling
                                                  * and no further */
        const uint32_t want = now_ms() + timeout_s * 1000u;
        /* Last ask wins, so an app cannot lengthen another's window by
         * re-asking short — adopt only a strictly later edge, read with the
         * same wrap-safe subtraction the decision uses (both edges are
         * recent-by-construction, so one ceiling's span disambiguates). */
        if (s_app_until_ms == 0 ||
            (uint32_t)(want - s_app_until_ms) <
                (uint32_t)CONFIG_ORB_APP_POWER_MAX_S * 1000u)
            s_app_until_ms = want;
    }
    if (slug) snprintf(s_app_holder, sizeof s_app_holder, "%s", slug);
    return ESP_OK;
}

void idle_dim_app_release(const char *slug)
{
    /* Holder-checked: the registry revokes each app's ask as it leaves the
     * glass, and that must never cut short the app that is actually on it. */
    if (slug && s_app_holder[0] && strcmp(s_app_holder, slug) != 0) return;
    s_app_hold = false;
    s_app_until_ms = 0;
    s_app_holder[0] = '\0';
}

bool idle_dim_app_hold(void)
{
    return s_app_hold ||
           (s_app_until_ms != 0 &&
            (uint32_t)(s_app_until_ms - now_ms()) <
                (uint32_t)CONFIG_ORB_APP_POWER_MAX_S * 1000u);
}

const char *idle_dim_app_holder(void)
{
    return s_app_holder;
}

bool idle_dim_dimmed(void)
{
    return s_dimmed;
}

uint8_t idle_dim_user_level(void)
{
    return (uint8_t)s_user_pct;
}

uint32_t idle_dim_ms_since_touch(void)
{
    /* One subtraction of the same unsigned clock the task compares, so the
     * number on /api/v1/status and the decision to dim cannot disagree, and
     * cannot straddle a wrap differently either. */
    return (uint32_t)(now_ms() - s_last_ms);
}

#if CONFIG_ORB_IDLE_MOTION_ENABLE
uint32_t idle_dim_motion_wakes(void)
{
    return s_wakes;
}

int idle_dim_worst_mg(void)
{
    /* Safe in int: a sample is an int16 and the baseline a filtered average of
     * int16s, so the deviation is under 65535 and the x1000 stays inside 2^31. */
    return (int)((s_worst_lsb * 1000) / IMU_LSB_PER_G_8G);
}

int idle_dim_worst_dark_mg(void)
{
    return (int)((s_worst_dark_lsb * 1000) / IMU_LSB_PER_G_8G);
}
#else
uint32_t idle_dim_motion_wakes(void) { return 0; }
int      idle_dim_worst_mg(void)     { return 0; }
int      idle_dim_worst_dark_mg(void){ return 0; }
#endif

#else /* !CONFIG_ORB_IDLE_DIM_ENABLE */

esp_err_t idle_dim_init(void)                 { return ESP_OK; }
/* main's LVGL loop calls the drain unconditionally, so the disabled build must
 * answer it too: with no dim there is never a pending brightness write, and a
 * drain that does nothing is the honest no-op, not a link error. */
void    idle_dim_render_drain(void)            { }
void    idle_dim_activity(const char *why)    { (void)why; }
uint32_t idle_dim_activity_count(void)        { return 0; }
/* The status block reads it unconditionally, so the disabled build says so
 * rather than failing to link: no dim, no brightness write, no lost one. */
uint32_t idle_dim_level_write_fail_count(void) { return 0; }
void    idle_dim_set_user_level(uint8_t p)    { (void)p; }
bool    idle_dim_dimmed(void)                 { return false; }
/* The settings API answers too, rather than failing to link: the DISPLAY menu
 * is built whatever Kconfig says, and it reads these getters to paint its
 * rows. The setters refuse so the menu can say "not in this build" -- an
 * accepted-but-ignored switch would be the same lie as a dim that never
 * dims, one menu level up. */
bool    idle_dim_enabled(void)                { return false; }
esp_err_t idle_dim_set_enabled(bool on)       { (void)on; return ESP_ERR_NOT_SUPPORTED; }
uint32_t idle_dim_timeout_s(void)             { return 0; }
esp_err_t idle_dim_set_timeout_s(uint32_t s)  { (void)s; return ESP_ERR_NOT_SUPPORTED; }
/* The status block reads all four unconditionally, so the disabled build answers
 * rather than failing to link -- a dial with the dim compiled out is still a dial
 * that can report that it is not dimmed. */
uint32_t idle_dim_ms_since_touch(void)         { return 0; }
uint32_t idle_dim_motion_wakes(void)           { return 0; }
int      idle_dim_worst_mg(void)               { return 0; }
int      idle_dim_worst_dark_mg(void)          { return 0; }
/* 100, not 0: with the dim compiled out the panel keeps the 0xFF its own init
 * wrote, and a status page that said 0 would be reporting a black dial. */
uint8_t  idle_dim_user_level(void)              { return 100; }

#endif /* CONFIG_ORB_IDLE_DIM_ENABLE */
