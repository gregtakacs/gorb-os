/**
 * orb_svc.c — the broker (see orb_svc.h for the contract; plan §4.2/§4.3).
 *
 * One static row per registry slot carries what the frozen ABI needs the OS to
 * remember about an app: its stored owner-grants, its asks, its call and
 * denial counts (the P3 pass line on /api/v1/status), its D16 beat, its
 * config byte charge, and the last line its status() printed. The row index
 * is orb_app_index_of(), so the table order — which IS boot order — is also
 * the persistence key, the same stability argument orb_app.h uses for
 * art_offset. P4 pins the slot in the catalog beside the app id.
 *
 * Free caps (CAP_FREE_MASK: time/config/ui/art) are never stored and never
 * revocable — orb_grant_ok() answers them before it ever looks at NVS, which
 * is §4.2's "always on" made structural. The owner-granted four live as one
 * u32 per app under orb.grants, defaulting to 0: nothing is implicit (B3),
 * and the first ask from an ungranted app sets a pending bit the dial's
 * grant prompt will read.
 *
 * Stubs are honest: every entry that awaits a later commit's wiring (time ->
 * the compose_now extraction, power -> the idle_dim request path, net -> P4,
 * layer_* -> P5, themes -> the D18 wrappers) sits BEHIND its gate, counts the
 * call, and answers ESP_ERR_NOT_SUPPORTED. A stub in place of the gate would
 * pass this file's own suite while shipping an app that can call below its
 * grant — the one failure mode the whole broker exists to prevent.
 */
#include "orb_svc.h"
#include "orb_app.h"
#include "orb_jobs.h"
#include "art_bank.h"
#include "orb_net.h"          /* orb_net_time_synced / tz_label / tz_offset */
#include "idle_dim.h"         /* the power request path — a request, not a write */

#include <math.h>             /* fabs, floor — the drift rule's own tools */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"        /* the anchor half of the time pair */
#include "nvs.h"
#include "nvs_flash.h"        /* nvs_flash_init_partition("storage") — the
                              * same idempotent call idle_dim.c makes before
                              * opening its namespace */

static const char *TAG = "orb_svc";

/* Registry slots, incl. the shell's chrome entries — which is why this is not
 * ORB_APP_CAP: B5 caps installed APPS at 8, and the compiled-in pair plus the
 * menu entry already live beside that budget, not inside it. */
#define ORB_SVC_ROWS 16

typedef struct {
    uint32_t grants;        /* stored owner-granted mask (orb.grants)      */
    uint32_t asked;         /* gated entries called while denied -> prompt */
    uint32_t calls, denials;/* lifetime totals, any entry, either outcome  */
    uint32_t beat_ms;       /* D16 rate in force; 0 = stopped              */
    uint32_t cfg_used;      /* bytes charged against ORB_CFG_QUOTA_BYTES   */
    bool     loaded;        /* grant word read from NVS                    */
    bool     cfg_loaded;    /* quota charge read from NVS                  */
    char     last_status[72];
} svc_row_t;

static svc_row_t s_row[ORB_SVC_ROWS];

#define GRANTS_NS "orb.grants"

/* One NVS round trip per row, per boot. Read-only and small; the boot ladder
 * and the first gated call are the only callers, neither on the frame path. */
static void row_load_grants(svc_row_t *r, int slot)
{
    r->loaded = true;
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", GRANTS_NS, NVS_READONLY, &h) != ESP_OK)
        return;                                  /* nothing stored: all off — B3 */
    char key[12];
    snprintf(key, sizeof key, "g%d", slot);
    uint32_t v = 0;
    if (nvs_get_u32(h, key, &v) == ESP_OK)
        r->grants = v & ~CAP_FREE_MASK;           /* free caps are not storable */
    nvs_close(h);
}

static svc_row_t *row_of(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    if (slot < 0 || slot >= ORB_SVC_ROWS) return NULL;   /* not in the table:
        * an app the registry does not know has no grants to check, and a
        * silent pass here is how a broker becomes a formality. */
    return &s_row[slot];
}

