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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_co5300.h"
#include "nvs.h"
#include "nvs_flash.h"   /* nvs_flash_init_partition: see settings_load */
#include "sdkconfig.h"

/* The two clocks, per the argument in the header. */
#define TICK_MS_LIGHT   1000
#define TICK_MS_DARK      50

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

#if CONFIG_ORB_IDLE_DIM_ENABLE

/* Inside the guard, not above it: every ESP_LOGx that uses it is compiled out
 * with the feature, and a file-scope TAG would then trip -Wunused-variable. */
static const char *TAG = "idle_dim";

static volatile uint32_t s_last_ms;           /* last touch, from any source     */
static volatile uint32_t s_user_pct = 100;    /* where the dial sits while awake */
static volatile bool     s_dimmed;
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

/* The brightness write. Only this task calls it, so the panel driver never sees
 * a brightness transaction racing the clock's flush from another context. */
static void apply_level(uint8_t percent, const char *why)
{
    esp_lcd_panel_handle_t panel = display_get_panel();
    if (!panel) {
        ESP_LOGW(TAG, "no panel yet: %s not applied", why);
        return;
    }
    const esp_err_t err = esp_lcd_panel_co5300_set_brightness(panel, percent);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "%s (%u%%) failed: %s", why, (unsigned)percent,
                 esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "%u%% -- %s", (unsigned)percent, why);
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
        vTaskDelay(per);
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
         * decision itself is the host-pinned idle_dim_elapsed_decide. */
        const bool want = idle_dim_elapsed_decide(now, s_last_ms,
                                                  s_timeout_ms, s_enabled);
        if (want != s_dimmed) {
            s_dimmed = want;
            if (want) {
                s_dim_at_ms = now;            /* stamp BEFORE the write: the delay
                                               * the wake log reports is measured
                                               * from the dim that caused it      */
#if CONFIG_ORB_IDLE_MOTION_ENABLE
                motion.have_base = false;   /* re-seed: the orb may have moved
                                              * while it was lit and awake        */
                /* Reset the run too: it accumulates on the lit tick as well, so
                 * without this the face could arrive in the dark already "fired"
                 * and the first genuine pick-up would be swallowed. */
                wake.run = 0;
                wake.fired = false;

#endif
                apply_level((uint8_t)CONFIG_ORB_IDLE_DIM_PCT, "idle dim");
            } else {
                apply_level((uint8_t)s_user_pct, "touch, back to the user's level");
            }
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
    if (xTaskCreate(idle_task, "idle_dim", 3072, NULL, 2, NULL) != pdPASS)
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
    /* Setting from the menu IS a touch, stamped here rather than leaning on
     * input_router's stamp of the detent that opened the row: enabling after a
     * long off-period would otherwise hand the task a stale s_last_ms and dim
     * the face the instant the switch flips, which reads as the switch
     * misfiring. And while dimmed, this stamp is what lifts the face on the
     * task's next dark tick (50 ms) -- the restore itself stays in the task,
     * the one writer of the brightness register per apply_level's rule. */
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
    return settings_persist(DIM_NVS_KEY_TO, s);
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
void    idle_dim_activity(const char *why)    { (void)why; }
uint32_t idle_dim_activity_count(void)        { return 0; }
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