bool orb_grant_ok(const orb_app_t *a, orb_cap_t cap)
{
    svc_row_t *r = row_of(a);
    if (!r) {
        ESP_LOGE(TAG, "grant check for an app that is not in the registry");
        return false;
    }
    r->calls++;
    const uint32_t bit = 1u << cap;
    if (bit & CAP_FREE_MASK) return true;        /* §4.2: always on. Counted,
                                                  * because the pass line is
                                                  * the number, not the verdict */
    if (!r->loaded) row_load_grants(r, orb_app_index_of(a));
    if (r->grants & bit) return true;

    /* Denied — and the denial is made visible three ways at once: counted for
     * /api/v1/status, latched for the dial's prompt (B3, shown once per ask),
     * and logged once, because an app that retries a gated entry every tick
     * would otherwise bury every other line on the console. */
    if (!(r->asked & bit))
        ESP_LOGW(TAG, "\"%s\" called %s without the grant — prompt pending",
                 a->slug ? a->slug : "?", orb_svc_cap_name(cap));
    r->asked |= bit;
    r->denials++;
    return false;
}

/* ---- config: declared keys only, own namespace, byte quota (§4.2/C9) ------
 *
 * The three bounds are the grant: a key the manifest did not declare is as
 * much below-grant as an ungranted capability, because an app that could
 * write arbitrary NVS keys owns the device's settings whether or not it holds
 * a cap. So every entry resolves the id through app->settings BEFORE any NVS
 * handle opens, and the namespace is the slot's own — orb.app<slot> — which
 * makes another app's keys unreachable even to a bug in this file.
 *
 * These four entries may block on NVS and so belong on the worker, per the
 * header's threading note. The byte charge is kept in RAM and written beside
 * the values: one extra u32 ("_used") pays for the quota surviving the reboot
 * that the erase it is rationing might cause. */

static const orb_setting_desc_t *find_setting(const orb_app_t *a, const char *id,
                                              orb_set_type_t want)
{
    for (int i = 0; i < a->n_settings; i++) {
        const orb_setting_desc_t *s = &a->settings[i];
        if (strcmp(s->id, id) == 0)
            return (s->type == want) ? s : NULL;  /* right key, wrong door: the
                                                   * int/string entries are not
                                                   * interchangeable — C9's types
                                                   * are the contract */
    }
    return NULL;
}

static void row_load_cfg(const orb_app_t *a, svc_row_t *r, int slot)
{
    r->cfg_loaded = true;
    char ns[20];
    snprintf(ns, sizeof ns, "orb.app%d", slot);
    nvs_handle_t h;
    if (nvs_open_from_partition("storage", ns, NVS_READONLY, &h) != ESP_OK) {
        r->cfg_used = 0;
        return;
    }
    uint32_t v = 0;
    r->cfg_used = (nvs_get_u32(h, "_used", &v) == ESP_OK) ? v : 0;
    nvs_close(h);
}

/* Returns ESP_OK with the handle open RW, or the reason it is not. Caller
 * closes. Shared by all four entries so the namespace shape appears once. */
static esp_err_t cfg_open(const orb_app_t *a, int slot, nvs_handle_t *h)
{
    char ns[20];
    snprintf(ns, sizeof ns, "orb.app%d", slot);
    return nvs_open_from_partition("storage", ns, NVS_READWRITE, h);
}

static esp_err_t svc_cfg_get(orb_app_t *a, const char *id, int32_t *out)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_INT);
    if (!s) s = find_setting(a, id, ORB_SET_ENUM);
    if (!s) s = find_setting(a, id, ORB_SET_TOGGLE);
    if (!s) return ESP_ERR_NOT_FOUND;      /* undeclared, or declared STRING:
                                            * the type IS part of the grant */
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    uint32_t v = (uint32_t)s->def;         /* absent = the declared default,
                                            * which is why the default lives in
                                            * the manifest and not in the app:
                                            * first run has no key to read */
    (void)nvs_get_u32(h, id, &v);
    nvs_close(h);
    *out = (int32_t)v;
    return ESP_OK;
}

static esp_err_t svc_cfg_set(orb_app_t *a, const char *id, int32_t v)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_INT);
    if (!s) s = find_setting(a, id, ORB_SET_ENUM);
    if (!s) s = find_setting(a, id, ORB_SET_TOGGLE);
    if (!s) return ESP_ERR_NOT_FOUND;
    if (s->type == ORB_SET_TOGGLE && (v == 0 || v == 1)) {}      /* the two words */
    else if (s->type == ORB_SET_TOGGLE) return ESP_ERR_INVALID_ARG;
    else if (v < s->min || v > s->max)
        /* Out of range is the app's bug, said with the numbers: the manifest's
         * min/max are the contract the options page renders, and an app that
         * writes past them is about to read back something it did not write. */
        return ESP_ERR_INVALID_ARG;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    esp_err_t w = nvs_set_u32(h, id, (uint32_t)v);
    if (w == ESP_OK) w = nvs_commit(h);
    nvs_close(h);
    return w;
}

static esp_err_t svc_cfg_get_str(orb_app_t *a, const char *id, char *buf, size_t cap)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_STRING);
    if (!s) return ESP_ERR_NOT_FOUND;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    const esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;
    size_t n = cap;
    const esp_err_t g = nvs_get_str(h, id, buf, &n);
    nvs_close(h);
    if (g == ESP_ERR_NVS_NOT_FOUND)            /* first run: the declared default */
        return (snprintf(buf, cap, "%s", s->def_str) < 0) ? ESP_FAIL : ESP_OK;
    return g;                                   /* ESP_ERR_INVALID_SIZE when the
                                                 * caller's cap is under the stored
                                                 * length — the app's bug, named */
}

static esp_err_t svc_cfg_set_str(orb_app_t *a, const char *id, const char *v)
{
    SVC(a, CONFIG);
    const orb_setting_desc_t *s = find_setting(a, id, ORB_SET_STRING);
    if (!s) return ESP_ERR_NOT_FOUND;
    const size_t n = strlen(v);
    if (n >= sizeof s->def_str)                /* 31 + NUL, the manifest's own
                                                * width — no silent truncation:
                                                * a stored value must equal the
                                                * value the app wrote */
        return ESP_ERR_INVALID_SIZE;
    const int slot = orb_app_index_of(a);
    if (!s_row[slot].cfg_loaded) row_load_cfg(a, &s_row[slot], slot);
    nvs_handle_t h;
    esp_err_t e = cfg_open(a, slot, &h);
    if (e != ESP_OK) return e;

    /* The quota is exact against NVS's own accounting: ask the old value's
     * length (NULL buf returns the size it would have written, NUL included),
     * refund it, charge the new. Overwrite is therefore free when the value
     * shrinks and costs only the difference when it grows. */
    size_t old = 0;
    if (nvs_get_str(h, id, NULL, &old) != ESP_OK) old = 0;
    const uint32_t base = (old && s_row[slot].cfg_used >= old)
                        ? s_row[slot].cfg_used - (uint32_t)old : s_row[slot].cfg_used;
    if (base + n + 1 > ORB_CFG_QUOTA_BYTES) {
        nvs_close(h);
        /* The number goes in the log because the app cannot see it in the code
         * and the owner cannot see it on the dial until the quota page ships;
         * an unaccounted refusal is how "my setting vanished" becomes a bug
         * report against the wrong file. */
        ESP_LOGW(TAG, "\"%s\" setting \"%s\": %zu B over the %d B app quota",
                 a->slug ? a->slug : "?", id,
                 (size_t)(base + n + 1 - ORB_CFG_QUOTA_BYTES), ORB_CFG_QUOTA_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    e = nvs_set_str(h, id, v);
    if (e == ESP_OK) {
        s_row[slot].cfg_used = base + (uint32_t)n + 1;
        e = nvs_set_u32(h, "_used", s_row[slot].cfg_used);
        if (e == ESP_OK) e = nvs_commit(h);
        if (e != ESP_OK)                    /* flash refused the pair: roll the
                                             * RAM charge back so RAM and NVS
                                             * cannot disagree about the quota */
            s_row[slot].cfg_used = (old) ? base + (uint32_t)old : base;
    }
    nvs_close(h);
    return e;
}

/* ---- entries that await a later commit's wiring -----------------------------
 * Each sits behind its gate and says NOT_SUPPORTED. The gate and the counts
 * are the shipped part NOW; the bodies land with their commits:
 *   power_request               -> the idle_dim request path (this phase)
 *   net                         -> P4's allowlisted client
 *   layer_*                     -> P5's table-driven compositor face
 *   theme_*                     -> the D18 wrappers over the shipped orb_theme
 * A stub in place of the gate would pass this file's own suite while shipping
 * an app that can call below its grant — the one failure the broker exists to
 * prevent. The suite therefore checks the gate AND the refusal per stub.
 */

/* ---- the wall clock: compose_now's anchoring, moved verbatim (plan §4.3,
 * the P3 time commit). The clock reads it through the same door every app
 * will; what stayed in clock_view.c is dial policy — the zone conversion,
 * the hands, the complications. The SECOND and its fraction are the OS's, as
 * ruling D15 signed, and this block is the ONLY anchor pair in the firmware.
 *
 * Free-running time base: starts at a fixed epoch, ticks in wall-clock real
 * time until SNTP lands. time() is taken from that point on — see the
 * dual-source note on orb_time_now for which source wins when. */
#define CLOCK_EPOCH 1700000000  /* Nov 2023 baseline */

/* Wall-clock base the reading ticks from, and the esp_timer instant it was
 * sampled at. ONE pair, not two sources: tm_sec and the sub-second fraction
 * must come from the same clock, or the second hand steps backward once per
 * second by the phase difference between time()'s seconds and the esp_timer
 * sawtooth. Starts on the free-running baseline and is overwritten the first
 * time the system clock looks credible (see orb_time_now). */
static time_t s_epoch_base     = CLOCK_EPOCH;
static int64_t s_t0_us         = 0;
static bool   s_anchor_is_real = false;   /* has s_epoch_base ever come from time()? */
static bool   s_seeded         = false;   /* the first call stamps t0 — the role
                                           * clock_view_init's seed line played,
                                           * keeping the pair sampled together */
#define CLOCK_RESYNC_SLACK 2.0            /* s of drift that re-triggers a re-anchor */

void orb_time_now(time_t *sec, double *frac, bool *synced)
{
    /* Two time sources, one anchor pair (see s_epoch_base). Sampled HERE, on
     * the calling task, so time() and esp_timer_get_time() are taken
     * back-to-back and the pair is consistent; the SNTP callback deliberately
     * only sets a flag.
     *
     * The system clock is trusted when it is either freshly synced or already
     * past the dial's own baseline:
     *   - orb_net_time_synced(): SNTP stepped it during this boot.
     *   - time() >= CLOCK_EPOCH: IDF keeps the boot time in RTC slow memory,
     *     so an epoch set by the previous boot SURVIVES a warm reboot
     *     (measured: after POST /api/v1/reboot the epoch is already right
     *     ~1.5 s before the next SNTP reply lands). Trusting it is what stops
     *     the dial from sliding back to Nov-2023 for a few seconds every time
     *     the orb reboots.
     * Anything lower is the 1970 default after a power cut, so the dial keeps
     * free-running from CLOCK_EPOCH instead — a stale dial beats a 1970 dial,
     * and the hourly CONFIG_LWIP_SNTP_UPDATE_DELAY re-sync then only
     * re-anchors when the drift is big enough to be worth the visible jump. */
    const int64_t now_us = esp_timer_get_time();
    const time_t  real   = time(NULL);
    const bool    trust  = orb_net_time_synced();
    if (!s_seeded) { s_t0_us = now_us; s_seeded = true; }
    if (trust || real >= (time_t)CLOCK_EPOCH) {
        const double drift = (double)(real - s_epoch_base)
                           - (double)(now_us - s_t0_us) / 1e6;
        if (!s_anchor_is_real || fabs(drift) >= CLOCK_RESYNC_SLACK) {
            s_epoch_base     = real;
            s_t0_us          = now_us;
            s_anchor_is_real = true;
        }
    }

    const double  el   = (double)(now_us - s_t0_us) / 1e6;
    *sec  = s_epoch_base + (time_t)floor(el);
    *frac = el - floor(el);
    *synced = trust;
}

static void svc_now(orb_app_t *a, time_t *sec, double *frac, bool *synced)
{
    (void)orb_grant_ok(a, CAP_TIME);             /* counted; time is §4.2's
                                                  * "free, always on" — no app
                                                  * is useful without it */
    orb_time_now(sec, frac, synced);
}

static const char *svc_tz_label(orb_app_t *a)
{
    (void)orb_grant_ok(a, CAP_TIME);
    return orb_net_tz_current();                 /* the stored zone key ("UTC"
                                                  * when none) — a name out,
                                                  * never a struct tm (D15) */
}

static esp_err_t svc_tz_offset(orb_app_t *a, time_t when, int32_t *offset_min)
{
    SVC(a, TIME);
    return orb_net_tz_offset(when, offset_min);  /* D17: the owner's zone,
                                                  * measured at `when`, via
                                                  * orb_net's one computation */
}

static esp_err_t svc_power_request(orb_app_t *a, uint32_t timeout_s, bool hold)
{
    SVC(a, POWER);
    /* A request, not a write (§4.2:375): idle_dim stays the policy owner,
     * applies the Kconfig ceiling and the owner's DISPLAY truth, and the
     * registry revokes the ask when the app leaves the glass below.
     * (0, false) is the app withdrawing an earlier ask. */
    if (!hold && timeout_s == 0) {
        idle_dim_app_release(a->slug);
        return ESP_OK;
    }
    return idle_dim_app_request(a->slug, timeout_s, hold);
}

static esp_err_t svc_http_get(orb_app_t *a, const char *url, orb_buf_t *out)
{
    SVC(a, NET);
    (void)url; (void)out;
    return ESP_ERR_NOT_SUPPORTED;                /* P4 */
}

static const uint8_t *svc_bank(orb_app_t *a)
{
    (void)orb_grant_ok(a, CAP_ART);
    return art_bank_extra_ptr(a->art_slot);      /* the app's OWN bank — §4.2's
                                                  * "free for its own bank".
                                                  * slot -1 maps to no mapping,
                                                  * which reads as "none". */
}

static int svc_layer_add(orb_app_t *a, const orb_layer_desc_t *d)
{
    SVC(a, UI);
    (void)d;
    return -1;                                   /* P5's compositor face */
}

static void svc_layer_set(orb_app_t *a, int h, const orb_layer_desc_t *d)
{
    if (!orb_grant_ok(a, CAP_UI)) return;        /* a void entry cannot return
                                                  * the refusal; the denial is
                                                  * counted, and that is what
                                                  * the owner's status page
                                                  * sees */
    (void)h; (void)d;
}

static void svc_layer_touch(orb_app_t *a, int h, const struct _lv_area_t *area)
{
    if (!orb_grant_ok(a, CAP_UI)) return;
    (void)h; (void)area;
}

static void svc_layer_del(orb_app_t *a, int h)
{
    if (!orb_grant_ok(a, CAP_UI)) return;
    (void)h;
}

static esp_err_t svc_job_post(orb_app_t *a, orb_job_fn fn, void *arg, size_t arglen)
{
    SVC(a, UI);                                  /* free, but counted: the call
                                                  * number is what shows an app
                                                  * hammering the worker */
    return orb_jobs_post(fn, arg, arglen);       /* the worker's own busy gate
                                                  * answers for double-taps
                                                  * (orb_jobs.h) */
}

/* D16 (§0.6), the policy half — the RULE is here now because it is what makes
 * the manifest the cadence budget. The delivery half (resizing the shared
 * beat, measuring each tick against the declared budget, parking overrunners)
 * lands with the scheduler commit; the row carries the rate either way, and
 * orb_svc_beat_ms() is the seam the scheduler reads. */
static esp_err_t svc_set_beat(orb_app_t *a, uint32_t period_ms)
{
    SVC(a, UI);
    const int slot = orb_app_index_of(a);
    if (period_ms == 0) {                        /* stop until the next resume */
        s_row[slot].beat_ms = 0;
        return ESP_OK;
    }
    if (a->period_ms == 0)
        /* The app declared no beat at install: it gets none at runtime. An
         * app that wants frames asks for them where the owner can price them. */
        return ESP_ERR_NOT_SUPPORTED;
    if (period_ms < a->period_ms) {
        /* Refused with the number in the reason — the plan's wording, honoured
         * literally: without the log line, "the beat did not change" is the
         * only feedback an app ever gets. */
        ESP_LOGW(TAG, "\"%s\" asked for a %lu ms beat; its manifest floor is %lu ms",
                 a->slug ? a->slug : "?", (unsigned long)period_ms,
                 (unsigned long)a->period_ms);
        return ESP_ERR_INVALID_ARG;
    }
    s_row[slot].beat_ms = period_ms;
    return ESP_OK;
}

/* The D18 family: enumerate and request — never a path, never bytes. Gated
 * like every owner cap; the bodies become thin wrappers over the shipped
 * orb_theme_scan()/orb_theme_post_install() with the themes commit, where the
 * id goes through orb_theme_id_ok(). */
static int svc_theme_count(orb_app_t *a)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return -1; /* -1 = no list: the int can't
                                                  * carry a code; the counted
                                                  * denial does */
    return -1;                                   /* with the themes commit */
}

static esp_err_t svc_theme_id(orb_app_t *a, int idx, char *buf, size_t cap)
{
    SVC(a, THEMES);
    (void)idx; (void)buf; (void)cap;
    return ESP_ERR_NOT_SUPPORTED;
}

static bool svc_theme_active(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return false;
    (void)idx;
    return false;
}

static bool svc_theme_changed(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return false;
    (void)idx;
    return false;
}

static const char *svc_theme_note(orb_app_t *a, int idx)
{
    if (!orb_grant_ok(a, CAP_THEMES)) return NULL;
    (void)idx;
    return NULL;
}

static esp_err_t svc_theme_install(orb_app_t *a, const char *id)
{
    SVC(a, THEMES);
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

static void svc_status(orb_app_t *a, const char *fmt, ...)
{
    (void)orb_grant_ok(a, CAP_UI);
    const int slot = orb_app_index_of(a);
    if (slot < 0) return;                        /* not in the table: nothing
                                                  * to print to */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_row[slot].last_status, sizeof s_row[slot].last_status, fmt, ap);
    va_end(ap);
}

/* The frozen table. The order IS orb_svc_t's field order — the one place a
 * missing entry becomes a compile-visible shape rather than a NULL an app
 * discovers at runtime — and nothing below is reachable without going through
 * a gate in its own entry. */
static const orb_svc_t S_TABLE = {
    .now           = svc_now,
    .tz_label      = svc_tz_label,
    .tz_offset     = svc_tz_offset,
    .power_request = svc_power_request,
    .cfg_get       = svc_cfg_get,
    .cfg_set       = svc_cfg_set,
    .cfg_get_str   = svc_cfg_get_str,
    .cfg_set_str   = svc_cfg_set_str,
    .http_get      = svc_http_get,
    .bank          = svc_bank,
    .layer_add     = svc_layer_add,
    .layer_set     = svc_layer_set,
    .layer_touch   = svc_layer_touch,
    .layer_del     = svc_layer_del,
    .job_post      = svc_job_post,
    .set_beat      = svc_set_beat,
    .theme_count   = svc_theme_count,
    .theme_id      = svc_theme_id,
    .theme_active  = svc_theme_active,
    .theme_changed = svc_theme_changed,
    .theme_note    = svc_theme_note,
    .theme_install = svc_theme_install,
    .status        = svc_status,
};

const orb_svc_t *orb_svc_table(void) { return &S_TABLE; }

const char *orb_svc_cap_name(orb_cap_t cap)
{
    static const char *const N[CAP_N] = {
        "time", "config", "ui", "art", "power", "net", "sd_read", "themes"
    };
    return (unsigned)cap < CAP_N ? N[cap] : "?";
}

/* ---- the OS side -------------------------------------------------------------
 * The grant word is the only app state NVS holds outside the app's own
 * namespace, and its only writer is the owner's answer to the dial's prompt
 * (B3) — never an app, never a default. An absent key means every owner cap
 * OFF: "nothing is implicit" spelled as an absence, not as a zero somebody
 * has to remember to write at install. */
static esp_err_t store_grants(int slot, uint32_t mask)
{
    nvs_handle_t h;
    const esp_err_t e = nvs_open_from_partition("storage", GRANTS_NS,
                                                NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    char key[12];
    snprintf(key, sizeof key, "g%d", slot);
    esp_err_t w = nvs_set_u32(h, key, mask);
    if (w == ESP_OK) w = nvs_commit(h);
    nvs_close(h);
    return w;
}

esp_err_t orb_svc_init(void)
{
    /* Preload every row so the first gated call of the boot never pays an NVS
     * read on the frame path, and so /api/v1/status has real numbers before
     * anybody asks. A storage partition that will not open is not fatal: no
     * grants stored is the same answer as all owner caps off — exactly the
     * safe direction — and the log line says which of the two it is. */
    if (nvs_flash_init_partition("storage") != ESP_OK)
        ESP_LOGW(TAG, "storage partition not up — every owner cap reads OFF");
    for (int i = 0; i < ORB_SVC_ROWS; i++)
        row_load_grants(&s_row[i], i);
    /* The line is the P3 pass line's boot-half: it names the table so a
     * glance at serial says the broker shipped, and taking its address is
     * what stops the linker collecting an unreferenced S_TABLE out of the
     * image before P4's loader (and the status endpoint) wire consumers to
     * it — a broker that boots invisibly and a broker that is gone read the
     * same from the dial. */
    ESP_LOGI(TAG, "broker up: table %p, %u entries, %u app rows",
             (const void *)orb_svc_table(),
             (unsigned)(sizeof S_TABLE / sizeof(void *)),
             (unsigned)ORB_SVC_ROWS);
    return ESP_OK;
}

uint32_t orb_svc_grants(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].grants : 0;
}

uint32_t orb_svc_pending(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].asked : 0;
}

void orb_svc_prompt_shown(const orb_app_t *a, uint32_t caps)
{
    const int slot = orb_app_index_of(a);
    if (slot >= 0) s_row[slot].asked &= ~caps;
}

esp_err_t orb_svc_set_grant(orb_app_t *a, orb_cap_t cap, bool on)
{
    if ((1u << cap) & CAP_FREE_MASK)
        return ESP_ERR_INVALID_ARG;              /* free caps are not the
                                                  * owner's to give away, and
                                                  * revoking one would strand
                                                  * every app on the dial */
    const int slot = orb_app_index_of(a);
    if (slot < 0) return ESP_ERR_INVALID_ARG;
    if (!s_row[slot].loaded) row_load_grants(&s_row[slot], slot);
    const uint32_t want = on ? (s_row[slot].grants | (1u << cap))
                             : (s_row[slot].grants & ~(1u << cap));
    /* Persist first, cache second: a refusal must leave the RAM answer
     * matching the flash, or the dial shows a grant the next boot undoes. */
    const esp_err_t e = store_grants(slot, want);
    if (e == ESP_OK) s_row[slot].grants = want;
    return e;
}

uint32_t orb_svc_calls(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].calls : 0;
}

uint32_t orb_svc_denials(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].denials : 0;
}

const char *orb_svc_last_status(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].last_status : "";
}

uint32_t orb_svc_beat_ms(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].beat_ms : 0;
}

uint32_t orb_svc_cfg_used(const orb_app_t *a)
{
    const int slot = orb_app_index_of(a);
    return (slot >= 0) ? s_row[slot].cfg_used : 0;
}

/* The registry calls this where it suspends the outgoing entry
 * (orb_app.c): §4.2's "auto-revoked when the app is not visible", executed
 * by the one place that knows visibility rather than trusted to the app.
 * Only the power ask is revoked — the beat stops with the tick delivery,
 * and the config bytes are the app's to keep. */
void orb_svc_app_suspended(const orb_app_t *a)
{
    if (orb_app_index_of(a) < 0) return;      /* not a registered entry */
    idle_dim_app_release(a->slug);
}